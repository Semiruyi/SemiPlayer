use crate::render::backends::d3d11::D3d11Renderer;
use crate::render::core::frame::{DecodedVideoFrame, PresentationFrame};

pub fn try_render(
    frame: DecodedVideoFrame,
    d3d11_renderer: &mut D3d11Renderer,
) -> Result<PresentationFrame, DecodedVideoFrame> {
    match d3d11_renderer.render_frame(&frame) {
        Ok(rendered_frame) => Ok(rendered_frame),
        Err(_) => Err(frame),
    }
}
