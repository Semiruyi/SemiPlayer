use crate::api::error::{ResultCode, SEMI_E_INVALID_STATE};
use crate::decode::session::SharedMediaSession;
use crate::decode::{DecodePolicy, DecodedOutputPoll};

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

#[cfg(test)]
mod tests {
    use std::sync::Arc;

    use super::DECODE_POLL_PACKET_BUDGET;
    use crate::audio::core::frame::{AudioFrame, AudioSampleFormatCategory};
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
        let rt = &mut player.runtime.get_mut().unwrap().runtime;

        for index in 0..8 {
            rt.push_audio_frame(audio_frame(index * 10_000));
        }

        rt.push_video_frame(video_frame(0));
        rt.push_video_frame(video_frame(33_000));
        rt.push_video_frame(video_frame(66_000));

        assert!(rt.decode_supply_status().has_sufficient_presentation_buffer);
    }

    #[test]
    fn decode_poll_budget_stays_small_for_worker_fairness() {
        assert_eq!(DECODE_POLL_PACKET_BUDGET, 4);
    }
}
