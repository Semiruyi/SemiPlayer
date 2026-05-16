use crate::api::error::{ResultCode, SEMI_E_INVALID_STATE, SEMI_OK};
use crate::core::media::{DecodedOutput, DecodedOutputPoll, SharedOpenedMedia};
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
        let output = match poll_decoded_output_once(&opened_media) {
            Ok(DecodedOutputPoll::Output(output)) => output,
            Ok(DecodedOutputPoll::Pending) | Ok(DecodedOutputPoll::Finished) => break,
            Err(code) => return code,
        };

        if apply_decoded_output(player, output) {
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
) -> Result<DecodedOutputPoll, ResultCode> {
    opened_media
        .with_mut(|opened_media| opened_media.poll_decoded_output(DECODE_POLL_PACKET_BUDGET))
        .map_err(|_| SEMI_E_INVALID_STATE)
}

pub(crate) fn apply_decoded_output(player: &mut SemiPlayerHandle, output: DecodedOutput) -> bool {
    match output {
        DecodedOutput::Video(frame) => {
            player.runtime.push_video_frame(frame);
            VideoSyncService::mark_dirty(player);
            false
        }
        DecodedOutput::Audio(frame) => {
            player.runtime.push_audio_frame(frame);
            false
        }
        DecodedOutput::EndOfStream => {
            player.runtime.mark_end_of_stream();
            true
        }
    }
}

#[cfg(test)]
mod tests {
    use crate::audio::core::frame::{AudioFrame, AudioSampleFormatCategory};
    use crate::core::player::handle::SemiPlayerHandle;
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
}
