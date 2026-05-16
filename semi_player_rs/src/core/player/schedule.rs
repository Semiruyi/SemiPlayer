use crate::api::types::PlayerState;
use crate::core::player::handle::SemiPlayerHandle;
use crate::core::player::video_sync::VideoSyncService;
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

pub struct PlayerScheduleService;

impl PlayerScheduleService {
    pub fn evaluate(player: &SemiPlayerHandle) -> PumpScheduleHint {
        let playback_time_us = player.audio_clock.presentation_time_us();
        let video_snapshot = VideoSyncService::evaluate(player, playback_time_us);
        let next_video_deadline_us =
            compute_video_deadline_us(player, playback_time_us, video_snapshot);
        let next_audio_refill_deadline_us =
            compute_audio_refill_deadline_us(player, playback_time_us);
        let next_pump_deadline_us =
            min_optional_time(next_video_deadline_us, next_audio_refill_deadline_us);
        let playback_due_now = next_pump_deadline_us
            .is_some_and(|deadline_us| deadline_us <= playback_time_us);
        let decode_supply_needed = crate::core::player::pump::needs_decode_supply(player);
        let suggested_wait_us = compute_suggested_wait_us(
            player.state(),
            playback_time_us,
            next_pump_deadline_us,
        );

        PumpScheduleHint {
            playback_time_us,
            playback_due_now,
            decode_supply_needed,
            next_video_deadline_us,
            next_audio_refill_deadline_us,
            next_pump_deadline_us,
            suggested_wait_us,
        }
    }
}

fn compute_video_deadline_us(
    player: &SemiPlayerHandle,
    playback_time_us: MediaTimeUs,
    video_snapshot: crate::core::player::video_sync::VideoSyncSnapshot,
) -> Option<MediaTimeUs> {
    if player.video_sync.is_dirty() {
        return Some(playback_time_us);
    }

    if video_snapshot.core_sync_error_us > 0 {
        return Some(playback_time_us);
    }

    if player.runtime.current_video_frame().is_none() && player.runtime.video_queue_len() > 0 {
        return Some(playback_time_us);
    }

    video_snapshot.next_wake_deadline_us
}

fn compute_audio_refill_deadline_us(
    player: &SemiPlayerHandle,
    playback_time_us: MediaTimeUs,
) -> Option<MediaTimeUs> {
    let snapshot = player.audio_output.snapshot();
    let format = snapshot.configured_format?;

    if player.state() == PlayerState::Playing && !snapshot.started {
        return Some(playback_time_us);
    }

    if snapshot.target_buffer_frames == 0 {
        return Some(playback_time_us);
    }

    if snapshot.buffered_frames <= AUDIO_REFILL_HEADROOM_FRAMES {
        return Some(playback_time_us);
    }

    let frames_until_refill = snapshot
        .buffered_frames
        .saturating_sub(AUDIO_REFILL_HEADROOM_FRAMES);
    let refill_delta_us = frames_to_us(frames_until_refill, format.sample_rate);
    Some(playback_time_us.saturating_add(refill_delta_us))
}

fn compute_suggested_wait_us(
    state: PlayerState,
    playback_time_us: MediaTimeUs,
    next_pump_deadline_us: Option<MediaTimeUs>,
) -> MediaTimeUs {
    if state != PlayerState::Playing {
        return MAX_PUMP_INTERVAL_US;
    }

    let Some(deadline_us) = next_pump_deadline_us else {
        return MIN_PUMP_INTERVAL_US;
    };

    let wait_us = deadline_us.saturating_sub(playback_time_us);
    wait_us.clamp(MIN_PUMP_INTERVAL_US, MAX_PUMP_INTERVAL_US)
}

fn frames_to_us(frame_count: usize, sample_rate: u32) -> MediaTimeUs {
    if frame_count == 0 || sample_rate == 0 {
        return 0;
    }

    (frame_count as i64)
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
    use super::PlayerScheduleService;
    use crate::api::types::PlayerState;
    use crate::audio::core::output::AudioOutputChunk;
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
    fn schedule_prefers_earlier_video_deadline() {
        let mut player = SemiPlayerHandle::new();
        player.set_state(PlayerState::Playing);
        player.audio_clock.play();
        player.runtime.push_video_frame(frame(0, Some(33_000)));
        player.runtime.push_video_frame(frame(41_000, Some(41_000)));
        let _ = player.runtime.select_video_frame(&player.video_scheduler, 0);

        let hint = PlayerScheduleService::evaluate(&player);

        assert_eq!(hint.next_video_deadline_us, Some(41_000));
        assert_eq!(hint.next_pump_deadline_us, Some(41_000));
        assert_eq!(hint.suggested_wait_us, 33_000);
    }

    #[test]
    fn schedule_pulls_deadline_forward_for_audio_refill() {
        let mut player = SemiPlayerHandle::new();
        player.set_state(PlayerState::Ready);
        player.audio_clock.play();
        player.audio_output.ensure_backend_format(Some(crate::audio::core::output::AudioStreamFormat {
            sample_rate: 48_000,
            channels: 2,
        }));
        player.audio_output.submit_chunk(&AudioOutputChunk {
            pts_us: Some(0),
            sample_rate: 48_000,
            channels: 2,
            frame_count: 2_560,
            samples: vec![0.0; 2_560 * 2],
        });

        let hint = PlayerScheduleService::evaluate(&player);

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

        let hint = PlayerScheduleService::evaluate(&player);

        assert!(hint.playback_due_now);
        assert_eq!(hint.next_video_deadline_us, Some(0));
        assert_eq!(hint.next_pump_deadline_us, Some(0));
    }

    #[test]
    fn playing_with_unstarted_audio_backend_forces_immediate_pump() {
        let mut player = SemiPlayerHandle::new();
        player.set_state(PlayerState::Playing);
        player.audio_clock.play();
        player.audio_output.ensure_backend_format(Some(crate::audio::core::output::AudioStreamFormat {
            sample_rate: 48_000,
            channels: 2,
        }));

        let hint = PlayerScheduleService::evaluate(&player);

        assert!(hint.playback_due_now);
        assert_eq!(hint.next_audio_refill_deadline_us, Some(hint.playback_time_us));
        assert_eq!(hint.next_pump_deadline_us, Some(hint.playback_time_us));
        assert_eq!(hint.suggested_wait_us, 1_000);
    }

    #[test]
    fn insufficient_buffers_report_decode_supply_needed() {
        let player = SemiPlayerHandle::new();

        let hint = PlayerScheduleService::evaluate(&player);

        assert!(hint.decode_supply_needed);
    }
}
