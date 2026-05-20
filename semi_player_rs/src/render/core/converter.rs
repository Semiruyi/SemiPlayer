use std::fmt;
use std::sync::Arc;

use crate::render::core::convert::pixel_format;
use crate::render::core::frame::{
    DecodedVideoFrame, PixelFormatCategory, PresentationFrame, VideoFrame, VideoSurface,
    VideoSurfaceKind, VideoSurfaceStorage,
};

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

pub(crate) fn convert_cpu_packed_to_bgra(
    frame: DecodedVideoFrame,
) -> Result<PresentationFrame, DecodedVideoFrame> {
    let VideoSurfaceStorage::CpuPacked { stride, data } = &frame.surface.storage else {
        return Err(frame);
    };

    let transformed_data = match frame.pixel_format() {
        PixelFormatCategory::Rgba8 => Some(pixel_format::convert_rgba8_to_bgra8(data)),
        PixelFormatCategory::Gray8 => Some(pixel_format::convert_gray8_to_bgra8(
            data,
            frame.width as usize,
            frame.height as usize,
            *stride,
        )),
        _ => None,
    };

    let Some(transformed_data) = transformed_data else {
        return Err(frame);
    };

    Ok(VideoFrame {
        pts_us: frame.pts_us,
        duration_us: frame.duration_us,
        width: frame.width,
        height: frame.height,
        is_key_frame: frame.is_key_frame,
        surface: Arc::new(VideoSurface::new_cpu_packed(
            PixelFormatCategory::Bgra8,
            frame.width as usize * 4,
            transformed_data,
        )),
    })
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
            } => convert_cpu_packed_to_bgra(frame),
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
