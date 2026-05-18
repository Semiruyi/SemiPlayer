use crate::core::player::handle::SemiPlayerHandle;
use crate::render::core::frame::DecodedVideoFrame;
use crate::render::core::pipeline::{
    PresentationPixelFormatPreference, PresentationSurfaceKindPreference, VideoRenderPipeline,
    VideoRenderRequest,
};

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub(crate) struct RenderSupplyResult {
    pub rendered_frames: usize,
}

impl RenderSupplyResult {
    pub fn has_new_presentation_frames(self) -> bool {
        self.rendered_frames > 0
    }
}

pub(crate) fn render_supply(player: &mut SemiPlayerHandle) -> RenderSupplyResult {
    let pipeline = VideoRenderPipeline::new();
    let request = VideoRenderRequest {
        presentation_pixel_format: PresentationPixelFormatPreference::PreserveInput,
        presentation_surface_kind: PresentationSurfaceKindPreference::PreserveInput,
        subtitles_visible: player.subtitles_visible,
    };
    let mut decoded_frames = Vec::<DecodedVideoFrame>::new();

    while let Some(frame) = player.runtime.pop_decoded_video_frame() {
        decoded_frames.push(frame);
    }

    let mut result = RenderSupplyResult::default();

    for frame in pipeline.render_frames(request, decoded_frames) {
        player.runtime.push_presentation_video_frame(frame);
        result.rendered_frames = result.rendered_frames.saturating_add(1);
    }

    result
}

#[cfg(test)]
mod tests {
    use std::sync::Arc;

    use super::{render_supply, RenderSupplyResult};
    use crate::core::player::handle::SemiPlayerHandle;
    use crate::render::core::frame::{PixelFormatCategory, VideoFrame, VideoSurface};

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
        player.runtime.push_decoded_video_frame(decoded_frame(33_000));

        let result = render_supply(&mut player);

        assert_eq!(result, RenderSupplyResult { rendered_frames: 2 });
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

        assert_eq!(result, RenderSupplyResult { rendered_frames: 1 });
        assert_eq!(player.runtime.presentation_video_queue_len(), 1);
    }
}
