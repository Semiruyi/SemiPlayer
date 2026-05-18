use crate::render::core::frame::{
    DecodedVideoFrame, PixelFormatCategory, PresentationFrame, VideoSurfaceKind,
};

#[allow(dead_code)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum PresentationPixelFormatPreference {
    PreserveInput,
    Bgra8,
}

impl Default for PresentationPixelFormatPreference {
    fn default() -> Self {
        Self::PreserveInput
    }
}

#[allow(dead_code)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum PresentationSurfaceKindPreference {
    PreserveInput,
    CpuPacked,
    D3d11Texture2D,
}

impl Default for PresentationSurfaceKindPreference {
    fn default() -> Self {
        Self::PreserveInput
    }
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct VideoRenderRequest {
    pub presentation_pixel_format: PresentationPixelFormatPreference,
    pub presentation_surface_kind: PresentationSurfaceKindPreference,
    pub subtitles_visible: bool,
}

#[derive(Clone, Copy, Debug, Default)]
pub struct VideoRenderPipeline;

impl VideoRenderPipeline {
    pub fn new() -> Self {
        Self
    }

    pub fn render_frame(
        &self,
        request: VideoRenderRequest,
        frame: DecodedVideoFrame,
    ) -> PresentationFrame {
        let _target_pixel_format = self.resolve_target_pixel_format(request, &frame);
        let _target_surface_kind = self.resolve_target_surface_kind(request, &frame);
        frame
    }

    pub fn render_frames(
        &self,
        request: VideoRenderRequest,
        frames: impl IntoIterator<Item = DecodedVideoFrame>,
    ) -> Vec<PresentationFrame> {
        frames
            .into_iter()
            .map(|frame| self.render_frame(request, frame))
            .collect()
    }

    fn resolve_target_pixel_format(
        &self,
        request: VideoRenderRequest,
        frame: &DecodedVideoFrame,
    ) -> PixelFormatCategory {
        match request.presentation_pixel_format {
            PresentationPixelFormatPreference::PreserveInput => frame.pixel_format(),
            PresentationPixelFormatPreference::Bgra8 => PixelFormatCategory::Bgra8,
        }
    }

    fn resolve_target_surface_kind(
        &self,
        request: VideoRenderRequest,
        frame: &DecodedVideoFrame,
    ) -> VideoSurfaceKind {
        match request.presentation_surface_kind {
            PresentationSurfaceKindPreference::PreserveInput => frame.surface_kind(),
            PresentationSurfaceKindPreference::CpuPacked => VideoSurfaceKind::CpuPacked,
            PresentationSurfaceKindPreference::D3d11Texture2D => VideoSurfaceKind::D3d11Texture2D,
        }
    }
}

#[cfg(test)]
mod tests {
    use std::sync::Arc;

    use super::{
        PresentationPixelFormatPreference, PresentationSurfaceKindPreference, VideoRenderPipeline,
        VideoRenderRequest,
    };
    use crate::render::core::frame::{
        PixelFormatCategory, VideoFrame, VideoSurface, VideoSurfaceKind,
    };

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
    fn passthrough_pipeline_preserves_timing_and_surface_shape() {
        let pipeline = VideoRenderPipeline::new();
        let input = decoded_frame(33_000);

        let output = pipeline.render_frame(VideoRenderRequest::default(), input.clone());

        assert_eq!(output.pts_us, input.pts_us);
        assert_eq!(output.duration_us, input.duration_us);
        assert_eq!(output.width, input.width);
        assert_eq!(output.height, input.height);
        assert_eq!(output.pixel_format(), input.pixel_format());
        assert_eq!(output.byte_len(), input.byte_len());
    }

    #[test]
    fn request_can_express_bgra_output_preference_without_changing_current_passthrough() {
        let pipeline = VideoRenderPipeline::new();
        let input = decoded_frame(66_000);

        let output = pipeline.render_frame(
            VideoRenderRequest {
                presentation_pixel_format: PresentationPixelFormatPreference::Bgra8,
                presentation_surface_kind: PresentationSurfaceKindPreference::PreserveInput,
                subtitles_visible: true,
            },
            input.clone(),
        );

        assert_eq!(output.pts_us, input.pts_us);
        assert_eq!(output.pixel_format(), input.pixel_format());
    }

    #[test]
    fn request_can_express_d3d11_surface_preference_without_changing_current_passthrough() {
        let pipeline = VideoRenderPipeline::new();
        let input = decoded_frame(99_000);

        let output = pipeline.render_frame(
            VideoRenderRequest {
                presentation_pixel_format: PresentationPixelFormatPreference::PreserveInput,
                presentation_surface_kind: PresentationSurfaceKindPreference::D3d11Texture2D,
                subtitles_visible: false,
            },
            input.clone(),
        );

        assert_eq!(output.pts_us, input.pts_us);
        assert_eq!(output.surface_kind(), VideoSurfaceKind::CpuPacked);
    }
}
