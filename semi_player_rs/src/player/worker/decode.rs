use std::sync::{Arc, Condvar, Mutex};
use std::thread::{self, JoinHandle};

use crate::decode::session::SharedMediaSession;
use crate::decode::{DecodePolicy, DecodedOutputPoll};
use crate::player::access::DecodePlanContext;
use crate::player::execution::{apply_decoded_output, poll_decoded_output_once};
use crate::player::handle::SemiPlayerHandle;
use crate::sync::schedule::PlayerScheduleService;

#[derive(Default)]
struct DecodeWorkerControl {
    shutdown: bool,
    wake_requested: bool,
    decode_requested: bool,
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

    pub fn request_decode(&self) {
        let (lock, condvar) = &*self.control;
        let mut control = lock.lock().unwrap();
        control.decode_requested = true;
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

#[allow(clippy::needless_pass_by_value)]
fn worker_loop(player_addr: usize, control: Arc<(Mutex<DecodeWorkerControl>, Condvar)>) {
    loop {
        let plan = unsafe {
            let player_ptr = player_addr as *mut SemiPlayerHandle;
            plan_decode_action(&*player_ptr)
        };

        match plan {
            DecodeWorkerPlan::Decode {
                opened_media,
                generation,
                decode_policy,
            } => {
                let polled_output = poll_decoded_output_once(&opened_media, decode_policy);
                let action = unsafe {
                    let player_ptr = player_addr as *mut SemiPlayerHandle;
                    complete_decode_action(&*player_ptr, generation, polled_output)
                };

                match action {
                    DecodeWorkerAction::ContinueSoon => {}
                    DecodeWorkerAction::WaitIndefinitely => {
                        if wait_for_signal(&control) {
                            break;
                        }
                    }
                }
            }
            DecodeWorkerPlan::WaitIndefinitely => {
                if wait_for_signal(&control) {
                    break;
                }
            }
        }
    }
}

fn plan_decode_action(player: &SemiPlayerHandle) -> DecodeWorkerPlan {
    let context = player.decode_plan_context();
    let hint = PlayerScheduleService::evaluate_decode_from_inputs(player.decode_schedule_inputs());
    plan_decode_action_from_context(context, hint)
}

fn plan_decode_action_from_context(
    context: DecodePlanContext,
    hint: crate::sync::schedule::DecodeScheduleHint,
) -> DecodeWorkerPlan {
    if !hint.worker_active {
        return DecodeWorkerPlan::WaitIndefinitely;
    }

    if !hint.should_decode_now {
        return DecodeWorkerPlan::WaitIndefinitely;
    }

    let Some(opened_media) = context.opened_media else {
        return DecodeWorkerPlan::WaitIndefinitely;
    };

    DecodeWorkerPlan::Decode {
        opened_media,
        generation: context.generation,
        decode_policy: context.decode_policy,
    }
}

fn complete_decode_action(
    player: &SemiPlayerHandle,
    generation: u64,
    polled_output: Result<DecodedOutputPoll, i32>,
) -> DecodeWorkerAction {
    if generation != player.media_generation() {
        return next_decode_action(player);
    }

    match polled_output {
        Ok(DecodedOutputPoll::Output(output)) => {
            let apply_result = apply_decoded_output(player, output);
            if apply_result.should_wake_sync {
                player.notify_sync_worker();
            }
            if apply_result.reached_end {
                return DecodeWorkerAction::WaitIndefinitely;
            }
        }
        Ok(DecodedOutputPoll::Pending | DecodedOutputPoll::Finished) => {}
        Err(_) => return DecodeWorkerAction::WaitIndefinitely,
    }

    next_decode_action(player)
}

fn next_decode_action(player: &SemiPlayerHandle) -> DecodeWorkerAction {
    if PlayerScheduleService::evaluate_decode_from_inputs(player.decode_schedule_inputs())
        .should_decode_now
    {
        DecodeWorkerAction::ContinueSoon
    } else {
        DecodeWorkerAction::WaitIndefinitely
    }
}

fn wait_for_signal(control: &Arc<(Mutex<DecodeWorkerControl>, Condvar)>) -> bool {
    let (lock, condvar) = &**control;
    let mut state = lock.lock().unwrap();

    if state.shutdown {
        return true;
    }

    if !state.decode_requested {
        loop {
            state = condvar.wait(state).unwrap();
            if state.shutdown {
                return true;
            }
            if state.decode_requested || state.wake_requested {
                break;
            }
        }
    }

    if state.wake_requested {
        state.wake_requested = false;
    }

    state.decode_requested = false;
    state.wake_requested = false;
    false
}

enum DecodeWorkerAction {
    ContinueSoon,
    WaitIndefinitely,
}

enum DecodeWorkerPlan {
    Decode {
        opened_media: SharedMediaSession,
        generation: u64,
        decode_policy: DecodePolicy,
    },
    WaitIndefinitely,
}

#[cfg(test)]
mod tests {
    use std::sync::Arc;

    use super::{complete_decode_action, DecodeWorkerAction};
    use crate::decode::{DecodedOutput, DecodedOutputPoll};
    use crate::player::handle::SemiPlayerHandle;
    use crate::render::core::frame::{PixelFormatCategory, VideoFrame, VideoSurface};

    fn frame(pts_us: i64, duration_us: Option<i64>) -> VideoFrame {
        VideoFrame {
            pts_us,
            duration_us,
            width: 1920,
            height: 1080,
            is_key_frame: false,
            surface: Arc::new(VideoSurface::new_cpu_packed(
                PixelFormatCategory::Bgra8,
                1920 * 4,
                vec![0; 16],
            )),
        }
    }

    #[test]
    fn stale_decode_output_is_dropped_when_generation_changes() {
        let mut player = SemiPlayerHandle::new();
        let stale_generation = player.media_generation();
        let _ = player.bump_media_generation();

        let action = complete_decode_action(
            &player,
            stale_generation,
            Ok(DecodedOutputPoll::Output(DecodedOutput::Video(frame(
                0,
                Some(33_000),
            )))),
        );

        assert!(matches!(action, DecodeWorkerAction::WaitIndefinitely));
        assert_eq!(player.runtime.get_mut().unwrap().runtime.video_queue_len(), 0);
        assert!(!player.runtime.get_mut().unwrap().video_sync.is_dirty());
    }

    #[test]
    fn current_generation_decode_output_is_applied_to_runtime() {
        let mut player = SemiPlayerHandle::new();
        let generation = player.media_generation();

        let action = complete_decode_action(
            &player,
            generation,
            Ok(DecodedOutputPoll::Output(DecodedOutput::Video(frame(
                0,
                Some(33_000),
            )))),
        );

        assert!(matches!(action, DecodeWorkerAction::WaitIndefinitely));
        assert_eq!(player.runtime.get_mut().unwrap().runtime.video_queue_len(), 1);
        assert!(player.runtime.get_mut().unwrap().video_sync.is_dirty());
    }
}
