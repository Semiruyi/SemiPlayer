use crate::api::error::{ResultCode, SEMI_E_INVALID_STATE, SEMI_OK};
use crate::api::types::PlayerState;
use crate::core::media::{DecodePolicy, DecodedOutput, DecodedOutputPoll, SharedOpenedMedia};
use crate::core::player::handle::SemiPlayerHandle;
use crate::core::player::video_sync::VideoSyncService;

const DEFAULT_PUMP_ITERATIONS: u32 = 256;
pub(crate) const DECODE_POLL_PACKET_BUDGET: usize = 4;

pub fn decode_supply(player: &mut SemiPlayerHandle, max_iterations: u32) -> ResultCode {
    let iterations = if max_iterations == 0 {
        DEFAULT_PUMP_ITERATIONS
    } else {
        max_iterations
    };
    let Some(opened_media) = player.opened_media.as_ref().cloned() else {
        return SEMI_E_INVALID_STATE;
    };

    for _ in 0..iterations {
        let decode_policy = player.decode_policy();
        let output = match poll_decoded_output_once(&opened_media, decode_policy) {
            Ok(DecodedOutputPoll::Output(output)) => output,
            Ok(DecodedOutputPoll::Pending) | Ok(DecodedOutputPoll::Finished) => break,
            Err(code) => return code,
        };

        if apply_decoded_output(player, output).reached_end {
            break;
        }

        if !player.runtime.decode_supply_status().needs_decode_supply {
            break;
        }
    }

    SEMI_OK
}

pub(crate) fn poll_decoded_output_once(
    opened_media: &SharedOpenedMedia,
    decode_policy: DecodePolicy,
) -> Result<DecodedOutputPoll, ResultCode> {
    opened_media
        .with_mut(|opened_media| {
            opened_media.poll_decoded_output(DECODE_POLL_PACKET_BUDGET, decode_policy)
        })
        .map_err(|_| SEMI_E_INVALID_STATE)
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub(crate) struct DecodedOutputApplyResult {
    pub reached_end: bool,
    pub should_wake_sync: bool,
}

pub(crate) fn apply_decoded_output(
    player: &mut SemiPlayerHandle,
    output: DecodedOutput,
) -> DecodedOutputApplyResult {
    match output {
        DecodedOutput::Video(frame) => {
            player.observe_seek_video_decoded(frame.pts_us);
            player.runtime.push_video_frame(frame);
            VideoSyncService::mark_dirty(player);
            DecodedOutputApplyResult {
                reached_end: false,
                should_wake_sync: true,
            }
        }
        DecodedOutput::SkippedVideo(frame) => {
            player.observe_seek_video_decoded(frame.pts_us);
            DecodedOutputApplyResult {
                reached_end: false,
                should_wake_sync: false,
            }
        }
        DecodedOutput::Audio(frame) => {
            player.runtime.push_audio_frame(frame);
            player.observe_seek_first_audio_decoded();
            DecodedOutputApplyResult {
                reached_end: false,
                should_wake_sync: should_wake_sync_for_audio_enqueue(player),
            }
        }
        DecodedOutput::EndOfStream => {
            player.runtime.mark_end_of_stream();
            DecodedOutputApplyResult {
                reached_end: true,
                should_wake_sync: true,
            }
        }
    }
}

fn should_wake_sync_for_audio_enqueue(player: &SemiPlayerHandle) -> bool {
    player.state() == PlayerState::Playing
        && player
            .audio_output
            .with_ref(|audio_output| !audio_output.snapshot().started)
}

#[cfg(test)]
mod tests {
    use super::{apply_decoded_output, DecodedOutputApplyResult};
    use crate::api::types::PlayerState;
    use crate::audio::core::frame::{AudioFrame, AudioSampleFormatCategory};
    use crate::audio::core::output::AudioStreamFormat;
    use crate::core::player::handle::SemiPlayerHandle;
    use crate::core::media::DecodedOutput;
    use crate::render::core::frame::{PixelFormatCategory, VideoFrame};

    fn audio_frame(pts_us: i64) -> AudioFrame {
        AudioFrame {
            pts_us,
            duration_us: Some(10_000),
            sample_rate: 48_000,
            channels: 2,
            sample_count: 480,
            sample_format: AudioSampleFormatCategory::F32,
            is_planar: false,
            data: vec![0.0; 480 * 2],
        }
    }

    fn video_frame(pts_us: i64) -> VideoFrame {
        VideoFrame {
            pts_us,
            duration_us: Some(33_000),
            width: 1920,
            height: 1080,
            pixel_format: PixelFormatCategory::Bgra8,
            stride: 1920 * 4,
            data: vec![0; 16],
            is_key_frame: false,
        }
    }

    #[test]
    fn queued_video_frames_count_toward_startup_buffer_target() {
        let mut player = SemiPlayerHandle::new();

        for index in 0..8 {
            player.runtime.push_audio_frame(audio_frame(index * 10_000));
        }

        player.runtime.push_video_frame(video_frame(0));
        player.runtime.push_video_frame(video_frame(33_000));
        player.runtime.push_video_frame(video_frame(66_000));

        assert!(player.runtime.decode_supply_status().has_sufficient_buffer);
    }

    #[test]
    fn applying_video_output_requests_sync_wake() {
        let mut player = SemiPlayerHandle::new();

        let result = apply_decoded_output(&mut player, DecodedOutput::Video(video_frame(0)));

        assert_eq!(
            result,
            DecodedOutputApplyResult {
                reached_end: false,
                should_wake_sync: true,
            }
        );
        assert!(player.video_sync.is_dirty());
    }

    #[test]
    fn applying_audio_output_does_not_wake_sync_when_audio_is_already_started() {
        let mut player = SemiPlayerHandle::new();
        player.set_state(PlayerState::Playing);
        player.audio_output.with_mut(|audio_output| {
            audio_output.ensure_backend_format(Some(AudioStreamFormat {
                sample_rate: 48_000,
                channels: 2,
            }));
            audio_output.sync_started_state(PlayerState::Playing);
        });

        let result = apply_decoded_output(&mut player, DecodedOutput::Audio(audio_frame(0)));

        assert_eq!(
            result,
            DecodedOutputApplyResult {
                reached_end: false,
                should_wake_sync: false,
            }
        );
    }

    #[test]
    fn applying_audio_output_wakes_sync_when_playing_backend_has_not_started() {
        let mut player = SemiPlayerHandle::new();
        player.set_state(PlayerState::Playing);
        player.audio_output.with_mut(|audio_output| {
            audio_output.ensure_backend_format(Some(AudioStreamFormat {
                sample_rate: 48_000,
                channels: 2,
            }));
        });

        let result = apply_decoded_output(&mut player, DecodedOutput::Audio(audio_frame(0)));

        assert_eq!(
            result,
            DecodedOutputApplyResult {
                reached_end: false,
                should_wake_sync: true,
            }
        );
    }
}
