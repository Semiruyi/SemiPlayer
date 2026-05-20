use std::sync::Arc;

use crate::render::core::frame::{DecodedVideoFrame, PresentationFrame};
use crate::render::core::pipeline::{VideoRenderBatch, VideoRenderPipeline, VideoRenderRequest};
use crate::render::gpu::{
    GpuRenderer, GpuRendererSnapshot, NoopGpuRenderer, RenderBackend, RenderBackendCapabilities,
};

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
#[allow(dead_code)]
pub struct RenderServiceSnapshot {
    pub renderer: GpuRendererSnapshot,
    pub backend_capabilities: RenderBackendCapabilities,
}

pub struct RenderService {
    pipeline: VideoRenderPipeline,
    renderer: Box<dyn GpuRenderer>,
    backend_capabilities: RenderBackendCapabilities,
    backend: Option<Arc<dyn RenderBackend>>,
}

impl std::fmt::Debug for RenderService {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("RenderService")
            .field("pipeline", &self.pipeline)
            .field("renderer", &self.renderer)
            .field("backend_capabilities", &self.backend_capabilities)
            .field("has_backend", &self.backend.is_some())
            .finish()
    }
}

impl Default for RenderService {
    fn default() -> Self {
        Self {
            pipeline: VideoRenderPipeline::new(),
            renderer: Box::new(NoopGpuRenderer),
            backend_capabilities: RenderBackendCapabilities::default(),
            backend: None,
        }
    }
}

impl RenderService {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn from_backend(backend: Arc<dyn RenderBackend>) -> Self {
        Self {
            pipeline: VideoRenderPipeline::new(),
            renderer: backend.create_renderer(),
            backend_capabilities: backend.capabilities(),
            backend: Some(backend),
        }
    }

    pub fn prepare_decoded_frame_for_runtime(&self, frame: DecodedVideoFrame) -> DecodedVideoFrame {
        let Some(backend) = self.backend.as_ref() else {
            return frame;
        };

        match backend.copy_frame_to_owned_texture(&frame) {
            Ok(owned_frame) => owned_frame,
            Err(_) => frame,
        }
    }

    pub fn render_frames(
        &mut self,
        request: VideoRenderRequest,
        frames: impl IntoIterator<Item = DecodedVideoFrame>,
    ) -> VideoRenderBatch {
        self.pipeline
            .render_frames(request, frames, self.backend_capabilities, &mut self.renderer)
    }

    #[allow(dead_code)]
    pub fn render_frame(
        &mut self,
        request: VideoRenderRequest,
        frame: DecodedVideoFrame,
    ) -> PresentationFrame {
        self.pipeline
            .render_frame(request, frame, self.backend_capabilities, &mut self.renderer)
    }

    #[allow(dead_code)]
    pub fn snapshot(&self) -> RenderServiceSnapshot {
        RenderServiceSnapshot {
            renderer: self.renderer.snapshot(),
            backend_capabilities: self.backend_capabilities,
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
    use crate::render::gpu::{GpuBackendKind, GpuTextureData, RenderBackendCapabilities};

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
                    GpuTextureData::new(GpuBackendKind::D3d11, 0x1234, None, 0, None),
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
            VideoRenderRequest::gpu_bgra_presenter(false),
            gpu_frame(PixelFormatCategory::Nv12),
        );
        let _ = render.render_frame(
            VideoRenderRequest::gpu_bgra_presenter(false),
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
                backend_capabilities: RenderBackendCapabilities::default(),
            }
        );
    }
}
