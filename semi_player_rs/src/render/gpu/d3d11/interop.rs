use std::mem::ManuallyDrop;
use std::ptr;
use std::sync::Arc;

use crate::render::core::frame::{
    DecodedVideoFrame, VideoFrame, VideoSurface, VideoSurfaceStorage,
};
use crate::render::gpu::{
    GpuBackendKind, GpuDeviceError, GpuRenderError, GpuTextureData, GpuTextureLease,
};

use super::device::D3d11DeviceContext;

#[repr(C)]
struct AvD3d11vaDeviceContext {
    device: *mut std::ffi::c_void,
    device_context: *mut std::ffi::c_void,
    lock: Option<unsafe extern "C" fn(*mut std::ffi::c_void)>,
    unlock: Option<unsafe extern "C" fn(*mut std::ffi::c_void)>,
    lock_ctx: *mut std::ffi::c_void,
}

pub(crate) fn create_ffmpeg_hw_device_ctx(
    context: &D3d11DeviceContext,
) -> Result<*mut ffmpeg_next::ffi::AVBufferRef, GpuDeviceError> {
    use ffmpeg_next::ffi::{
        av_buffer_unref, av_hwdevice_ctx_alloc, av_hwdevice_ctx_init, AVHWDeviceContext,
        AVHWDeviceType,
    };
    use windows::core::Interface;

    let hw_device_ref = unsafe { av_hwdevice_ctx_alloc(AVHWDeviceType::AV_HWDEVICE_TYPE_D3D11VA) };
    if hw_device_ref.is_null() {
        return Err(GpuDeviceError::HwContextAllocFailed);
    }

    let hw_device_ctx = unsafe { (*hw_device_ref).data as *mut AVHWDeviceContext };
    let d3d11_ctx = unsafe { (*hw_device_ctx).hwctx as *mut AvD3d11vaDeviceContext };

    let device_for_ffmpeg = context.device.clone();
    let ctx_for_ffmpeg = context.device_context.clone();

    unsafe {
        (*d3d11_ctx).device = device_for_ffmpeg.as_raw();
        (*d3d11_ctx).device_context = ctx_for_ffmpeg.as_raw();
        (*d3d11_ctx).lock = None;
        (*d3d11_ctx).unlock = None;
        (*d3d11_ctx).lock_ctx = ptr::null_mut();

        std::mem::forget(device_for_ffmpeg);
        std::mem::forget(ctx_for_ffmpeg);

        let init_result = av_hwdevice_ctx_init(hw_device_ref);
        if init_result < 0 {
            av_buffer_unref(&mut (hw_device_ref as *mut _));
            return Err(GpuDeviceError::HwContextInitFailed(init_result));
        }
    }

    Ok(hw_device_ref)
}

pub(crate) fn copy_frame_texture(
    context: &D3d11DeviceContext,
    frame: &DecodedVideoFrame,
) -> Result<DecodedVideoFrame, GpuRenderError> {
    let gpu_data = match &frame.surface.storage {
        VideoSurfaceStorage::GpuTexture(data) => data,
        VideoSurfaceStorage::CpuPacked { .. } => return Ok(frame.clone()),
    };

    context.with_multithread_guard(|| {
        use windows::Win32::Graphics::Direct3D11::{
            ID3D11Resource, ID3D11Texture2D, D3D11_TEXTURE2D_DESC, D3D11_USAGE_DEFAULT,
        };
        use windows::core::Interface;

        if gpu_data.backend() != GpuBackendKind::D3d11 {
            return Err(GpuRenderError::UnsupportedInput);
        }

        let texture_ptr = gpu_data.texture_ptr;
        let array_slice = gpu_data.array_slice;

        let source_texture: ID3D11Texture2D = unsafe { Interface::from_raw(texture_ptr as *mut _) };
        let source = ManuallyDrop::new(source_texture);

        let mut source_desc = D3D11_TEXTURE2D_DESC::default();
        unsafe {
            source.GetDesc(&mut source_desc);
        }

        let owned_desc = D3D11_TEXTURE2D_DESC {
            Width: source_desc.Width,
            Height: source_desc.Height,
            MipLevels: 1,
            ArraySize: 1,
            Format: source_desc.Format,
            SampleDesc: source_desc.SampleDesc,
            Usage: D3D11_USAGE_DEFAULT,
            BindFlags: 0,
            CPUAccessFlags: 0,
            MiscFlags: 0,
        };

        let mut owned_texture: Option<ID3D11Texture2D> = None;
        unsafe {
            context
                .device
                .CreateTexture2D(&owned_desc, None, Some(&mut owned_texture))
                .map_err(|_| GpuRenderError::BackendUnavailable)?;
        }
        let owned_texture = owned_texture.ok_or(GpuRenderError::BackendUnavailable)?;

        unsafe {
            let src_resource: ID3D11Resource = source
                .cast()
                .map_err(|_| GpuRenderError::BackendUnavailable)?;
            let dst_resource: ID3D11Resource = owned_texture
                .cast()
                .map_err(|_| GpuRenderError::BackendUnavailable)?;
            context.device_context.CopySubresourceRegion(
                &dst_resource,
                0,
                0,
                0,
                0,
                &src_resource,
                array_slice,
                None,
            );
        }

        let owned_ptr = owned_texture.as_raw() as usize as u64;
        std::mem::forget(owned_texture);

        Ok(VideoFrame {
            pts_us: frame.pts_us,
            duration_us: frame.duration_us,
            width: frame.width,
            height: frame.height,
            is_key_frame: frame.is_key_frame,
            surface: Arc::new(
                VideoSurface::new_gpu_texture(
                    frame.pixel_format(),
                    GpuTextureData::new(
                        GpuBackendKind::D3d11,
                        owned_ptr,
                        None,
                        0,
                        Some(GpuTextureLease::new(GpuBackendKind::D3d11, owned_ptr)),
                    ),
                )
                .with_color_info(frame.color_info()),
            ),
        })
    })
}

pub(crate) fn nv12_to_bgra_via_swscale(
    y_ptr: *const u8,
    uv_ptr: *const u8,
    y_pitch: i32,
    uv_pitch: i32,
    width: u32,
    height: u32,
) -> Option<Vec<u8>> {
    use ffmpeg_next::ffi;

    let sws_ctx = unsafe {
        ffi::sws_getContext(
            width as i32,
            height as i32,
            ffi::AVPixelFormat::AV_PIX_FMT_NV12,
            width as i32,
            height as i32,
            ffi::AVPixelFormat::AV_PIX_FMT_BGRA,
            ffi::SWS_BILINEAR as i32,
            ptr::null_mut(),
            ptr::null_mut(),
            ptr::null_mut(),
        )
    };
    if sws_ctx.is_null() {
        return None;
    }

    let stride = width as usize * 4;
    let mut dst = vec![0u8; stride * height as usize];
    let src_data = [y_ptr, uv_ptr];
    let src_stride = [y_pitch, uv_pitch];
    let dst_data = [dst.as_mut_ptr()];
    let dst_stride = [stride as i32];

    unsafe {
        ffi::sws_scale(
            sws_ctx,
            src_data.as_ptr(),
            src_stride.as_ptr(),
            0,
            height as i32,
            dst_data.as_ptr(),
            dst_stride.as_ptr(),
        );
        ffi::sws_freeContext(sws_ctx);
    }

    Some(dst)
}
