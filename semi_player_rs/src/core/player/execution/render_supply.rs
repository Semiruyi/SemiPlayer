use crate::core::player::handle::SemiPlayerHandle;
use crate::render::core::frame::DecodedVideoFrame;
use crate::render::core::pipeline::{VideoRenderRequest, VideoRenderStats};

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub(crate) struct RenderSupplyResult {
    pub rendered_frames: usize,
    pub passthrough_frames: usize,
    pub passthrough_with_subtitle_intent_frames: usize,
    pub requires_transform_frames: usize,
    pub fallback_passthrough_frames: usize,
}

impl RenderSupplyResult {
    pub fn has_new_presentation_frames(self) -> bool {
        self.rendered_frames > 0
    }
}

pub(crate) fn render_supply(player: &mut SemiPlayerHandle) -> RenderSupplyResult {
    let request = default_render_request(player);
    let mut decoded_frames = Vec::<DecodedVideoFrame>::new();

    while let Some(frame) = player.runtime.pop_decoded_video_frame() {
        decoded_frames.push(frame);
    }

    let batch = player.render.render_frames(request, decoded_frames);
    let result = render_stats_to_result(batch.stats);

    for frame in batch.frames {
        player.runtime.push_presentation_video_frame(frame);
    }
    player.observe_render_stats(
        result.rendered_frames,
        result.passthrough_frames,
        result.passthrough_with_subtitle_intent_frames,
        result.requires_transform_frames,
        result.fallback_passthrough_frames,
    );

    result
}

fn default_render_request(player: &SemiPlayerHandle) -> VideoRenderRequest {
    VideoRenderRequest::from_target_profile(
        player.video_presentation_profile(),
        player.subtitles_visible,
    )
}

fn render_stats_to_result(stats: VideoRenderStats) -> RenderSupplyResult {
    RenderSupplyResult {
        rendered_frames: stats.rendered_frames,
        passthrough_frames: stats.passthrough_frames,
        passthrough_with_subtitle_intent_frames: stats.passthrough_with_subtitle_intent_frames,
        requires_transform_frames: stats.requires_transform_frames,
        fallback_passthrough_frames: stats.fallback_passthrough_frames,
    }
}

#[cfg(test)]
mod tests {
    use std::sync::Arc;

    use super::{default_render_request, render_supply, RenderSupplyResult};
    use crate::core::player::handle::SemiPlayerHandle;
    use crate::render::core::frame::{PixelFormatCategory, VideoFrame, VideoSurface};
    use crate::render::core::pipeline::VideoRenderRequest;

    fn decoded_frame(pts_us: i64) -> VideoFrame {
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
    fn synchronous_render_supply_promotes_all_decoded_frames() {
        let mut player = SemiPlayerHandle::new();
        player.runtime.push_decoded_video_frame(decoded_frame(0));
        player
            .runtime
            .push_decoded_video_frame(decoded_frame(33_000));

        let result = render_supply(&mut player);

        assert_eq!(
            result,
            RenderSupplyResult {
                rendered_frames: 2,
                passthrough_frames: 0,
                passthrough_with_subtitle_intent_frames: 2,
                requires_transform_frames: 0,
                fallback_passthrough_frames: 0,
            }
        );
        assert_eq!(player.runtime.decoded_video_queue_len(), 0);
        assert_eq!(player.runtime.presentation_video_queue_len(), 2);
        assert!(result.has_new_presentation_frames());
    }

    #[test]
    fn render_supply_reads_subtitle_visibility_from_player_state() {
        let mut player = SemiPlayerHandle::new();
        player.subtitles_visible = false;
        player.runtime.push_decoded_video_frame(decoded_frame(0));

        let result = render_supply(&mut player);

        assert_eq!(
            result,
            RenderSupplyResult {
                rendered_frames: 1,
                passthrough_frames: 1,
                passthrough_with_subtitle_intent_frames: 0,
                requires_transform_frames: 0,
                fallback_passthrough_frames: 0,
            }
        );
        assert_eq!(player.runtime.presentation_video_queue_len(), 1);
    }

    #[test]
    fn default_render_request_targets_cpu_bgra_compatibility() {
        let player = SemiPlayerHandle::new();

        let request = default_render_request(&player);

        assert_eq!(request, VideoRenderRequest::cpu_bgra_compatibility(true));
    }

    #[test]
    fn default_render_request_follows_player_profile() {
        let mut player = SemiPlayerHandle::new();
        player.set_video_presentation_profile(
            crate::render::core::pipeline::PresentationTargetProfile::D3d11BgraPresenter,
        );

        let request = default_render_request(&player);

        assert_eq!(request, VideoRenderRequest::d3d11_bgra_presenter(true));
    }

    #[test]
    fn gpu_texture_with_passthrough_profile_does_not_attempt_transform() {
        let mut player = SemiPlayerHandle::new();
        player.set_video_presentation_profile(
            crate::render::core::pipeline::PresentationTargetProfile::Passthrough,
        );
        player.runtime.push_decoded_video_frame(VideoFrame {
            pts_us: 0,
            duration_us: Some(33_000),
            width: 1920,
            height: 1080,
            is_key_frame: false,
            surface: Arc::new(VideoSurface::new_d3d11_texture_2d(
                PixelFormatCategory::Nv12,
                0x1234,
                None,
                0,
            )),
        });

        let result = render_supply(&mut player);

        assert_eq!(
            result,
            RenderSupplyResult {
                rendered_frames: 1,
                passthrough_frames: 0,
                passthrough_with_subtitle_intent_frames: 1,
                requires_transform_frames: 0,
                fallback_passthrough_frames: 0,
            }
        );
        assert_eq!(player.runtime.presentation_video_queue_len(), 1);
    }
}
