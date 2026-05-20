use std::sync::Arc;

use crate::render::gpu::{GpuTextureData, GpuTextureExportDesc, GpuTextureView};
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

#[repr(u32)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[allow(dead_code)]
pub enum VideoColorRange {
    Limited = 1,
    Full = 2,
    #[default]
    Unknown = 0,
}

#[repr(u32)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[allow(dead_code)]
pub enum VideoColorPrimaries {
    Bt709 = 1,
    Bt601 = 2,
    Bt2020 = 3,
    #[default]
    Unknown = 0,
}

#[repr(u32)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[allow(dead_code)]
pub enum VideoTransferCharacteristic {
    Bt709 = 1,
    Srgb = 2,
    Pq = 3,
    Hlg = 4,
    #[default]
    Unknown = 0,
}

#[repr(u32)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[allow(dead_code)]
pub enum VideoMatrixCoefficients {
    Bt709 = 1,
    Bt601 = 2,
    Bt2020Ncl = 3,
    Rgb = 4,
    #[default]
    Unknown = 0,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct VideoColorInfo {
    pub range: VideoColorRange,
    pub primaries: VideoColorPrimaries,
    pub transfer: VideoTransferCharacteristic,
    pub matrix: VideoMatrixCoefficients,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum VideoSurfaceKind {
    CpuPacked,
    GpuTexture,
}

impl VideoSurfaceKind {
    pub const fn as_raw(self) -> u32 {
        match self {
            Self::CpuPacked => 1,
            Self::GpuTexture => 2,
        }
    }
}

#[derive(Clone, Debug)]
#[allow(dead_code)]
pub enum VideoSurfaceStorage {
    CpuPacked { stride: usize, data: Vec<u8> },
    GpuTexture(GpuTextureData),
}

#[derive(Clone, Debug)]
pub struct VideoSurface {
    pub pixel_format: PixelFormatCategory,
    pub color_info: VideoColorInfo,
    pub storage: VideoSurfaceStorage,
}

impl VideoSurface {
    pub fn new_cpu_packed(pixel_format: PixelFormatCategory, stride: usize, data: Vec<u8>) -> Self {
        Self {
            pixel_format,
            color_info: VideoColorInfo::default(),
            storage: VideoSurfaceStorage::CpuPacked { stride, data },
        }
    }

    pub fn new_gpu_texture(pixel_format: PixelFormatCategory, gpu_data: GpuTextureData) -> Self {
        Self {
            pixel_format,
            color_info: VideoColorInfo::default(),
            storage: VideoSurfaceStorage::GpuTexture(gpu_data),
        }
    }

    #[allow(dead_code)]
    pub fn new_raw_gpu_texture(
        pixel_format: PixelFormatCategory,
        backend_kind: crate::render::gpu::GpuBackendKind,
        texture_ptr: u64,
        shared_handle: Option<u64>,
        array_slice: u32,
    ) -> Self {
        Self {
            pixel_format,
            color_info: VideoColorInfo::default(),
            storage: VideoSurfaceStorage::GpuTexture(GpuTextureData::new(
                backend_kind,
                texture_ptr,
                shared_handle,
                array_slice,
                None,
            )),
        }
    }

    pub fn with_color_info(mut self, color_info: VideoColorInfo) -> Self {
        self.color_info = color_info;
        self
    }

    pub fn stride(&self) -> usize {
        match &self.storage {
            VideoSurfaceStorage::CpuPacked { stride, .. } => *stride,
            VideoSurfaceStorage::GpuTexture(_) => 0,
        }
    }

    pub fn kind(&self) -> VideoSurfaceKind {
        match self.storage {
            VideoSurfaceStorage::CpuPacked { .. } => VideoSurfaceKind::CpuPacked,
            VideoSurfaceStorage::GpuTexture(_) => VideoSurfaceKind::GpuTexture,
        }
    }

    pub fn byte_len(&self) -> usize {
        match &self.storage {
            VideoSurfaceStorage::CpuPacked { data, .. } => data.len(),
            VideoSurfaceStorage::GpuTexture(_) => 0,
        }
    }

    pub fn cpu_packed_data(&self) -> Option<&[u8]> {
        match &self.storage {
            VideoSurfaceStorage::CpuPacked { data, .. } => Some(data.as_slice()),
            VideoSurfaceStorage::GpuTexture(_) => None,
        }
    }

    pub fn gpu_texture_data(&self) -> Option<&GpuTextureData> {
        match &self.storage {
            VideoSurfaceStorage::CpuPacked { .. } => None,
            VideoSurfaceStorage::GpuTexture(data) => Some(data),
        }
    }

    pub fn gpu_texture_view(&self) -> Option<GpuTextureView> {
        self.gpu_texture_data().map(GpuTextureData::view)
    }

    pub fn gpu_texture_export_desc(&self) -> Option<GpuTextureExportDesc> {
        self.gpu_texture_data().map(GpuTextureData::export_desc)
    }

    pub fn color_info(&self) -> VideoColorInfo {
        self.color_info
    }
}

#[derive(Clone, Debug)]
pub struct VideoFrame {
    pub pts_us: MediaTimeUs,
    pub duration_us: Option<MediaTimeUs>,
    pub width: u32,
    pub height: u32,
    pub is_key_frame: bool,
    pub surface: Arc<VideoSurface>,
}

// Decoder-owned frames may remain in decoder-native formats such as NV12.
// The first implementation keeps the storage model shared with presentation frames
// so the pipeline can be split incrementally without destabilizing scheduling.
pub type DecodedVideoFrame = VideoFrame;

// Presentation frames are what runtime scheduling and host-facing paths should
// converge on over time. Today this is still the same underlying frame type.
pub type PresentationFrame = VideoFrame;

impl VideoFrame {
    pub fn surface_kind(&self) -> VideoSurfaceKind {
        self.surface.kind()
    }

    pub fn pixel_format(&self) -> PixelFormatCategory {
        self.surface.pixel_format
    }

    pub fn stride(&self) -> usize {
        self.surface.stride()
    }

    pub fn cpu_packed_data(&self) -> Option<&[u8]> {
        self.surface.cpu_packed_data()
    }

    pub fn color_info(&self) -> VideoColorInfo {
        self.surface.color_info()
    }

    pub fn gpu_texture_view(&self) -> Option<GpuTextureView> {
        self.surface.gpu_texture_view()
    }

    pub fn gpu_texture_export_desc(&self) -> Option<GpuTextureExportDesc> {
        self.surface.gpu_texture_export_desc()
    }

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
        self.surface.byte_len()
    }
}
