use crate::render::core::frame::{DecodedVideoFrame, PresentationFrame};
use crate::render::core::pipeline::{VideoRenderBatch, VideoRenderPipeline, VideoRenderRequest};
use crate::render::gpu::{GpuDevice, GpuRenderer, GpuRendererSnapshot, NoopGpuRenderer};

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
#[allow(dead_code)]
pub struct RenderServiceSnapshot {
    pub renderer: GpuRendererSnapshot,
}

#[derive(Debug)]
pub struct RenderService {
    pipeline: VideoRenderPipeline,
    renderer: Box<dyn GpuRenderer>,
}

impl Default for RenderService {
    fn default() -> Self {
        Self {
            pipeline: VideoRenderPipeline::new(),
            renderer: Box::new(NoopGpuRenderer),
        }
    }
}

impl RenderService {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn from_device(device: &dyn GpuDevice) -> Self {
        Self {
            pipeline: VideoRenderPipeline::new(),
            renderer: device.create_renderer(),
        }
    }

    pub fn render_frames(
        &mut self,
        request: VideoRenderRequest,
        frames: impl IntoIterator<Item = DecodedVideoFrame>,
    ) -> VideoRenderBatch {
        self.pipeline
            .render_frames(request, frames, &mut self.renderer)
    }

    #[allow(dead_code)]
    pub fn render_frame(
        &mut self,
        request: VideoRenderRequest,
        frame: DecodedVideoFrame,
    ) -> PresentationFrame {
        self.pipeline
            .render_frame(request, frame, &mut self.renderer)
    }

    #[allow(dead_code)]
    pub fn snapshot(&self) -> RenderServiceSnapshot {
        RenderServiceSnapshot {
            renderer: self.renderer.snapshot(),
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
    use crate::render::gpu::GpuTextureData;

    fn gpu_frame(pixel_format: PixelFormatCategory) -> VideoFrame {
        VideoFrame {
            pts_us: 0,
            duration_us: Some(33_000),
            width: 1920,
            height: 1080,
            is_key_frame: false,
            surface: Arc::new(
                VideoSurface::new_gpu_texture(
                    pixel_format,
                    GpuTextureData::D3d11 {
                        texture_ptr: 0x1234,
                        shared_handle: None,
                        array_slice: 0,
                        lease: None,
                    },
                )
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
    fn render_service_uses_noop_renderer_without_device() {
        let mut render = RenderService::new();

        let _ = render.render_frame(
            VideoRenderRequest::d3d11_bgra_presenter(false),
            gpu_frame(PixelFormatCategory::Nv12),
        );
        let _ = render.render_frame(
            VideoRenderRequest::d3d11_bgra_presenter(false),
            cpu_frame(PixelFormatCategory::Bgra8),
        );

        assert_eq!(
            render.snapshot(),
            RenderServiceSnapshot {
                renderer: crate::render::gpu::GpuRendererSnapshot {
                    render_attempts: 0,
                    successful_renders: 0,
                    backend_unavailable_errors: 0,
                },
            }
        );
    }
}
