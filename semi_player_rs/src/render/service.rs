use crate::render::backends::d3d11::{D3d11Renderer, D3d11RendererStateSnapshot};
use crate::render::core::frame::{DecodedVideoFrame, VideoFrame};
use crate::render::core::pipeline::{VideoRenderBatch, VideoRenderPipeline, VideoRenderRequest};

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
#[allow(dead_code)]
pub struct RenderServiceSnapshot {
    pub d3d11_renderer: D3d11RendererStateSnapshot,
}

#[derive(Debug, Default)]
pub struct RenderService {
    pipeline: VideoRenderPipeline,
    d3d11_renderer: D3d11Renderer,
}

impl RenderService {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn render_frames(
        &mut self,
        request: VideoRenderRequest,
        frames: impl IntoIterator<Item = DecodedVideoFrame>,
    ) -> VideoRenderBatch {
        self.pipeline
            .render_frames_with_d3d11_renderer(request, frames, &mut self.d3d11_renderer)
    }

    #[allow(dead_code)]
    pub fn render_frame(
        &mut self,
        request: VideoRenderRequest,
        frame: DecodedVideoFrame,
    ) -> VideoFrame {
        self.pipeline
            .render_frame_with_d3d11_renderer(request, frame, &mut self.d3d11_renderer)
    }

    #[allow(dead_code)]
    pub fn snapshot(&self) -> RenderServiceSnapshot {
        RenderServiceSnapshot {
            d3d11_renderer: self.d3d11_renderer.snapshot(),
        }
    }
}

#[cfg(test)]
mod tests {
    use std::sync::Arc;

    use super::{RenderService, RenderServiceSnapshot};
    use crate::render::core::frame::{
        PixelFormatCategory, VideoColorInfo, VideoFrame, VideoSurface,
    };
    use crate::render::core::pipeline::VideoRenderRequest;

    fn d3d11_frame(pixel_format: PixelFormatCategory) -> VideoFrame {
        VideoFrame {
            pts_us: 0,
            duration_us: Some(33_000),
            width: 1920,
            height: 1080,
            is_key_frame: false,
            surface: Arc::new(
                VideoSurface::new_d3d11_texture_2d(pixel_format, 0x1234, None, 0)
                    .with_color_info(VideoColorInfo::default()),
            ),
        }
    }

    fn cpu_frame(pixel_format: PixelFormatCategory) -> VideoFrame {
        VideoFrame {
            pts_us: 0,
            duration_us: Some(33_000),
            width: 2,
            height: 1,
            is_key_frame: false,
            surface: Arc::new(VideoSurface::new_cpu_packed(pixel_format, 8, vec![0; 8])),
        }
    }

    #[test]
    fn render_service_keeps_d3d11_renderer_state_across_calls() {
        let mut render = RenderService::new();

        let _ = render.render_frame(
            VideoRenderRequest::d3d11_bgra_presenter(false),
            d3d11_frame(PixelFormatCategory::Nv12),
        );
        let _ = render.render_frame(
            VideoRenderRequest::d3d11_bgra_presenter(false),
            cpu_frame(PixelFormatCategory::Bgra8),
        );

        assert_eq!(
            render.snapshot(),
            RenderServiceSnapshot {
                d3d11_renderer: crate::render::backends::d3d11::D3d11RendererStateSnapshot {
                    render_attempts: 2,
                    successful_renders: 0,
                    backend_unavailable_errors: 1,
                    output_pool_hint: 0,
                },
            }
        );
    }
}
