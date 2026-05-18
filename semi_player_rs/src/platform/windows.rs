use std::ptr;

use ffmpeg_next::ffi::{
    av_hwdevice_ctx_alloc, av_hwdevice_ctx_init, av_buffer_unref,
    AVBufferRef, AVHWDeviceContext, AVHWDeviceType,
};
use windows::core::Interface;
use windows::Win32::Foundation::HMODULE;
use windows::Win32::Graphics::Direct3D::{D3D_DRIVER_TYPE_HARDWARE, D3D_FEATURE_LEVEL};
use windows::Win32::Graphics::Direct3D11::{
    D3D11CreateDevice, ID3D11Device, ID3D11DeviceContext,
    D3D11_CREATE_DEVICE_SINGLETHREADED, D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
};

#[derive(Debug)]
pub enum D3d11DeviceError {
    CreationFailed(windows::core::Error),
    HwContextAllocFailed,
    HwContextInitFailed(i32),
}

impl std::fmt::Display for D3d11DeviceError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::CreationFailed(e) => write!(f, "D3D11 device creation failed: {e}"),
            Self::HwContextAllocFailed => write!(f, "FFmpeg D3D11VA hw context allocation failed"),
            Self::HwContextInitFailed(code) => {
                write!(f, "FFmpeg D3D11VA hw context init failed: {code}")
            }
        }
    }
}

impl std::error::Error for D3d11DeviceError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::CreationFailed(e) => Some(e),
            _ => None,
        }
    }
}

#[repr(C)]
struct AvD3d11vaDeviceContext {
    device: *mut std::ffi::c_void,
    device_context: *mut std::ffi::c_void,
    lock: Option<unsafe extern "C" fn(*mut std::ffi::c_void)>,
    unlock: Option<unsafe extern "C" fn(*mut std::ffi::c_void)>,
    lock_ctx: *mut std::ffi::c_void,
}

pub struct D3d11SharedDevice {
    device: ID3D11Device,
    device_context: ID3D11DeviceContext,
}

impl D3d11SharedDevice {
    pub fn new() -> Result<Self, D3d11DeviceError> {
        let mut device: Option<ID3D11Device> = None;
        let mut device_context: Option<ID3D11DeviceContext> = None;

        let feature_levels = [D3D_FEATURE_LEVEL(0xB100)]; // D3D_FEATURE_LEVEL_11_0

        unsafe {
            D3D11CreateDevice(
                None,
                D3D_DRIVER_TYPE_HARDWARE,
                HMODULE::default(),
                D3D11_CREATE_DEVICE_SINGLETHREADED | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                Some(&feature_levels),
                0, // SDK version
                Some(&mut device),
                None,
                Some(&mut device_context),
            )
            .map_err(D3d11DeviceError::CreationFailed)?;
        }

        let device = device.ok_or_else(|| {
            D3d11DeviceError::CreationFailed(windows::core::Error::from_win32())
        })?;
        let device_context = device_context.ok_or_else(|| {
            D3d11DeviceError::CreationFailed(windows::core::Error::from_win32())
        })?;

        Ok(Self {
            device,
            device_context,
        })
    }

    #[allow(dead_code)]
    pub fn device(&self) -> &ID3D11Device {
        &self.device
    }

    #[allow(dead_code)]
    pub fn device_context(&self) -> &ID3D11DeviceContext {
        &self.device_context
    }

    pub fn create_ffmpeg_hw_device_ctx(&self) -> Result<*mut AVBufferRef, D3d11DeviceError> {
        let hw_device_ref =
            unsafe { av_hwdevice_ctx_alloc(AVHWDeviceType::AV_HWDEVICE_TYPE_D3D11VA) };
        if hw_device_ref.is_null() {
            return Err(D3d11DeviceError::HwContextAllocFailed);
        }

        let hw_device_ctx =
            unsafe { (*hw_device_ref).data as *mut AVHWDeviceContext };
        let d3d11_ctx = unsafe { (*hw_device_ctx).hwctx as *mut AvD3d11vaDeviceContext };

        // Clone COM objects to AddRef before handing to FFmpeg.
        // FFmpeg's d3d11va_device_uninit will Release them.
        let device_for_ffmpeg = self.device.clone();
        let ctx_for_ffmpeg = self.device_context.clone();

        unsafe {
            (*d3d11_ctx).device = device_for_ffmpeg.as_raw();
            (*d3d11_ctx).device_context = ctx_for_ffmpeg.as_raw();
            (*d3d11_ctx).lock = None;
            (*d3d11_ctx).unlock = None;
            (*d3d11_ctx).lock_ctx = ptr::null_mut();

            // Prevent the clones from Release-ing on drop.
            // FFmpeg owns these references now.
            std::mem::forget(device_for_ffmpeg);
            std::mem::forget(ctx_for_ffmpeg);

            let init_result = av_hwdevice_ctx_init(hw_device_ref);
            if init_result < 0 {
                av_buffer_unref(&mut (hw_device_ref as *mut _));
                return Err(D3d11DeviceError::HwContextInitFailed(init_result));
            }
        }

        Ok(hw_device_ref)
    }
}

unsafe impl Send for D3d11SharedDevice {}
