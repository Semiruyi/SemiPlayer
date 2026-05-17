use crate::render::core::frame::{PixelFormatCategory, VideoSurface};

#[allow(dead_code)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct D3d11TextureSurfaceDesc {
    pub texture_ptr: u64,
    pub shared_handle: Option<u64>,
    pub array_slice: u32,
    pub pixel_format: PixelFormatCategory,
}

impl D3d11TextureSurfaceDesc {
    #[allow(dead_code)]
    pub fn into_surface(self) -> VideoSurface {
        VideoSurface::new_d3d11_texture_2d(
            self.pixel_format,
            self.texture_ptr,
            self.shared_handle,
            self.array_slice,
        )
    }
}
