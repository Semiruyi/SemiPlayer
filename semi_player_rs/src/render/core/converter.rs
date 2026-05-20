use std::fmt;

use crate::render::core::frame::{
    DecodedVideoFrame, PixelFormatCategory, PresentationFrame, VideoSurfaceKind,
};
use crate::render::pipelines::cpu_bgra;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum ConversionRequest {
    Passthrough,
    Convert {
        target_pixel_format: PixelFormatCategory,
        target_surface_kind: VideoSurfaceKind,
    },
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct FrameConverterSnapshot {
    pub conversion_attempts: u64,
    pub successful_conversions: u64,
    pub backend_unavailable_errors: u64,
}

pub(crate) trait FrameConverter: Send + fmt::Debug {
    fn convert(
        &mut self,
        frame: DecodedVideoFrame,
        request: ConversionRequest,
    ) -> Result<PresentationFrame, DecodedVideoFrame>;

    fn snapshot(&self) -> FrameConverterSnapshot;
}

#[derive(Debug, Default)]
pub struct NoopFrameConverter;

impl FrameConverter for NoopFrameConverter {
    fn convert(
        &mut self,
        frame: DecodedVideoFrame,
        request: ConversionRequest,
    ) -> Result<PresentationFrame, DecodedVideoFrame> {
        match request {
            ConversionRequest::Passthrough => Ok(frame),
            ConversionRequest::Convert {
                target_pixel_format: PixelFormatCategory::Bgra8,
                target_surface_kind: VideoSurfaceKind::CpuPacked,
            } => cpu_bgra::try_render(frame),
            _ => Err(frame),
        }
    }

    fn snapshot(&self) -> FrameConverterSnapshot {
        FrameConverterSnapshot::default()
    }
}

#[cfg(test)]
pub fn create_noop_converter() -> Box<dyn FrameConverter> {
    Box::new(NoopFrameConverter)
}
