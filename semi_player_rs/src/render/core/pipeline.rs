use crate::render::core::frame::{DecodedVideoFrame, PresentationFrame};

#[derive(Clone, Copy, Debug, Default)]
pub struct VideoRenderPipeline;

impl VideoRenderPipeline {
    pub fn new() -> Self {
        Self
    }

    pub fn render_frame(&self, frame: DecodedVideoFrame) -> PresentationFrame {
        frame
    }

    pub fn render_frames(
        &self,
        frames: impl IntoIterator<Item = DecodedVideoFrame>,
    ) -> Vec<PresentationFrame> {
        frames
            .into_iter()
            .map(|frame| self.render_frame(frame))
            .collect()
    }
}

#[cfg(test)]
mod tests {
    use std::sync::Arc;

    use super::VideoRenderPipeline;
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
    fn passthrough_pipeline_preserves_timing_and_surface_shape() {
        let pipeline = VideoRenderPipeline::new();
        let input = decoded_frame(33_000);

        let output = pipeline.render_frame(input.clone());

        assert_eq!(output.pts_us, input.pts_us);
        assert_eq!(output.duration_us, input.duration_us);
        assert_eq!(output.width, input.width);
        assert_eq!(output.height, input.height);
        assert_eq!(output.pixel_format(), input.pixel_format());
        assert_eq!(output.byte_len(), input.byte_len());
    }
}
