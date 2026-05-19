mod d3d11;

pub use d3d11::D3d11GpuDevice;

use std::fmt;
use std::sync::Arc;

use crate::render::core::frame::{DecodedVideoFrame, PresentationFrame};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum GpuBackendKind {
    D3d11,
}

#[derive(Clone, Debug)]
pub enum GpuTextureData {
    D3d11 {
        texture_ptr: u64,
        shared_handle: Option<u64>,
        array_slice: u32,
    },
}

impl GpuTextureData {
    pub fn backend(&self) -> GpuBackendKind {
        match self {
            Self::D3d11 { .. } => GpuBackendKind::D3d11,
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum GpuDeviceError {
    DeviceCreationFailed,
    HwContextAllocFailed,
    HwContextInitFailed(i32),
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum GpuRenderError {
    UnsupportedInput,
    UnsupportedPixelFormat,
    BackendUnavailable,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct GpuRendererSnapshot {
    pub render_attempts: u64,
    pub successful_renders: u64,
    pub backend_unavailable_errors: u64,
}

pub trait GpuDevice: Send + Sync {
    fn backend_kind(&self) -> GpuBackendKind;
    fn create_ffmpeg_hw_device_ctx(
        &self,
    ) -> Result<*mut ffmpeg_next::ffi::AVBufferRef, GpuDeviceError>;
    fn create_renderer(&self) -> Box<dyn GpuRenderer>;
}

pub trait GpuRenderer: Send + fmt::Debug {
    fn render_frame(
        &mut self,
        frame: &DecodedVideoFrame,
    ) -> Result<PresentationFrame, GpuRenderError>;
    fn snapshot(&self) -> GpuRendererSnapshot;
}

pub fn create_default_device() -> Result<Arc<dyn GpuDevice>, GpuDeviceError> {
    #[cfg(windows)]
    {
        let device = D3d11GpuDevice::new()?;
        Ok(Arc::new(device))
    }
    #[cfg(not(windows))]
    {
        Err(GpuDeviceError::DeviceCreationFailed)
    }
}

/// Renderer that always returns `BackendUnavailable`. Used when no GPU device is available
/// or in tests that don't need GPU rendering.
#[derive(Debug, Default)]
pub struct NoopGpuRenderer;

impl GpuRenderer for NoopGpuRenderer {
    fn render_frame(
        &mut self,
        _frame: &DecodedVideoFrame,
    ) -> Result<PresentationFrame, GpuRenderError> {
        Err(GpuRenderError::BackendUnavailable)
    }

    fn snapshot(&self) -> GpuRendererSnapshot {
        GpuRendererSnapshot::default()
    }
}

#[cfg(test)]
pub fn create_noop_renderer() -> Box<dyn GpuRenderer> {
    Box::new(NoopGpuRenderer)
}
