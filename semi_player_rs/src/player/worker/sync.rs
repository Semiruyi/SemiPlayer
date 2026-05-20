use std::sync::{Arc, Condvar, Mutex};
use std::thread::{self, JoinHandle};
use std::time::Duration;

use crate::api::types::PlayerState;
use crate::player::access::SyncWorkerPlanContext;
use crate::player::execution::{
    execute_playback_plan, finish_playback_advance, plan_playback_advance,
};
use crate::player::handle::SemiPlayerHandle;
use crate::scheduler::types::SchedulerEvent;
use crate::util::debug_trace::append_trace_line;
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
        append_trace_line("sync:stop requested");
        let (lock, condvar) = &*self.control;
        {
            let mut control = lock.lock().unwrap();
            control.shutdown = true;
            control.wake_requested = true;
        }
        condvar.notify_all();

        if let Some(thread) = self.thread.take() {
            append_trace_line("sync:joining");
            let _ = thread.join();
            append_trace_line("sync:joined");
        }
    }
}

#[allow(clippy::needless_pass_by_value)]
fn worker_loop(player_addr: usize, control: Arc<(Mutex<SyncWorkerControl>, Condvar)>) {
    loop {
        if shutdown_requested(&control) {
            append_trace_line("sync:loop exit shutdown_requested");
            break;
        }

        let action = unsafe {
            let player_ptr = player_addr as *mut SemiPlayerHandle;
            evaluate_worker_action(&*player_ptr)
        };

        match action {
            WorkerAction::AdvancePlayback { phase_lock } => {
                let _phase_guard = phase_lock.lock().unwrap();
                let plan = unsafe {
                    let player_ptr = player_addr as *mut SemiPlayerHandle;
                    let player = &*player_ptr;
                    if player.control_access().is_media_loaded() {
                        Some(plan_playback_advance(player))
                    } else {
                        None
                    }
                };

                let Some(plan) = plan else {
                    continue;
                };

                let result = execute_playback_plan(&plan);
                unsafe {
                    let player_ptr = player_addr as *mut SemiPlayerHandle;
                    finish_playback_advance(&*player_ptr, plan, result);
                }
            }
            WorkerAction::WaitFor(duration) => {
                if wait_for_signal(&control, Some(duration)) {
                    append_trace_line("sync:loop exit wait_for_signal timeout");
                    break;
                }
            }
            WorkerAction::WaitIndefinitely => {
                if wait_for_signal(&control, None) {
                    append_trace_line("sync:loop exit wait_for_signal");
                    break;
                }
            }
        }
    }
}

fn shutdown_requested(control: &Arc<(Mutex<SyncWorkerControl>, Condvar)>) -> bool {
    control.0.lock().unwrap().shutdown
}

fn evaluate_worker_action(player: &SemiPlayerHandle) -> WorkerAction {
    let context = player.sync_worker_plan_context();
    if !context.media_loaded {
        return WorkerAction::WaitIndefinitely;
    }

    let action = match context.state {
        PlayerState::Playing => execute_worker_step(player, context.clone(), WorkerMode::Playing),
        PlayerState::Ready | PlayerState::Paused => {
            execute_worker_step(player, context.clone(), WorkerMode::Stabilizing)
        }
        PlayerState::Idle => WorkerAction::WaitIndefinitely,
    };
    append_trace_line(&format!(
        "sync:plan state={:?} hint={:?} action={:?} snapshot={:?}",
        context.state,
        context.schedule_hint,
        action,
        player.worker_state_trace_snapshot()
    ));
    action
}

fn execute_worker_step(
    player: &SemiPlayerHandle,
    context: SyncWorkerPlanContext,
    mode: WorkerMode,
) -> WorkerAction {
    let hint = context.schedule_hint;
    if mode == WorkerMode::Stabilizing && !hint.playback_due_now && !hint.playback_supply_needed {
        return WorkerAction::WaitIndefinitely;
    }

    let scheduled_work = hint.scheduled_work();
    if scheduled_work.should_request_render {
        player.dispatch_scheduler_event(SchedulerEvent::PlaybackDemandChanged);
    }

    if scheduled_work.should_advance_playback {
        observe_worker_deadline_slip(player, scheduled_work.deadline_us);
        return WorkerAction::AdvancePlayback {
            phase_lock: context.phase_lock,
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

    let playback_time_us = player.playback_position_us_snapshot();
    let slip_us = playback_time_us.saturating_sub(deadline_us).max(0);
    player.observe_worker_deadline_slip(slip_us);
}

enum WorkerAction {
    AdvancePlayback { phase_lock: Arc<Mutex<()>> },
    WaitFor(Duration),
    WaitIndefinitely,
}

impl std::fmt::Debug for WorkerAction {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::AdvancePlayback { .. } => f.write_str("AdvancePlayback"),
            Self::WaitFor(duration) => f.debug_tuple("WaitFor").field(duration).finish(),
            Self::WaitIndefinitely => f.write_str("WaitIndefinitely"),
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum WorkerMode {
    Playing,
    Stabilizing,
}
