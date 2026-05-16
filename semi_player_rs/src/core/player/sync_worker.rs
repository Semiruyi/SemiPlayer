use std::sync::{Arc, Condvar, Mutex};
use std::thread::{self, JoinHandle};
use std::time::Duration;

use crate::api::types::PlayerState;
use crate::core::player::execution::{
    execute_playback_plan, finish_playback_advance, plan_playback_advance,
};
use crate::core::player::handle::{LockOwner, SemiPlayerHandle};
use crate::core::player::schedule::PlayerScheduleService;
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
            SemiPlayerHandle::with_locked_ptr_as(
                player_ptr,
                LockOwner::SyncWorker,
                evaluate_worker_action,
            )
        };

        match action {
            WorkerAction::AdvancePlayback { phase_lock } => {
                let _phase_guard = phase_lock.lock().unwrap();
                let plan = unsafe {
                    let player_ptr = player_addr as *mut SemiPlayerHandle;
                    SemiPlayerHandle::with_locked_ptr_as(
                        player_ptr,
                        LockOwner::SyncWorker,
                        |player| {
                            if !player.is_media_loaded() {
                                None
                            } else {
                                Some(plan_playback_advance(player))
                            }
                        },
                    )
                };

                let Some(plan) = plan else {
                    continue;
                };

                let result = execute_playback_plan(&plan);
                unsafe {
                    let player_ptr = player_addr as *mut SemiPlayerHandle;
                    SemiPlayerHandle::with_locked_ptr_as(
                        player_ptr,
                        LockOwner::SyncWorker,
                        |player| {
                            finish_playback_advance(player, plan, result);
                        },
                    );
                }
                continue;
            }
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

fn evaluate_worker_action(player: &mut SemiPlayerHandle) -> WorkerAction {
    if !player.is_media_loaded() {
        return WorkerAction::WaitIndefinitely;
    }

    match player.state() {
        PlayerState::Playing => execute_worker_step(player, WorkerMode::Playing),
        PlayerState::Ready | PlayerState::Paused => {
            execute_worker_step(player, WorkerMode::Stabilizing)
        }
        PlayerState::Idle => WorkerAction::WaitIndefinitely,
    }
}

fn execute_worker_step(player: &mut SemiPlayerHandle, mode: WorkerMode) -> WorkerAction {
    let hint = PlayerScheduleService::evaluate(player);
    if mode == WorkerMode::Stabilizing && !hint.playback_due_now && !hint.decode_supply_needed {
        return WorkerAction::WaitIndefinitely;
    }

    let scheduled_work = hint.scheduled_work();
    if scheduled_work.should_request_decode {
        player.notify_decode_worker();
    }

    if scheduled_work.should_advance_playback {
        observe_worker_deadline_slip(player, scheduled_work.deadline_us);
        return WorkerAction::AdvancePlayback {
            phase_lock: player.playback_phase_lock(),
        };
    }

    match mode {
        WorkerMode::Playing => WorkerAction::WaitFor(Duration::from_micros(
            u64::try_from(scheduled_work.wait_us.max(1)).unwrap_or(u64::MAX),
        )),
        WorkerMode::Stabilizing => WorkerAction::WaitIndefinitely,
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
    AdvancePlayback { phase_lock: Arc<Mutex<()>> },
    WaitFor(Duration),
    WaitIndefinitely,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum WorkerMode {
    Playing,
    Stabilizing,
}
