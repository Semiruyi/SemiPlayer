use crate::util::time::MediaTimeUs;

#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[allow(dead_code)]
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

    pub fn effective_end_time_us(&self, next_frame: Option<&VideoFrame>) -> Option<MediaTimeUs> {
        let next_pts_us = next_frame
            .map(|frame| frame.pts_us)
            .filter(|next_pts_us| *next_pts_us > self.pts_us);

        match (self.end_time_us(), next_pts_us) {
            // Prefer the next frame PTS when available; it is the authoritative boundary
            // for how long the current frame should stay on screen.
            (_, Some(next_pts_us)) => Some(next_pts_us),
            (Some(current_end_us), None) => Some(current_end_us),
            (None, None) => None,
        }
    }

    pub fn covers_time_us(&self, target_time_us: MediaTimeUs) -> bool {
        if target_time_us < self.pts_us {
            return false;
        }

        match self.end_time_us() {
            Some(end_time_us) => target_time_us < end_time_us,
            None => true,
        }
    }

    pub fn covers_time_with_next_us(
        &self,
        next_frame: Option<&VideoFrame>,
        target_time_us: MediaTimeUs,
    ) -> bool {
        if target_time_us < self.pts_us {
            return false;
        }

        match self.effective_end_time_us(next_frame) {
            Some(end_time_us) => target_time_us < end_time_us,
            None => true,
        }
    }

    pub fn is_stale_for_time_us(
        &self,
        next_frame: Option<&VideoFrame>,
        target_time_us: MediaTimeUs,
    ) -> bool {
        match self.effective_end_time_us(next_frame) {
            Some(end_time_us) => target_time_us >= end_time_us,
            None => false,
        }
    }

    pub fn byte_len(&self) -> usize {
        self.data.len()
    }
}
