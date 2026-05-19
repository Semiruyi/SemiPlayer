use crate::api::error::{ResultCode, SEMI_E_INVALID_STATE};
use crate::api::types::PlayerState;
use crate::decode::session::SharedMediaSession;
use crate::decode::{DecodePolicy, DecodedOutput, DecodedOutputPoll};
use crate::player::execution::render_supply::render_supply;
use crate::player::handle::SemiPlayerHandle;
use crate::render::core::frame::DecodedVideoFrame;

pub(crate) const DECODE_POLL_PACKET_BUDGET: usize = 4;

pub(crate) fn poll_decoded_output_once(
    opened_media: &SharedMediaSession,
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

#[derive(Clone, Copy, Debug)]
struct VideoOutputPrepare {
    playback_time_us: i64,
}

pub(crate) fn apply_decoded_output(
    player: &mut SemiPlayerHandle,
    output: DecodedOutput,
) -> DecodedOutputApplyResult {
    match output {
        DecodedOutput::Video(frame) => apply_video_output(player, frame),
        DecodedOutput::SkippedVideo(frame) => {
            let playback_time_us = player.playback_position_us_snapshot();
            player.observe_seek_video_decoded_access(frame.pts_us, playback_time_us);
            DecodedOutputApplyResult {
                reached_end: false,
                should_wake_sync: false,
            }
        }
        DecodedOutput::Audio(frame) => {
            player.observe_seek_first_audio_decoder_output_access();
            player.observe_seek_first_audio_decoded_access();
            let context = player.decode_audio_commit_context();
            let Some(frame) = trim_audio_for_seek_recovery(context.decode_policy, frame) else {
                return DecodedOutputApplyResult {
                    reached_end: false,
                    should_wake_sync: false,
                };
            };
            player.with_runtime_access(|mut runtime| {
                runtime.push_audio_frame(frame);
            });
            DecodedOutputApplyResult {
                reached_end: false,
                should_wake_sync: should_wake_sync_for_audio_enqueue(context),
            }
        }
        DecodedOutput::SkippedAudio(frame) => {
            player.observe_seek_first_audio_decoder_output_access();
            let _ = frame;
            DecodedOutputApplyResult {
                reached_end: false,
                should_wake_sync: false,
            }
        }
        DecodedOutput::EndOfStream => {
            player.with_runtime_access(|mut runtime| {
                runtime.mark_end_of_stream();
            });
            DecodedOutputApplyResult {
                reached_end: true,
                should_wake_sync: true,
            }
        }
    }
}

fn apply_video_output(
    player: &mut SemiPlayerHandle,
    frame: DecodedVideoFrame,
) -> DecodedOutputApplyResult {
    let prepared = prepare_video_output(player);
    commit_video_prepare(player, prepared, frame);

    let render_result = render_supply(player);

    DecodedOutputApplyResult {
        reached_end: false,
        should_wake_sync: render_result.has_new_presentation_frames(),
    }
}

fn prepare_video_output(player: &SemiPlayerHandle) -> VideoOutputPrepare {
    VideoOutputPrepare {
        playback_time_us: player.playback_position_us_snapshot(),
    }
}

fn commit_video_prepare(
    player: &mut SemiPlayerHandle,
    prepared: VideoOutputPrepare,
    frame: DecodedVideoFrame,
) {
    player.observe_seek_video_decoded_access(frame.pts_us, prepared.playback_time_us);
    player.with_runtime_access(|mut runtime| {
        runtime.push_decoded_video_frame(frame);
    });
}

fn should_wake_sync_for_audio_enqueue(
    context: crate::player::access::DecodeAudioCommitContext,
) -> bool {
    context.state == PlayerState::Playing && !context.audio_output.started
}

fn trim_audio_for_seek_recovery(
    decode_policy: DecodePolicy,
    frame: crate::audio::core::frame::AudioFrame,
) -> Option<crate::audio::core::frame::AudioFrame> {
    let Some(target_us) = decode_policy
        .seek_recovery
        .map(|policy| policy.target_video_us)
    else {
        return Some(frame);
    };

    frame.trim_to_start_us(target_us)
}

#[cfg(test)]
mod tests {
    use std::sync::Arc;

    use super::{apply_decoded_output, DecodedOutputApplyResult};
    use crate::api::types::PlayerState;
    use crate::audio::core::frame::{AudioFrame, AudioSampleFormatCategory};
    use crate::audio::core::output::AudioStreamFormat;
    use crate::decode::DecodedOutput;
    use crate::player::handle::SemiPlayerHandle;
    use crate::render::core::frame::{PixelFormatCategory, VideoFrame, VideoSurface};

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
            is_key_frame: false,
            surface: Arc::new(VideoSurface::new_cpu_packed(
                PixelFormatCategory::Bgra8,
                1920 * 4,
                vec![0; 16],
            )),
        }
    }

    #[test]
    fn queued_video_frames_count_toward_startup_buffer_target() {
        let mut player = SemiPlayerHandle::new();
        let rt = player.runtime.get_mut().unwrap();

        for index in 0..8 {
            rt.push_audio_frame(audio_frame(index * 10_000));
        }

        rt.push_video_frame(video_frame(0));
        rt.push_video_frame(video_frame(33_000));
        rt.push_video_frame(video_frame(66_000));

        assert!(rt.decode_supply_status().has_sufficient_presentation_buffer);
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
