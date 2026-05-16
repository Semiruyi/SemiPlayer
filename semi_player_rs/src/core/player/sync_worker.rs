use std::sync::{Arc, Condvar, Mutex};
use std::thread::{self, JoinHandle};
use std::time::Duration;

use crate::api::types::PlayerState;
use crate::core::player::handle::SemiPlayerHandle;
use crate::core::player::pump::pump_player;
use crate::core::player::schedule::PlayerScheduleService;

#[derive(Default)]
struct SyncWorkerControl {
    shutdown: bool,
    wake_requested: bool,
}

pub struct SyncWorkerHandle {
    control: Arc<(Mutex<SyncWorkerControl>, Condvar)>,
    thread: Option<JoinHandle<()>>,
}

impl SyncWorkerHandle {
    pub fn start(player_ptr: *mut SemiPlayerHandle) -> Self {
        let control = Arc::new((Mutex::new(SyncWorkerControl::default()), Condvar::new()));
        let thread_control = Arc::clone(&control);
        let player_addr = player_ptr as usize;

        let thread = thread::Builder::new()
            .name("semi-sync-worker".to_string())
            .spawn(move || worker_loop(player_addr, thread_control))
            .expect("failed to start sync worker");

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

fn worker_loop(player_addr: usize, control: Arc<(Mutex<SyncWorkerControl>, Condvar)>) {
    loop {
        let action = unsafe {
            let player_ptr = player_addr as *mut SemiPlayerHandle;
            SemiPlayerHandle::with_locked_ptr(player_ptr, |player| {
                if !player.is_media_loaded() || player.state() != PlayerState::Playing {
                    return WorkerAction::WaitIndefinitely;
                }

                let hint = PlayerScheduleService::evaluate(player);
                if hint
                    .next_pump_deadline_us
                    .is_some_and(|deadline_us| deadline_us <= hint.playback_time_us)
                {
                    WorkerAction::PumpNow
                } else {
                    WorkerAction::WaitFor(Duration::from_micros(
                        u64::try_from(hint.suggested_wait_us.max(1)).unwrap_or(u64::MAX),
                    ))
                }
            })
        };

        match action {
            WorkerAction::PumpNow => unsafe {
                let player_ptr = player_addr as *mut SemiPlayerHandle;
                let _ = SemiPlayerHandle::with_locked_ptr(player_ptr, |player| pump_player(player, 0));
            },
            WorkerAction::WaitFor(duration) => {
                if wait_for_signal(&control, Some(duration)) {
                    break;
                }
            }
            WorkerAction::WaitIndefinitely => {
                if wait_for_signal(&control, None) {
                    break;
                }
            }
        }
    }
}

fn wait_for_signal(
    control: &Arc<(Mutex<SyncWorkerControl>, Condvar)>,
    timeout: Option<Duration>,
) -> bool {
    let (lock, condvar) = &**control;
    let mut state = lock.lock().unwrap();

    if state.shutdown {
        return true;
    }

    if state.wake_requested {
        state.wake_requested = false;
        return false;
    }

    match timeout {
        Some(duration) => {
            let (next_state, _) = condvar.wait_timeout(state, duration).unwrap();
            state = next_state;
        }
        None => {
            state = condvar.wait(state).unwrap();
        }
    }

    if state.shutdown {
        return true;
    }

    state.wake_requested = false;
    false
}

enum WorkerAction {
    PumpNow,
    WaitFor(Duration),
    WaitIndefinitely,
}
