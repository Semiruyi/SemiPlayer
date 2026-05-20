use std::sync::{Arc, Condvar, Mutex};
use std::thread::{self, JoinHandle};

use crate::decode::session::SharedMediaSession;
use crate::decode::{DecodePolicy, DecodedOutput, DecodedOutputPoll};
use crate::player::access::DecodePlanContext;
use crate::player::execution::{apply_decoded_output, poll_decoded_output_once};
use crate::player::handle::SemiPlayerHandle;
use crate::scheduler::types::{ResourceKey, SchedulerEvent, StageId};
use crate::sync::schedule::PlayerScheduleService;
use crate::util::debug_trace::append_trace_line;

#[derive(Default)]
struct DecodeWorkerControl {
    shutdown: bool,
    wake_requested: bool,
    decode_requested: bool,
    audio_decode_requested: bool,
    video_decode_requested: bool,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
struct DecodeRequestIntent {
    audio: bool,
    video: bool,
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

    pub fn request_decode_stage(&self, stage: StageId) {
        let (lock, condvar) = &*self.control;
        let mut control = lock.lock().unwrap();
        control.decode_requested = true;
        control.wake_requested = true;
        match stage {
            StageId::AudioDecode => control.audio_decode_requested = true,
            StageId::VideoDecode => control.video_decode_requested = true,
            StageId::AudioRender | StageId::VideoRender => {}
        }
        condvar.notify_all();
    }

    pub fn stop(&mut self) {
        append_trace_line("decode:stop requested");
        let (lock, condvar) = &*self.control;
        {
            let mut control = lock.lock().unwrap();
            control.shutdown = true;
            control.wake_requested = true;
        }
        condvar.notify_all();

        if let Some(thread) = self.thread.take() {
            append_trace_line("decode:joining");
            let _ = thread.join();
            append_trace_line("decode:joined");
        }
    }
}

#[allow(clippy::needless_pass_by_value)]
fn worker_loop(player_addr: usize, control: Arc<(Mutex<DecodeWorkerControl>, Condvar)>) {
    loop {
        if shutdown_requested(&control) {
            append_trace_line("decode:loop exit shutdown_requested");
            break;
        }

        let plan = unsafe {
            let player_ptr = player_addr as *mut SemiPlayerHandle;
            plan_decode_action(&*player_ptr, &control)
        };

        match plan {
            DecodeWorkerPlan::Decode {
                opened_media,
                generation,
                decode_policy,
                intent,
            } => {
                emit_decode_stage_started(player_addr, intent);
                let polled_output = poll_decoded_output_once(&opened_media, decode_policy);
                let action = unsafe {
                    let player_ptr = player_addr as *mut SemiPlayerHandle;
                    complete_decode_action(&*player_ptr, generation, polled_output, intent, &control)
                };

                match action {
                    DecodeWorkerAction::ContinueSoon => {}
                    DecodeWorkerAction::WaitIndefinitely => {
                        if wait_for_signal(&control) {
                            append_trace_line("decode:loop exit wait_for_signal");
                            break;
                        }
                    }
                }
            }
            DecodeWorkerPlan::WaitIndefinitely => {
                emit_decode_stage_idle(player_addr);
                if wait_for_signal(&control) {
                    append_trace_line("decode:loop exit wait_for_signal");
                    break;
                }
            }
        }
    }
}

fn shutdown_requested(control: &Arc<(Mutex<DecodeWorkerControl>, Condvar)>) -> bool {
    control.0.lock().unwrap().shutdown
}

fn plan_decode_action(
    player: &SemiPlayerHandle,
    control: &Arc<(Mutex<DecodeWorkerControl>, Condvar)>,
) -> DecodeWorkerPlan {
    let context = player.decode_plan_context();
    let hint = PlayerScheduleService::evaluate_decode_from_inputs(player.decode_schedule_inputs());
    let intent = decode_request_intent(control);
    let plan = plan_decode_action_from_context(context, hint, intent);
    append_trace_line(&format!(
        "decode:plan demand={:?} hint={:?} intent={:?} action={}",
        player.runtime_decode_demand_snapshot(),
        hint,
        intent,
        match plan {
            DecodeWorkerPlan::Decode { .. } => "decode",
            DecodeWorkerPlan::WaitIndefinitely => "wait",
        }
    ));
    plan
}

fn plan_decode_action_from_context(
    context: DecodePlanContext,
    hint: crate::sync::schedule::DecodeScheduleHint,
    intent: DecodeRequestIntent,
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
        intent,
    }
}

