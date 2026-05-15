use crate::util::time::MediaTimeUs;

#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PixelFormatCategory {
    Yuv420p = 1,
    Nv12 = 2,
    Rgba8 = 3,
    Bgra8 = 4,
    Gray8 = 5,
    Unknown = 0,
}

impl PixelFormatCategory {
    pub const fn as_raw(self) -> u32 {
        self as u32
    }
}

#[derive(Clone, Debug)]
pub struct VideoFrame {
    pub pts_us: MediaTimeUs,
    pub duration_us: Option<MediaTimeUs>,
    pub width: u32,
    pub height: u32,
    pub pixel_format: PixelFormatCategory,
    pub stride: usize,
    pub data: Vec<u8>,
    pub is_key_frame: bool,
}

impl VideoFrame {
    pub fn end_time_us(&self) -> Option<MediaTimeUs> {
        self.duration_us
            .map(|duration_us| self.pts_us.saturating_add(duration_us))
    }

    pub fn byte_len(&self) -> usize {
        self.data.len()
    }
}
