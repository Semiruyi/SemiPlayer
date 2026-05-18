use crate::render::backends::d3d11::D3d11Renderer;
use crate::render::core::frame::DecodedVideoFrame;
use crate::render::core::pipeline::{VideoRenderBatch, VideoRenderPipeline, VideoRenderRequest};

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
}