fn complete_decode_action(
    player: &SemiPlayerHandle,
    generation: u64,
    polled_output: Result<DecodedOutputPoll, i32>,
    intent: DecodeRequestIntent,
    control: &Arc<(Mutex<DecodeWorkerControl>, Condvar)>,
) -> DecodeWorkerAction {
    if generation != player.media_generation() {
        return next_decode_action(player);
    }

    match polled_output {
        Ok(DecodedOutputPoll::Output(output)) => {
            clear_decode_request_intent_for_output(control, output_stage(&output), intent);
            let scheduler_event = decode_progress_event(&output);
            let apply_result = apply_decoded_output(player, output);
            append_trace_line(&format!(
                "decode:complete generation={} apply_result={:?} demand={:?}",
                generation, apply_result, player.runtime_decode_demand_snapshot()
            ));
            if let Some(event) = scheduler_event {
                player.dispatch_scheduler_event(event);
            } else if apply_result.reached_end || apply_result.should_wake_sync {
                player.dispatch_scheduler_event(SchedulerEvent::PlaybackDemandChanged);
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
    let hint = PlayerScheduleService::evaluate_decode_from_inputs(player.decode_schedule_inputs());
    let demand = player.runtime_decode_demand_snapshot();

    let should_continue_soon =
        (demand.needs_audio_decode || demand.needs_video_decode) && hint.should_decode_now;

    append_trace_line(&format!(
        "decode:next_action hint={:?} demand={:?} continue_soon={}",
        hint, demand, should_continue_soon
    ));

    if should_continue_soon {
        DecodeWorkerAction::ContinueSoon
    } else {
        DecodeWorkerAction::WaitIndefinitely
    }
}

fn decode_progress_event(output: &DecodedOutput) -> Option<SchedulerEvent> {
    match output {
        DecodedOutput::Video(_) => Some(SchedulerEvent::StageProgress {
            stage: StageId::VideoDecode,
            produced: vec![ResourceKey::DecodedVideo],
        }),
        // Compatibility shim:
        // audio currently reaches playback without an explicit audio-render stage.
        // Emit both decoded and presentation-audio progress so the scheduler can
        // wake playback now, and later this can collapse to decoded-only when an
        // audio-render stage lands.
        DecodedOutput::Audio(_) => Some(SchedulerEvent::StageProgress {
            stage: StageId::AudioDecode,
            produced: vec![ResourceKey::DecodedAudio, ResourceKey::PresentationAudio],
        }),
        DecodedOutput::EndOfStream => Some(SchedulerEvent::PlaybackDemandChanged),
        DecodedOutput::SkippedVideo(_) | DecodedOutput::SkippedAudio(_) => None,
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
        intent: DecodeRequestIntent,
    },
    WaitIndefinitely,
}

fn decode_request_intent(control: &Arc<(Mutex<DecodeWorkerControl>, Condvar)>) -> DecodeRequestIntent {
    let state = control.0.lock().unwrap();
    DecodeRequestIntent {
        audio: state.audio_decode_requested,
        video: state.video_decode_requested,
    }
}

fn clear_decode_request_intent_for_output(
    control: &Arc<(Mutex<DecodeWorkerControl>, Condvar)>,
    stage: Option<StageId>,
    intent: DecodeRequestIntent,
) {
    let Some(stage) = stage else {
        return;
    };

    let mut state = control.0.lock().unwrap();
    match stage {
        StageId::AudioDecode => {
            if intent.audio {
                state.audio_decode_requested = false;
            }
        }
        StageId::VideoDecode => {
            if intent.video {
                state.video_decode_requested = false;
            }
        }
        StageId::AudioRender | StageId::VideoRender => {}
    }
}

fn output_stage(output: &DecodedOutput) -> Option<StageId> {
    match output {
        DecodedOutput::Video(_) | DecodedOutput::SkippedVideo(_) => Some(StageId::VideoDecode),
        DecodedOutput::Audio(_) | DecodedOutput::SkippedAudio(_) => Some(StageId::AudioDecode),
        DecodedOutput::EndOfStream => None,
    }
}

fn emit_decode_stage_started(player_addr: usize, intent: DecodeRequestIntent) {
    if !intent.audio && !intent.video {
        return;
    }

    unsafe {
        let player_ptr = player_addr as *mut SemiPlayerHandle;
        if intent.audio {
            (&*player_ptr).dispatch_scheduler_event(SchedulerEvent::StageStarted(
                StageId::AudioDecode,
            ));
        }
        if intent.video {
            (&*player_ptr).dispatch_scheduler_event(SchedulerEvent::StageStarted(
                StageId::VideoDecode,
            ));
        }
    }
}

fn emit_decode_stage_idle(player_addr: usize) {
    unsafe {
        let player_ptr = player_addr as *mut SemiPlayerHandle;
        (&*player_ptr).dispatch_scheduler_event(SchedulerEvent::StageIdle(
            StageId::AudioDecode,
        ));
        (&*player_ptr).dispatch_scheduler_event(SchedulerEvent::StageIdle(
            StageId::VideoDecode,
        ));
    }
}

#[cfg(test)]
mod tests {
    use std::sync::Arc;
    use std::sync::{Condvar, Mutex};

    use super::{
        complete_decode_action, decode_request_intent, wait_for_signal, DecodeRequestIntent,
        DecodeWorkerAction, DecodeWorkerControl,
    };
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

    fn decode_control(
        audio_requested: bool,
        video_requested: bool,
    ) -> Arc<(Mutex<DecodeWorkerControl>, Condvar)> {
        Arc::new((
            Mutex::new(DecodeWorkerControl {
                audio_decode_requested: audio_requested,
                video_decode_requested: video_requested,
                ..DecodeWorkerControl::default()
            }),
            Condvar::new(),
        ))
    }

    #[test]
    fn stale_decode_output_is_dropped_when_generation_changes() {
        let mut player = SemiPlayerHandle::new();
        let stale_generation = player.media_generation();
        let control = decode_control(false, true);
        let _ = player.bump_media_generation();

        let action = complete_decode_action(
            &player,
            stale_generation,
            Ok(DecodedOutputPoll::Output(DecodedOutput::Video(frame(
                0,
                Some(33_000),
            )))),
            DecodeRequestIntent {
                audio: false,
                video: true,
            },
            &control,
        );

        assert!(matches!(action, DecodeWorkerAction::WaitIndefinitely));
        assert_eq!(player.runtime.get_mut().unwrap().runtime.video_queue_len(), 0);
        assert!(!player.runtime.get_mut().unwrap().video_sync.is_dirty());
    }

    #[test]
    fn current_generation_decode_output_is_applied_to_runtime() {
        let mut player = SemiPlayerHandle::new();
        let generation = player.media_generation();
        let control = decode_control(false, true);

        let action = complete_decode_action(
            &player,
            generation,
            Ok(DecodedOutputPoll::Output(DecodedOutput::Video(frame(
                0,
                Some(33_000),
            )))),
            DecodeRequestIntent {
                audio: false,
                video: true,
            },
            &control,
        );

        assert!(matches!(action, DecodeWorkerAction::WaitIndefinitely));
        assert_eq!(player.runtime.get_mut().unwrap().runtime.decoded_video_queue_len(), 1);
        assert_eq!(player.runtime.get_mut().unwrap().runtime.presentation_video_queue_len(), 0);
        assert!(!player.runtime.get_mut().unwrap().video_sync.is_dirty());
        assert_eq!(
            decode_request_intent(&control),
            DecodeRequestIntent {
                audio: false,
                video: false,
            }
        );
    }

    #[test]
    fn wait_for_signal_preserves_stage_specific_intent_until_decode_completes() {
        let control = Arc::new((
            Mutex::new(DecodeWorkerControl {
                wake_requested: true,
                decode_requested: true,
                audio_decode_requested: false,
                video_decode_requested: true,
                ..DecodeWorkerControl::default()
            }),
            Condvar::new(),
        ));

        let should_exit = wait_for_signal(&control);
        let state = control.0.lock().unwrap();

        assert!(!should_exit);
        assert!(!state.decode_requested);
        assert!(!state.wake_requested);
        assert!(!state.audio_decode_requested);
        assert!(state.video_decode_requested);
    }
}
