use std::sync::{Arc, Condvar, Mutex};
use std::thread::{self, JoinHandle};

use crate::api::types::PlayerState;
use crate::core::player::execution::decode_supply;
use crate::core::player::handle::{LockOwner, SemiPlayerHandle};

#[derive(Default)]
struct DecodeWorkerControl {
    shutdown: bool,
    wake_requested: bool,
}

pub struct DecodeWorkerHandle {
    control: Arc<(Mutex<DecodeWorkerControl>, Condvar)>,
    thread: Option<JoinHandle<()>>,
}

impl DecodeWorkerHandle {
    pub fn start(player_ptr: *mut SemiPlayerHandle) -> Self {
        let control = Arc::new((Mutex::new(DecodeWorkerControl::default()), Condvar::new()));
        let thread_control = Arc::clone(&control);
        let player_addr = player_ptr as usize;

        let thread = thread::Builder::new()
            .name("semi-decode-worker".to_string())
            .spawn(move || worker_loop(player_addr, thread_control))
            .expect("failed to start decode worker");

        Self {
            control,
            thread: Some(thread),
        }
    }

    pub fn notify(&self) {
        let (lock, condvar) = &*self.control;
        let mut control = lock.lock().unwrap();
        control.wake_requested = true;
        condvar.notify_all();
    }

    pub fn stop(&mut self) {
        let (lock, condvar) = &*self.control;
        {
            let mut control = lock.lock().unwrap();
            control.shutdown = true;
            control.wake_requested = true;
        }
        condvar.notify_all();

        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}

fn worker_loop(player_addr: usize, control: Arc<(Mutex<DecodeWorkerControl>, Condvar)>) {
    loop {
        let action = unsafe {
            let player_ptr = player_addr as *mut SemiPlayerHandle;
            SemiPlayerHandle::with_locked_ptr_as(player_ptr, LockOwner::Worker, |player| {
                evaluate_decode_action(player)
            })
        };

        match action {
            DecodeWorkerAction::ContinueSoon => continue,
            DecodeWorkerAction::WaitIndefinitely => {
                if wait_for_signal(&control) {
                    break;
                }
            }
        }
    }
}

fn evaluate_decode_action(player: &mut SemiPlayerHandle) -> DecodeWorkerAction {
    if !player.is_media_loaded() {
        return DecodeWorkerAction::WaitIndefinitely;
    }

    match player.state() {
        PlayerState::Idle => DecodeWorkerAction::WaitIndefinitely,
        PlayerState::Ready | PlayerState::Paused | PlayerState::Playing => {
            if !player.runtime.decode_supply_status().needs_decode_supply {
                return DecodeWorkerAction::WaitIndefinitely;
            }

            let _ = decode_supply(player, 0);
            player.notify_sync_worker();

            if player.runtime.decode_supply_status().needs_decode_supply {
                DecodeWorkerAction::ContinueSoon
            } else {
                DecodeWorkerAction::WaitIndefinitely
            }
        }
    }
}

fn wait_for_signal(control: &Arc<(Mutex<DecodeWorkerControl>, Condvar)>) -> bool {
    let (lock, condvar) = &**control;
    let mut state = lock.lock().unwrap();

    if state.shutdown {
        return true;
    }

    if state.wake_requested {
        state.wake_requested = false;
        return false;
    }

    state = condvar.wait(state).unwrap();

    if state.shutdown {
        return true;
    }

    state.wake_requested = false;
    false
}

enum DecodeWorkerAction {
    ContinueSoon,
    WaitIndefinitely,
}
