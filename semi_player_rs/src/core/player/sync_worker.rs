use std::sync::{Arc, Condvar, Mutex};
use std::thread::{self, JoinHandle};
use std::time::Duration;

use crate::api::types::PlayerState;
use crate::core::player::execution::execute_scheduled_work;
use crate::core::player::handle::{LockOwner, SemiPlayerHandle};
use crate::core::player::schedule::{PlayerScheduleService, ScheduledWork};
use crate::util::time::MediaTimeUs;

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
            SemiPlayerHandle::with_locked_ptr_as(player_ptr, LockOwner::Worker, |player| {
                if !player.is_media_loaded() || player.state() != PlayerState::Playing {
                    return WorkerAction::WaitIndefinitely;
                }

                execute_worker_step(player)
            })
        };

        match action {
            WorkerAction::ContinueSoon => continue,
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

fn execute_worker_step(player: &mut SemiPlayerHandle) -> WorkerAction {
    let hint = PlayerScheduleService::evaluate(player);
    let scheduled_work = hint.scheduled_work();
    let deadline_us = scheduled_work.deadline_us();

    match scheduled_work {
        ScheduledWork::AdvanceAndDecode { .. } | ScheduledWork::AdvancePlayback { .. } => {
            observe_worker_deadline_slip(player, deadline_us);
            let _ = execute_scheduled_work(player, scheduled_work, 0);
            WorkerAction::ContinueSoon
        }
        ScheduledWork::DecodeSupply => {
            let _ = execute_scheduled_work(player, scheduled_work, 0);
            WorkerAction::ContinueSoon
        }
        ScheduledWork::WaitFor { wait_us } => WorkerAction::WaitFor(Duration::from_micros(
            u64::try_from(wait_us.max(1)).unwrap_or(u64::MAX),
        )),
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

fn observe_worker_deadline_slip(player: &SemiPlayerHandle, deadline_us: Option<MediaTimeUs>) {
    let Some(deadline_us) = deadline_us else {
        return;
    };

    let playback_time_us = player.audio_clock.presentation_time_us();
    let slip_us = playback_time_us.saturating_sub(deadline_us).max(0);
    player.observe_worker_deadline_slip(slip_us);
}

enum WorkerAction {
    ContinueSoon,
    WaitFor(Duration),
    WaitIndefinitely,
}
