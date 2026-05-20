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
pub struct GpuTextureData {
    pub backend_kind: GpuBackendKind,
    pub texture_ptr: u64,
    pub shared_handle: Option<u64>,
    pub array_slice: u32,
    pub lease: Option<Arc<GpuTextureLease>>,
}

impl GpuTextureData {
    pub fn new(
        backend_kind: GpuBackendKind,
        texture_ptr: u64,
        shared_handle: Option<u64>,
        array_slice: u32,
        lease: Option<Arc<GpuTextureLease>>,
    ) -> Self {
        Self {
            backend_kind,
            texture_ptr,
            shared_handle,
            array_slice,
            lease,
        }
    }

    pub fn backend(&self) -> GpuBackendKind {
        self.backend_kind
    }
}

#[derive(Debug)]
pub struct GpuTextureLease {
    backend_kind: GpuBackendKind,
    texture_ptr: u64,
}

impl GpuTextureLease {
    pub fn new(backend_kind: GpuBackendKind, texture_ptr: u64) -> Arc<Self> {
        Arc::new(Self {
            backend_kind,
            texture_ptr,
        })
    }
}

impl Drop for GpuTextureLease {
    fn drop(&mut self) {
        match self.backend_kind {
            GpuBackendKind::D3d11 => {
                #[cfg(windows)]
                unsafe {
                    use windows::Win32::Graphics::Direct3D11::ID3D11Texture2D;
                    use windows::core::Interface;

                    if self.texture_ptr != 0 {
                        let texture = ID3D11Texture2D::from_raw(self.texture_ptr as *mut _);
                        drop(texture);
                    }
                }
            }
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

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct RenderBackendCapabilities {
    pub supports_ffmpeg_hw_device_ctx: bool,
    pub supports_owned_texture_copy: bool,
    pub supports_gpu_bgra_presentation: bool,
    pub supports_nv12_cpu_bgra_conversion: bool,
}

pub trait RenderBackend: Send + Sync {
    fn backend_kind(&self) -> GpuBackendKind;
    fn capabilities(&self) -> RenderBackendCapabilities {
        RenderBackendCapabilities::default()
    }
    fn create_ffmpeg_hw_device_ctx(
        &self,
    ) -> Result<*mut ffmpeg_next::ffi::AVBufferRef, GpuDeviceError>;
    fn create_renderer(&self) -> Box<dyn GpuRenderer>;
    fn copy_frame_to_owned_texture(
        &self,
        frame: &DecodedVideoFrame,
    ) -> Result<DecodedVideoFrame, GpuRenderError> {
        Ok(frame.clone())
    }
}

#[allow(dead_code)]
pub trait GpuDevice: RenderBackend {}

impl<T> GpuDevice for T where T: RenderBackend + ?Sized {}

pub trait GpuRenderer: Send + fmt::Debug {
    fn render_frame(
        &mut self,
        frame: &DecodedVideoFrame,
    ) -> Result<PresentationFrame, GpuRenderError>;
    fn snapshot(&self) -> GpuRendererSnapshot;
}

pub fn create_default_backend() -> Result<Arc<dyn RenderBackend>, GpuDeviceError> {
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

#[allow(dead_code)]
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
