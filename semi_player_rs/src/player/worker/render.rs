use std::sync::{Arc, Condvar, Mutex};
use std::thread::{self, JoinHandle};

use crate::api::types::PlayerState;
use crate::player::access::RenderWorkerPlanContext;
use crate::player::execution::render_supply;
use crate::player::handle::SemiPlayerHandle;
use crate::util::debug_trace::append_trace_line;

#[derive(Default)]
struct RenderWorkerControl {
    shutdown: bool,
    wake_requested: bool,
    render_requested: bool,
}

pub struct RenderWorkerHandle {
    control: Arc<(Mutex<RenderWorkerControl>, Condvar)>,
    thread: Option<JoinHandle<()>>,
}

impl RenderWorkerHandle {
    pub fn start(player_ptr: *mut SemiPlayerHandle) -> Self {
        let control = Arc::new((Mutex::new(RenderWorkerControl::default()), Condvar::new()));
        let thread_control = Arc::clone(&control);
        let player_addr = player_ptr as usize;

        let thread = thread::Builder::new()
            .name("semi-render-worker".to_string())
            .spawn(move || worker_loop(player_addr, thread_control))
            .expect("failed to start render worker");

        Self {
            control,
            thread: Some(thread),
        }
    }

    pub fn request_render(&self) {
        let (lock, condvar) = &*self.control;
        let mut control = lock.lock().unwrap();
        control.render_requested = true;
        control.wake_requested = true;
        condvar.notify_all();
    }

    pub fn stop(&mut self) {
        append_trace_line("render:stop requested");
        let (lock, condvar) = &*self.control;
        {
            let mut control = lock.lock().unwrap();
            control.shutdown = true;
            control.wake_requested = true;
        }
        condvar.notify_all();

        if let Some(thread) = self.thread.take() {
            append_trace_line("render:joining");
            let _ = thread.join();
            append_trace_line("render:joined");
        }
    }
}

#[allow(clippy::needless_pass_by_value)]
fn worker_loop(player_addr: usize, control: Arc<(Mutex<RenderWorkerControl>, Condvar)>) {
    loop {
        if shutdown_requested(&control) {
            append_trace_line("render:loop exit shutdown_requested");
            break;
        }

        if wait_for_signal(&control) {
            append_trace_line("render:loop exit wait_for_signal");
            break;
        }

        let action = unsafe {
            let player_ptr = player_addr as *mut SemiPlayerHandle;
            evaluate_worker_action(&*player_ptr)
        };

        match action {
            RenderWorkerAction::RenderNow => {
                let render_result = unsafe {
                    let player_ptr = player_addr as *mut SemiPlayerHandle;
                    render_supply(&*player_ptr)
                };

                if render_result.has_new_presentation_frames() {
                    unsafe {
                        let player_ptr = player_addr as *mut SemiPlayerHandle;
                        (&*player_ptr).notify_sync_worker();
                    }
                }
            }
            RenderWorkerAction::RequestDecode => unsafe {
                let player_ptr = player_addr as *mut SemiPlayerHandle;
                (&*player_ptr).notify_decode_worker();
            },
            RenderWorkerAction::Wait => {}
        }
    }
}

fn evaluate_worker_action(player: &SemiPlayerHandle) -> RenderWorkerAction {
    let context = player.render_worker_plan_context();
    render_action_from_context(context)
}

fn render_action_from_context(context: RenderWorkerPlanContext) -> RenderWorkerAction {
    if !context.media_loaded || context.state == PlayerState::Idle {
        return RenderWorkerAction::Wait;
    }

    let supply = context.decode_supply;

    if supply.decoded_video_queue_len > 0 && !supply.has_sufficient_presentation_buffer {
        return RenderWorkerAction::RenderNow;
    }

    if supply.needs_decode_supply
        && supply.decoded_video_queue_len == 0
        && !supply.has_in_flight_render_batch()
    {
        return RenderWorkerAction::RequestDecode;
    }

    if supply.decoded_video_queue_len > 0 {
        return RenderWorkerAction::RenderNow;
    }

    RenderWorkerAction::Wait
}

enum RenderWorkerAction {
    RenderNow,
    RequestDecode,
    Wait,
}

trait RenderDecodeSupplyExt {
    fn has_in_flight_render_batch(self) -> bool;
}

impl RenderDecodeSupplyExt for crate::player::runtime::DecodeSupplyStatus {
    fn has_in_flight_render_batch(self) -> bool {
        self.in_flight_decoded_video_queue_len > 0
    }
}

#[cfg(test)]
mod tests {
    use super::{render_action_from_context, RenderWorkerAction};
    use crate::api::types::PlayerState;
    use crate::player::access::RenderWorkerPlanContext;
    use crate::player::runtime::DecodeSupplyStatus;

    #[test]
    fn render_worker_requests_decode_when_decoded_supply_is_empty() {
        let action = render_action_from_context(RenderWorkerPlanContext {
            media_loaded: true,
            state: PlayerState::Playing,
            decode_supply: DecodeSupplyStatus {
                needs_decode_supply: true,
                decoded_video_queue_len: 0,
                in_flight_decoded_video_queue_len: 0,
                ..DecodeSupplyStatus::default()
            },
        });

        assert!(matches!(action, RenderWorkerAction::RequestDecode));
    }

    #[test]
    fn render_worker_renders_when_decoded_backlog_exists() {
        let action = render_action_from_context(RenderWorkerPlanContext {
            media_loaded: true,
            state: PlayerState::Playing,
            decode_supply: DecodeSupplyStatus {
                decoded_video_queue_len: 2,
                ..DecodeSupplyStatus::default()
            },
        });

        assert!(matches!(action, RenderWorkerAction::RenderNow));
    }

    #[test]
    fn render_worker_waits_without_media() {
        let action = render_action_from_context(RenderWorkerPlanContext {
            media_loaded: false,
            state: PlayerState::Playing,
            decode_supply: DecodeSupplyStatus::default(),
        });

        assert!(matches!(action, RenderWorkerAction::Wait));
    }
}

fn shutdown_requested(control: &Arc<(Mutex<RenderWorkerControl>, Condvar)>) -> bool {
    control.0.lock().unwrap().shutdown
}

fn wait_for_signal(control: &Arc<(Mutex<RenderWorkerControl>, Condvar)>) -> bool {
    let (lock, condvar) = &**control;
    let mut state = lock.lock().unwrap();

    if state.shutdown {
        return true;
    }

    if !state.render_requested {
        loop {
            state = condvar.wait(state).unwrap();
            if state.shutdown {
                return true;
            }
            if state.render_requested || state.wake_requested {
                break;
            }
        }
    }

    if state.wake_requested {
        state.wake_requested = false;
    }

    state.render_requested = false;
    state.wake_requested = false;
    false
}
