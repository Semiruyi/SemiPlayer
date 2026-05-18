use std::sync::Arc;

use crate::render::core::frame::{
    DecodedVideoFrame, PixelFormatCategory, PresentationFrame, VideoFrame, VideoSurface,
    VideoSurfaceStorage,
};

pub fn try_render(frame: DecodedVideoFrame) -> Result<PresentationFrame, DecodedVideoFrame> {
    let VideoSurfaceStorage::CpuPacked { stride, data } = &frame.surface.storage else {
        return Err(frame);
    };

    let transformed_data = match frame.pixel_format() {
        PixelFormatCategory::Rgba8 => Some(convert_rgba8_to_bgra8(data)),
        PixelFormatCategory::Gray8 => Some(convert_gray8_to_bgra8(
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

fn convert_rgba8_to_bgra8(data: &[u8]) -> Vec<u8> {
    let mut output = data.to_vec();
    for pixel in output.chunks_exact_mut(4) {
        pixel.swap(0, 2);
    }
    output
}

fn convert_gray8_to_bgra8(data: &[u8], width: usize, height: usize, stride: usize) -> Vec<u8> {
    let mut output = vec![0u8; width.saturating_mul(height).saturating_mul(4)];

    for y in 0..height {
        let src_row_start = y.saturating_mul(stride);
        let src_row_end = src_row_start.saturating_add(width);
        let dst_row_start = y.saturating_mul(width).saturating_mul(4);
        let src_row = &data[src_row_start..src_row_end];

        for (x, gray) in src_row.iter().copied().enumerate() {
            let dst_index = dst_row_start + x * 4;
            output[dst_index] = gray;
            output[dst_index + 1] = gray;
            output[dst_index + 2] = gray;
            output[dst_index + 3] = 255;
        }
    }

    output
}
