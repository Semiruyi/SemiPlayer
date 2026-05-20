use crate::render::core::converter::FrameConverter;
use crate::render::core::frame::DecodedVideoFrame;
use crate::render::gpu::{
    GpuBackendKind, GpuDeviceError, GpuRenderError, RenderBackend, RenderBackendCapabilities,
};

use super::converter::D3d11FrameConverter;
use super::interop;

#[derive(Clone, Debug)]
pub(crate) struct D3d11DeviceContext {
    pub(crate) device: windows::Win32::Graphics::Direct3D11::ID3D11Device,
    pub(crate) device_context: windows::Win32::Graphics::Direct3D11::ID3D11DeviceContext,
    pub(crate) multithread: windows::Win32::Graphics::Direct3D11::ID3D11Multithread,
}

impl D3d11DeviceContext {
    pub(crate) fn with_multithread_guard<T>(&self, f: impl FnOnce() -> T) -> T {
        unsafe {
            self.multithread.Enter();
        }
        let result = f();
        unsafe {
            self.multithread.Leave();
        }
        result
    }
}

#[derive(Debug)]
pub(crate) struct D3d11GpuDevice {
    context: D3d11DeviceContext,
}

impl D3d11GpuDevice {
    pub(crate) fn new() -> Result<Self, GpuDeviceError> {
        use windows::Win32::Foundation::HMODULE;
        use windows::Win32::Graphics::Direct3D::{D3D_DRIVER_TYPE_HARDWARE, D3D_FEATURE_LEVEL};
        use windows::Win32::Graphics::Direct3D11::{
            D3D11CreateDevice, ID3D11Device, ID3D11DeviceContext, ID3D11Multithread,
            D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        };
        use windows::core::Interface;

        let mut device: Option<ID3D11Device> = None;
        let mut device_context: Option<ID3D11DeviceContext> = None;

        let feature_levels = [D3D_FEATURE_LEVEL(0xB100)];

        unsafe {
            D3D11CreateDevice(
                None,
                D3D_DRIVER_TYPE_HARDWARE,
                HMODULE::default(),
                D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                Some(&feature_levels),
                7,
                Some(&mut device),
                None,
                Some(&mut device_context),
            )
            .map_err(|_| GpuDeviceError::DeviceCreationFailed)?;
        }

        let device = device.ok_or(GpuDeviceError::DeviceCreationFailed)?;
        let device_context = device_context.ok_or(GpuDeviceError::DeviceCreationFailed)?;
        let multithread: ID3D11Multithread = device
            .cast()
            .map_err(|_| GpuDeviceError::DeviceCreationFailed)?;

        unsafe {
            let _ = multithread.SetMultithreadProtected(true);
        }

        Ok(Self {
            context: D3d11DeviceContext {
                device,
                device_context,
                multithread,
            },
        })
    }

    #[allow(dead_code)]
    pub(crate) fn raw_device(&self) -> &windows::Win32::Graphics::Direct3D11::ID3D11Device {
        &self.context.device
    }

    #[allow(dead_code)]
    pub(crate) fn raw_device_context(&self) -> &windows::Win32::Graphics::Direct3D11::ID3D11DeviceContext {
        &self.context.device_context
    }
}

impl RenderBackend for D3d11GpuDevice {
    fn backend_kind(&self) -> GpuBackendKind {
        GpuBackendKind::D3d11
    }

    fn capabilities(&self) -> RenderBackendCapabilities {
        RenderBackendCapabilities {
            supports_ffmpeg_hw_device_ctx: true,
            supports_owned_texture_copy: true,
            supports_gpu_bgra_presentation: true,
            supports_nv12_cpu_bgra_conversion: true,
        }
    }

    fn create_ffmpeg_hw_device_ctx(
        &self,
    ) -> Result<*mut ffmpeg_next::ffi::AVBufferRef, GpuDeviceError> {
        interop::create_ffmpeg_hw_device_ctx(&self.context)
    }

    fn create_converter(&self) -> Box<dyn FrameConverter> {
        Box::new(D3d11FrameConverter::new(self.context.clone()))
    }

    fn copy_frame_to_owned_texture(
        &self,
        frame: &DecodedVideoFrame,
    ) -> Result<DecodedVideoFrame, GpuRenderError> {
        interop::copy_frame_texture(&self.context, frame)
    }
}

unsafe impl Send for D3d11GpuDevice {}
unsafe impl Sync for D3d11GpuDevice {}
