use std::sync::{Arc, Condvar, Mutex};
use std::thread::{self, JoinHandle};

use crate::core::media::{DecodePolicy, DecodedOutputPoll, SharedOpenedMedia};
use crate::core::player::execution::{apply_decoded_output, poll_decoded_output_once};
use crate::core::player::handle::{LockOwner, SemiPlayerHandle};
use crate::core::player::schedule::PlayerScheduleService;

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
            SemiPlayerHandle::with_locked_ptr_as(player_ptr, LockOwner::DecodeWorker, |player| {
                plan_decode_action(player)
            })
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
                    SemiPlayerHandle::with_locked_ptr_as(
                        player_ptr,
                        LockOwner::DecodeWorker,
                        |player| complete_decode_action(player, generation, polled_output),
                    )
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
    let hint = PlayerScheduleService::evaluate_decode(player);
    if !hint.worker_active {
        return DecodeWorkerPlan::WaitIndefinitely;
    }

    if !hint.should_decode_now {
        return DecodeWorkerPlan::WaitIndefinitely;
    }

    let Some(opened_media) = player.opened_media.clone() else {
        return DecodeWorkerPlan::WaitIndefinitely;
    };

    DecodeWorkerPlan::Decode {
        opened_media,
        generation: player.media_generation(),
        decode_policy: player.decode_policy(),
    }
}

fn complete_decode_action(
    player: &mut SemiPlayerHandle,
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
    if PlayerScheduleService::evaluate_decode(player).should_decode_now {
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
        opened_media: SharedOpenedMedia,
        generation: u64,
        decode_policy: DecodePolicy,
    },
    WaitIndefinitely,
}

#[cfg(test)]
mod tests {
    use super::{complete_decode_action, DecodeWorkerAction};
    use crate::core::media::{DecodedOutput, DecodedOutputPoll};
    use crate::core::player::handle::SemiPlayerHandle;
    use crate::render::core::frame::{PixelFormatCategory, VideoFrame};

    fn frame(pts_us: i64, duration_us: Option<i64>) -> VideoFrame {
        VideoFrame {
            pts_us,
            duration_us,
            width: 1920,
            height: 1080,
            pixel_format: PixelFormatCategory::Bgra8,
            stride: 1920 * 4,
            data: vec![0; 16],
            is_key_frame: false,
        }
    }

    #[test]
    fn stale_decode_output_is_dropped_when_generation_changes() {
        let mut player = SemiPlayerHandle::new();
        let stale_generation = player.media_generation();
        let _ = player.bump_media_generation();

        let action = complete_decode_action(
            &mut player,
            stale_generation,
            Ok(DecodedOutputPoll::Output(DecodedOutput::Video(frame(
                0,
                Some(33_000),
            )))),
        );

        assert!(matches!(action, DecodeWorkerAction::WaitIndefinitely));
        assert_eq!(player.runtime.video_queue_len(), 0);
        assert!(!player.video_sync.is_dirty());
    }

    #[test]
    fn current_generation_decode_output_is_applied_to_runtime() {
        let mut player = SemiPlayerHandle::new();
        let generation = player.media_generation();

        let action = complete_decode_action(
            &mut player,
            generation,
            Ok(DecodedOutputPoll::Output(DecodedOutput::Video(frame(
                0,
                Some(33_000),
            )))),
        );

        assert!(matches!(action, DecodeWorkerAction::WaitIndefinitely));
        assert_eq!(player.runtime.video_queue_len(), 1);
        assert!(player.video_sync.is_dirty());
    }
}
