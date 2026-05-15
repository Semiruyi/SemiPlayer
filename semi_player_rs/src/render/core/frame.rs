use crate::util::time::MediaTimeUs;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PixelFormatCategory {
    Yuv420p,
    Nv12,
    Rgba8,
    Bgra8,
    Gray8,
    Unknown,
}

#[derive(Clone, Debug)]
pub struct VideoFrame {
    pub pts_us: MediaTimeUs,
    pub duration_us: Option<MediaTimeUs>,
    pub width: u32,
    pub height: u32,
    pub pixel_format: PixelFormatCategory,
    pub is_key_frame: bool,
}

impl VideoFrame {
    pub fn end_time_us(&self) -> Option<MediaTimeUs> {
        self.duration_us.map(|duration_us| self.pts_us.saturating_add(duration_us))
    }
}
