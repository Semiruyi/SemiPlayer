use std::sync::Arc;

use crate::render::core::converter::{
    ConversionRequest, FrameConverter, FrameConverterSnapshot, NoopFrameConverter,
};
use crate::render::core::frame::{DecodedVideoFrame, PresentationFrame};
use crate::render::core::pipeline::{VideoRenderBatch, VideoRenderPath, VideoRenderPipeline, VideoRenderRequest};
use crate::render::gpu::{RenderBackend, RenderBackendCapabilities};

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
#[allow(dead_code)]
pub struct RenderServiceSnapshot {
    pub converter: FrameConverterSnapshot,
    pub backend_capabilities: RenderBackendCapabilities,
}

pub struct RenderService {
    pipeline: VideoRenderPipeline,
    converter: Box<dyn FrameConverter>,
    backend_capabilities: RenderBackendCapabilities,
    backend: Option<Arc<dyn RenderBackend>>,
}

impl std::fmt::Debug for RenderService {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("RenderService")
            .field("pipeline", &self.pipeline)
            .field("converter", &self.converter)
            .field("backend_capabilities", &self.backend_capabilities)
            .field("has_backend", &self.backend.is_some())
            .finish()
    }
}

impl Default for RenderService {
    fn default() -> Self {
        Self {
            pipeline: VideoRenderPipeline::new(),
            converter: Box::new(NoopFrameConverter),
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
            converter: backend.create_converter(),
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
        let mut batch = VideoRenderBatch::default();

        for frame in frames {
            let plan = self.pipeline.plan_render(request, &frame, self.backend_capabilities);
            match plan.path {
                VideoRenderPath::Passthrough => {
                    batch.stats.passthrough_frames =
                        batch.stats.passthrough_frames.saturating_add(1);
                }
                VideoRenderPath::PassthroughWithSubtitleIntent => {
                    batch.stats.passthrough_with_subtitle_intent_frames = batch
                        .stats
                        .passthrough_with_subtitle_intent_frames
                        .saturating_add(1);
                }
                VideoRenderPath::RequiresTransform => {
                    batch.stats.requires_transform_frames =
                        batch.stats.requires_transform_frames.saturating_add(1);
                }
                VideoRenderPath::UnsupportedTransform => {}
            }

            let result = match plan.request {
                ConversionRequest::Passthrough => Ok(frame),
                req => self.converter.convert(frame, req),
            };

            let (rendered_frame, fell_back) = match result {
                Ok(presentation_frame) => (presentation_frame, false),
                Err(original_frame) => (original_frame, true),
            };

            if fell_back {
                batch.stats.fallback_passthrough_frames =
                    batch.stats.fallback_passthrough_frames.saturating_add(1);
            }
            batch.frames.push(rendered_frame);
            batch.stats.rendered_frames = batch.stats.rendered_frames.saturating_add(1);
        }

        batch
    }

    #[allow(dead_code)]
    pub fn render_frame(
        &mut self,
        request: VideoRenderRequest,
        frame: DecodedVideoFrame,
    ) -> PresentationFrame {
        let plan = self.pipeline.plan_render(request, &frame, self.backend_capabilities);
        match self.converter.convert(frame, plan.request) {
            Ok(presentation_frame) => presentation_frame,
            Err(original_frame) => original_frame,
        }
    }

    #[allow(dead_code)]
    pub fn snapshot(&self) -> RenderServiceSnapshot {
        RenderServiceSnapshot {
            converter: self.converter.snapshot(),
            backend_capabilities: self.backend_capabilities,
        }
    }
}

#[cfg(test)]
mod tests {
    use std::sync::Arc;

    use super::{RenderService, RenderServiceSnapshot};
    use crate::render::core::converter::FrameConverterSnapshot;
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
    fn render_service_uses_noop_converter_without_device() {
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
                converter: FrameConverterSnapshot::default(),
                backend_capabilities: RenderBackendCapabilities::default(),
            }
        );
    }

    #[test]
    fn render_service_converts_rgba_to_bgra_via_noop_converter() {
        use crate::render::core::frame::VideoSurfaceKind;

        let mut render = RenderService::new();

        let output = render.render_frame(
            VideoRenderRequest::cpu_bgra_compatibility(false),
            cpu_frame(PixelFormatCategory::Rgba8),
        );

        assert_eq!(output.pixel_format(), PixelFormatCategory::Bgra8);
        assert_eq!(output.surface_kind(), VideoSurfaceKind::CpuPacked);
    }
}
