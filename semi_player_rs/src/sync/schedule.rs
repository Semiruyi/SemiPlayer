use crate::api::types::PlayerState;
use crate::audio::core::output_controller::AudioOutputSnapshot;
use crate::player::runtime::{DecodeSupplyStatus, RuntimeVideoSnapshot};
use crate::sync::video_sync::VideoSyncSnapshot;
use crate::util::time::MediaTimeUs;

const MIN_PUMP_INTERVAL_US: MediaTimeUs = 1_000;
const MAX_PUMP_INTERVAL_US: MediaTimeUs = 33_000;
const AUDIO_REFILL_HEADROOM_FRAMES: usize = 2_048;

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct PumpScheduleHint {
    pub playback_time_us: MediaTimeUs,
    pub playback_due_now: bool,
    pub decode_supply_needed: bool,
    pub next_video_deadline_us: Option<MediaTimeUs>,
    pub next_audio_refill_deadline_us: Option<MediaTimeUs>,
    pub next_pump_deadline_us: Option<MediaTimeUs>,
    pub suggested_wait_us: MediaTimeUs,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
#[allow(clippy::struct_excessive_bools)]
pub struct DecodeScheduleHint {
    pub media_loaded: bool,
    pub worker_active: bool,
    pub needs_decode_supply: bool,
    pub should_decode_now: bool,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct DecodeScheduleInputs {
    pub media_loaded: bool,
    pub state: PlayerState,
    pub decode_supply: DecodeSupplyStatus,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ScheduledWork {
    pub should_advance_playback: bool,
    pub should_request_decode: bool,
    pub deadline_us: Option<MediaTimeUs>,
    pub wait_us: MediaTimeUs,
}

impl PumpScheduleHint {
    pub fn scheduled_work(self) -> ScheduledWork {
        ScheduledWork {
            should_advance_playback: self.playback_due_now,
            should_request_decode: self.decode_supply_needed,
            deadline_us: self.next_pump_deadline_us,
            wait_us: self.suggested_wait_us.max(1),
        }
    }
}

pub struct PlayerScheduleService;

#[derive(Clone, Copy, Debug)]
pub struct ScheduleInputs<'a> {
    pub state: PlayerState,
    pub playback_time_us: MediaTimeUs,
    pub gating_audio_for_seek_recovery: bool,
    pub decode_supply: DecodeSupplyStatus,
    pub video_sync_dirty: bool,
    pub runtime_video: RuntimeVideoSnapshot<'a>,
    pub video_snapshot: VideoSyncSnapshot,
    pub audio_output: AudioOutputSnapshot,
}

impl PlayerScheduleService {
    pub fn evaluate_from_inputs(inputs: ScheduleInputs<'_>) -> PumpScheduleHint {
        let context = ScheduleContext::from_inputs(inputs);
        let next_video_deadline_us = compute_video_deadline_us(&context);
        let next_audio_refill_deadline_us = compute_audio_refill_deadline_us(&context);
        let next_pump_deadline_us =
            min_optional_time(next_video_deadline_us, next_audio_refill_deadline_us);
        let playback_due_now = next_pump_deadline_us
            .is_some_and(|deadline_us| deadline_us <= context.playback_time_us);
        let suggested_wait_us = compute_suggested_wait_us(&context, next_pump_deadline_us);

        PumpScheduleHint {
            playback_time_us: context.playback_time_us,
            playback_due_now,
            decode_supply_needed: context.decode_supply.needs_decode_supply,
            next_video_deadline_us,
            next_audio_refill_deadline_us,
            next_pump_deadline_us,
            suggested_wait_us,
        }
    }

    pub fn evaluate_decode_from_inputs(inputs: DecodeScheduleInputs) -> DecodeScheduleHint {
        compute_decode_schedule_hint(inputs.media_loaded, inputs.state, inputs.decode_supply)
    }
}

#[derive(Clone, Copy, Debug)]
struct ScheduleContext<'a> {
    state: PlayerState,
    playback_time_us: MediaTimeUs,
    gating_audio_for_seek_recovery: bool,
    decode_supply: DecodeSupplyStatus,
    video_sync_dirty: bool,
    runtime_video: RuntimeVideoSnapshot<'a>,
    video_snapshot: VideoSyncSnapshot,
    audio_output: AudioOutputSnapshot,
}

impl<'a> ScheduleContext<'a> {
    fn from_inputs(inputs: ScheduleInputs<'a>) -> Self {
        Self {
            state: inputs.state,
            playback_time_us: inputs.playback_time_us,
            gating_audio_for_seek_recovery: inputs.gating_audio_for_seek_recovery,
            decode_supply: inputs.decode_supply,
            video_sync_dirty: inputs.video_sync_dirty,
            runtime_video: inputs.runtime_video,
            video_snapshot: inputs.video_snapshot,
            audio_output: inputs.audio_output,
        }
    }
}

fn compute_video_deadline_us(context: &ScheduleContext<'_>) -> Option<MediaTimeUs> {
    if context.video_sync_dirty {
        return Some(context.playback_time_us);
    }

    if context.video_snapshot.core_sync_error_us > 0 {
        return Some(context.playback_time_us);
    }

    if context.runtime_video.current_frame.is_none() && context.runtime_video.next_frame.is_some() {
        return Some(context.playback_time_us);
    }

    context.video_snapshot.next_wake_deadline_us
}

fn compute_audio_refill_deadline_us(context: &ScheduleContext<'_>) -> Option<MediaTimeUs> {
    if context.gating_audio_for_seek_recovery {
        return None;
    }

    let snapshot = context.audio_output;
    let format = snapshot.configured_format?;

    if context.state == PlayerState::Playing && !snapshot.started {
        return Some(context.playback_time_us);
    }

    if snapshot.target_buffer_frames == 0 {
        return Some(context.playback_time_us);
    }

    if snapshot.buffered_frames <= AUDIO_REFILL_HEADROOM_FRAMES {
        return Some(context.playback_time_us);
    }

    let frames_until_refill = snapshot
        .buffered_frames
        .saturating_sub(AUDIO_REFILL_HEADROOM_FRAMES);
    let refill_delta_us = frames_to_us(frames_until_refill, format.sample_rate);
    Some(context.playback_time_us.saturating_add(refill_delta_us))
}

fn compute_suggested_wait_us(
    context: &ScheduleContext<'_>,
    next_pump_deadline_us: Option<MediaTimeUs>,
) -> MediaTimeUs {
    if context.state != PlayerState::Playing {
        return MAX_PUMP_INTERVAL_US;
    }

    let Some(deadline_us) = next_pump_deadline_us else {
        return MIN_PUMP_INTERVAL_US;
    };

    let wait_us = deadline_us.saturating_sub(context.playback_time_us);
    wait_us.clamp(MIN_PUMP_INTERVAL_US, MAX_PUMP_INTERVAL_US)
}

fn compute_decode_schedule_hint(
    media_loaded: bool,
    state: PlayerState,
    decode_supply: DecodeSupplyStatus,
) -> DecodeScheduleHint {
    let worker_active = media_loaded && state != PlayerState::Idle;
    let should_decode_now = worker_active && decode_supply.needs_decode_supply;

    DecodeScheduleHint {
        media_loaded,
        worker_active,
        needs_decode_supply: decode_supply.needs_decode_supply,
        should_decode_now,
    }
}

fn frames_to_us(frame_count: usize, sample_rate: u32) -> MediaTimeUs {
    if frame_count == 0 || sample_rate == 0 {
        return 0;
    }

    i64::try_from(frame_count)
        .unwrap_or(i64::MAX)
        .saturating_mul(1_000_000)
        .saturating_div(i64::from(sample_rate))
}

fn min_optional_time(lhs: Option<MediaTimeUs>, rhs: Option<MediaTimeUs>) -> Option<MediaTimeUs> {
    match (lhs, rhs) {
        (Some(lhs), Some(rhs)) => Some(lhs.min(rhs)),
        (Some(lhs), None) => Some(lhs),
        (None, Some(rhs)) => Some(rhs),
        (None, None) => None,
    }
}

#[cfg(test)]
mod tests {
    use std::sync::Arc;

    use super::{compute_decode_schedule_hint, PlayerScheduleService, PumpScheduleHint};
    use crate::api::types::PlayerState;
    use crate::audio::core::output::AudioOutputChunk;
    use crate::player::handle::SemiPlayerHandle;
    use crate::player::runtime::DecodeSupplyStatus;
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
    fn schedule_prefers_earlier_video_deadline() {
        let mut player = SemiPlayerHandle::new();
        player.set_state(PlayerState::Playing);
        player.audio_clock.play();
        player.runtime.push_video_frame(frame(0, Some(33_000)));
        player.runtime.push_video_frame(frame(41_000, Some(41_000)));
        let _ = player
            .runtime
            .select_video_frame(&player.video_scheduler, 0, |_| {});

        let hint = PlayerScheduleService::evaluate_from_inputs(player.schedule_inputs());

        assert_eq!(hint.next_video_deadline_us, Some(41_000));
        assert_eq!(hint.next_pump_deadline_us, Some(41_000));
        assert_eq!(hint.suggested_wait_us, 33_000);
    }

    #[test]
    fn schedule_pulls_deadline_forward_for_audio_refill() {
        let player = SemiPlayerHandle::new();
        player.set_state(PlayerState::Ready);
        player.audio_clock.play();
        player.audio_output.with_mut(|audio_output| {
            audio_output.ensure_backend_format(Some(
                crate::audio::core::output::AudioStreamFormat {
                    sample_rate: 48_000,
                    channels: 2,
                },
            ));
            audio_output.submit_chunk(&AudioOutputChunk {
                pts_us: Some(0),
                sample_rate: 48_000,
                channels: 2,
                frame_count: 2_560,
                samples: vec![0.0; 2_560 * 2],
            });
        });

        let hint = PlayerScheduleService::evaluate_from_inputs(player.schedule_inputs());

        let refill_delta_us = hint
            .next_audio_refill_deadline_us
            .expect("audio refill deadline")
            .saturating_sub(hint.playback_time_us);
        let pump_delta_us = hint
            .next_pump_deadline_us
            .expect("pump deadline")
            .saturating_sub(hint.playback_time_us);

        assert_eq!(refill_delta_us, 10_666);
        assert_eq!(pump_delta_us, 10_666);
        assert_eq!(hint.suggested_wait_us, 33_000);
    }

    #[test]
    fn dirty_video_sync_forces_immediate_pump() {
        let mut player = SemiPlayerHandle::new();
        player.video_sync.reset();

        let hint = PlayerScheduleService::evaluate_from_inputs(player.schedule_inputs());

        assert!(hint.playback_due_now);
        assert_eq!(hint.next_video_deadline_us, Some(0));
        assert_eq!(hint.next_pump_deadline_us, Some(0));
    }

    #[test]
    fn playing_with_unstarted_audio_backend_forces_immediate_pump() {
        let player = SemiPlayerHandle::new();
        player.set_state(PlayerState::Playing);
        player.audio_clock.play();
        player.audio_output.with_mut(|audio_output| {
            audio_output.ensure_backend_format(Some(
                crate::audio::core::output::AudioStreamFormat {
                    sample_rate: 48_000,
                    channels: 2,
                },
            ));
        });

        let hint = PlayerScheduleService::evaluate_from_inputs(player.schedule_inputs());

        assert!(hint.playback_due_now);
        assert_eq!(
            hint.next_audio_refill_deadline_us,
            Some(hint.playback_time_us)
        );
        assert_eq!(hint.next_pump_deadline_us, Some(hint.playback_time_us));
        assert_eq!(hint.suggested_wait_us, 1_000);
    }

    #[test]
    fn insufficient_buffers_report_decode_supply_needed() {
        let player = SemiPlayerHandle::new();

        let hint = PlayerScheduleService::evaluate_from_inputs(player.schedule_inputs());

        assert!(hint.decode_supply_needed);
    }

    #[test]
    fn scheduled_work_separates_playback_and_decode_axes() {
        let work = PumpScheduleHint {
            playback_due_now: true,
            decode_supply_needed: true,
            next_pump_deadline_us: Some(12_345),
            suggested_wait_us: 7_000,
            ..PumpScheduleHint::default()
        }
        .scheduled_work();

        assert!(work.should_advance_playback);
        assert!(work.should_request_decode);
        assert_eq!(work.deadline_us, Some(12_345));
        assert_eq!(work.wait_us, 7_000);
    }

    #[test]
    fn decode_hint_stays_inactive_without_loaded_media() {
        let hint = compute_decode_schedule_hint(
            false,
            PlayerState::Ready,
            DecodeSupplyStatus {
                needs_decode_supply: true,
                ..DecodeSupplyStatus::default()
            },
        );

        assert!(!hint.media_loaded);
        assert!(!hint.worker_active);
        assert!(hint.needs_decode_supply);
        assert!(!hint.should_decode_now);
    }

    #[test]
    fn decode_hint_runs_only_for_active_non_idle_states() {
        let status = DecodeSupplyStatus {
            needs_decode_supply: true,
            ..DecodeSupplyStatus::default()
        };

        let idle_hint = compute_decode_schedule_hint(true, PlayerState::Idle, status);
        let playing_hint = compute_decode_schedule_hint(true, PlayerState::Playing, status);

        assert!(!idle_hint.should_decode_now);
        assert!(playing_hint.should_decode_now);
    }
}
