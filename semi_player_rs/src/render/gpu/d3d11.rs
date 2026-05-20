use std::mem::ManuallyDrop;
use std::ptr;
use std::sync::Arc;

use crate::render::core::frame::{
    DecodedVideoFrame, PixelFormatCategory, PresentationFrame, VideoFrame, VideoSurface,
    VideoSurfaceStorage,
};
use crate::render::gpu::{
    D3d11TextureLease, GpuBackendKind, GpuDevice, GpuDeviceError, GpuRenderError, GpuRenderer,
    GpuRendererSnapshot, GpuTextureData,
};

#[derive(Debug)]
pub struct D3d11GpuDevice {
    device: windows::Win32::Graphics::Direct3D11::ID3D11Device,
    device_context: windows::Win32::Graphics::Direct3D11::ID3D11DeviceContext,
    multithread: windows::Win32::Graphics::Direct3D11::ID3D11Multithread,
}

#[repr(C)]
struct AvD3d11vaDeviceContext {
    device: *mut std::ffi::c_void,
    device_context: *mut std::ffi::c_void,
    lock: Option<unsafe extern "C" fn(*mut std::ffi::c_void)>,
    unlock: Option<unsafe extern "C" fn(*mut std::ffi::c_void)>,
    lock_ctx: *mut std::ffi::c_void,
}

impl D3d11GpuDevice {
    pub fn new() -> Result<Self, GpuDeviceError> {
        use windows::Win32::Foundation::HMODULE;
        use windows::Win32::Graphics::Direct3D::{D3D_DRIVER_TYPE_HARDWARE, D3D_FEATURE_LEVEL};
        use windows::Win32::Graphics::Direct3D11::{
            D3D11CreateDevice, ID3D11Device, ID3D11DeviceContext, ID3D11Multithread,
            D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        };
        use windows::core::Interface;

        let mut device: Option<ID3D11Device> = None;
        let mut device_context: Option<ID3D11DeviceContext> = None;

        let feature_levels = [D3D_FEATURE_LEVEL(0xB100)]; // D3D_FEATURE_LEVEL_11_0

        unsafe {
            D3D11CreateDevice(
                None,
                D3D_DRIVER_TYPE_HARDWARE,
                HMODULE::default(),
                D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                Some(&feature_levels),
                7, // D3D11_SDK_VERSION
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
            device,
            device_context,
            multithread,
        })
    }

    #[allow(dead_code)]
    pub fn raw_device(&self) -> &windows::Win32::Graphics::Direct3D11::ID3D11Device {
        &self.device
    }

    #[allow(dead_code)]
    pub fn raw_device_context(&self) -> &windows::Win32::Graphics::Direct3D11::ID3D11DeviceContext {
        &self.device_context
    }
}

impl GpuDevice for D3d11GpuDevice {
    fn backend_kind(&self) -> GpuBackendKind {
        GpuBackendKind::D3d11
    }

    fn create_ffmpeg_hw_device_ctx(
        &self,
    ) -> Result<*mut ffmpeg_next::ffi::AVBufferRef, GpuDeviceError> {
        use ffmpeg_next::ffi::{
            av_buffer_unref, av_hwdevice_ctx_alloc, av_hwdevice_ctx_init, AVHWDeviceContext,
            AVHWDeviceType,
        };
        use windows::core::Interface;

        let hw_device_ref =
            unsafe { av_hwdevice_ctx_alloc(AVHWDeviceType::AV_HWDEVICE_TYPE_D3D11VA) };
        if hw_device_ref.is_null() {
            return Err(GpuDeviceError::HwContextAllocFailed);
        }

        let hw_device_ctx = unsafe { (*hw_device_ref).data as *mut AVHWDeviceContext };
        let d3d11_ctx = unsafe { (*hw_device_ctx).hwctx as *mut AvD3d11vaDeviceContext };

        let device_for_ffmpeg = self.device.clone();
        let ctx_for_ffmpeg = self.device_context.clone();

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

    fn create_renderer(&self) -> Box<dyn GpuRenderer> {
        Box::new(D3d11GpuRenderer {
            device: self.device.clone(),
            device_context: self.device_context.clone(),
            multithread: self.multithread.clone(),
            state: RendererState::default(),
        })
    }

    fn copy_frame_to_owned_texture(
        &self,
        frame: &DecodedVideoFrame,
    ) -> Result<DecodedVideoFrame, GpuRenderError> {
        self.copy_frame_texture(frame)
    }
}

unsafe impl Send for D3d11GpuDevice {}
unsafe impl Sync for D3d11GpuDevice {}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
struct RendererState {
    render_attempts: u64,
    successful_renders: u64,
    backend_unavailable_errors: u64,
}

#[derive(Debug)]
pub struct D3d11GpuRenderer {
    device: windows::Win32::Graphics::Direct3D11::ID3D11Device,
    device_context: windows::Win32::Graphics::Direct3D11::ID3D11DeviceContext,
    multithread: windows::Win32::Graphics::Direct3D11::ID3D11Multithread,
    state: RendererState,
}

impl GpuRenderer for D3d11GpuRenderer {
    fn render_frame(
        &mut self,
        frame: &DecodedVideoFrame,
    ) -> Result<PresentationFrame, GpuRenderError> {
        self.state.render_attempts = self.state.render_attempts.saturating_add(1);

        let result = self.execute_render(&frame);
        match result {
            Ok(presentation_frame) => {
                self.state.successful_renders = self.state.successful_renders.saturating_add(1);
                Ok(presentation_frame)
            }
            Err(error) => {
                if error == GpuRenderError::BackendUnavailable {
                    self.state.backend_unavailable_errors =
                        self.state.backend_unavailable_errors.saturating_add(1);
                }
                Err(error)
            }
        }
    }

    fn snapshot(&self) -> GpuRendererSnapshot {
        GpuRendererSnapshot {
            render_attempts: self.state.render_attempts,
            successful_renders: self.state.successful_renders,
            backend_unavailable_errors: self.state.backend_unavailable_errors,
        }
    }
}

impl D3d11GpuRenderer {
    fn execute_render(
        &self,
        frame: &DecodedVideoFrame,
    ) -> Result<PresentationFrame, GpuRenderError> {
        let gpu_data = match &frame.surface.storage {
            VideoSurfaceStorage::GpuTexture(data) => data,
            VideoSurfaceStorage::CpuPacked { .. } => return Err(GpuRenderError::UnsupportedInput),
        };

        match (gpu_data, frame.pixel_format()) {
            (GpuTextureData::D3d11 { .. }, PixelFormatCategory::Bgra8) => {
                self.copy_bgra_texture(frame, gpu_data)
            }
            (GpuTextureData::D3d11 { .. }, PixelFormatCategory::Nv12) => {
                self.nv12_to_cpu_bgra(frame, gpu_data)
            }
            _ => Err(GpuRenderError::UnsupportedPixelFormat),
        }
    }

    fn with_multithread_guard<T>(&self, f: impl FnOnce() -> T) -> T {
        unsafe {
            self.multithread.Enter();
        }
        let result = f();
        unsafe {
            self.multithread.Leave();
        }
        result
    }

    fn copy_bgra_texture(
        &self,
        frame: &DecodedVideoFrame,
        gpu_data: &GpuTextureData,
    ) -> Result<PresentationFrame, GpuRenderError> {
        self.with_multithread_guard(|| {
            let GpuTextureData::D3d11 {
                texture_ptr,
                shared_handle,
                array_slice,
                lease,
            } = gpu_data;

            let result = VideoFrame {
                pts_us: frame.pts_us,
                duration_us: frame.duration_us,
                width: frame.width,
                height: frame.height,
                is_key_frame: frame.is_key_frame,
                surface: Arc::new(
                    VideoSurface::new_gpu_texture(
                        PixelFormatCategory::Bgra8,
                        GpuTextureData::D3d11 {
                            texture_ptr: *texture_ptr,
                            shared_handle: *shared_handle,
                            array_slice: *array_slice,
                            lease: lease.clone(),
                        },
                    )
                    .with_color_info(frame.color_info()),
                ),
            };
            Ok(result)
        })
    }

    fn nv12_to_cpu_bgra(
        &self,
        frame: &DecodedVideoFrame,
        gpu_data: &GpuTextureData,
    ) -> Result<PresentationFrame, GpuRenderError> {
        self.with_multithread_guard(|| {
            use windows::core::Interface;
            use windows::Win32::Graphics::Direct3D11::{
                ID3D11Resource, ID3D11Texture2D, D3D11_CPU_ACCESS_READ, D3D11_MAPPED_SUBRESOURCE,
                D3D11_MAP_READ, D3D11_TEXTURE2D_DESC, D3D11_USAGE_STAGING,
            };
            use windows::Win32::Graphics::Dxgi::Common::{DXGI_FORMAT, DXGI_SAMPLE_DESC};

            let GpuTextureData::D3d11 {
                texture_ptr,
                array_slice,
                ..
            } = gpu_data;

            let width = frame.width;
            let height = frame.height;

            let staging_desc = D3D11_TEXTURE2D_DESC {
                Width: width,
                Height: height,
                MipLevels: 1,
                ArraySize: 1,
                Format: DXGI_FORMAT(103), // DXGI_FORMAT_NV12
                SampleDesc: DXGI_SAMPLE_DESC {
                    Count: 1,
                    Quality: 0,
                },
                Usage: D3D11_USAGE_STAGING,
                BindFlags: 0,
                CPUAccessFlags: D3D11_CPU_ACCESS_READ.0 as u32,
                MiscFlags: 0,
            };

            let mut staging: Option<ID3D11Texture2D> = None;
            unsafe {
                self.device
                    .CreateTexture2D(&staging_desc, None, Some(&mut staging))
                    .map_err(|_| GpuRenderError::BackendUnavailable)?;
            }
            let staging = staging.ok_or(GpuRenderError::BackendUnavailable)?;

            let source_texture: ID3D11Texture2D =
                unsafe { Interface::from_raw(*texture_ptr as *mut _) };
            let source = ManuallyDrop::new(source_texture);

            unsafe {
                let src_resource: ID3D11Resource = source
                    .cast()
                    .map_err(|_| GpuRenderError::BackendUnavailable)?;
                let dst_resource: ID3D11Resource = staging
                    .cast()
                    .map_err(|_| GpuRenderError::BackendUnavailable)?;
                self.device_context.CopySubresourceRegion(
                    &dst_resource,
                    0,
                    0,
                    0,
                    0,
                    &src_resource,
                    *array_slice,
                    None,
                );
            }

            let mut mapped = D3D11_MAPPED_SUBRESOURCE::default();
            unsafe {
                let staging_resource: ID3D11Resource = staging
                    .cast()
                    .map_err(|_| GpuRenderError::BackendUnavailable)?;
                self.device_context
                    .Map(&staging_resource, 0, D3D11_MAP_READ, 0, Some(&mut mapped))
                    .map_err(|_| GpuRenderError::BackendUnavailable)?;
            }

            let y_pitch = mapped.RowPitch as i32;
            let uv_pitch = mapped.RowPitch as i32;
            let y_ptr = mapped.pData as *const u8;
            let uv_ptr = unsafe {
                mapped
                    .pData
                    .add((height as usize) * (mapped.RowPitch as usize)) as *const u8
            };

            let bgra_data =
                nv12_to_bgra_via_swscale(y_ptr, uv_ptr, y_pitch, uv_pitch, width, height);

            unsafe {
                let staging_resource: ID3D11Resource = staging
                    .cast()
                    .map_err(|_| GpuRenderError::BackendUnavailable)?;
                self.device_context.Unmap(&staging_resource, 0);
            }

            let Some(bgra_data) = bgra_data else {
                return Err(GpuRenderError::BackendUnavailable);
            };

            let result = VideoFrame {
                pts_us: frame.pts_us,
                duration_us: frame.duration_us,
                width,
                height,
                is_key_frame: frame.is_key_frame,
                surface: Arc::new(VideoSurface::new_cpu_packed(
                    PixelFormatCategory::Bgra8,
                    width as usize * 4,
                    bgra_data,
                )),
            };
            Ok(result)
        })
    }
}

impl D3d11GpuDevice {
    fn copy_frame_texture(
        &self,
        frame: &DecodedVideoFrame,
    ) -> Result<DecodedVideoFrame, GpuRenderError> {
        let gpu_data = match &frame.surface.storage {
            VideoSurfaceStorage::GpuTexture(data) => data,
            VideoSurfaceStorage::CpuPacked { .. } => return Ok(frame.clone()),
        };

        self.with_multithread_guard(|| {
            use windows::Win32::Graphics::Direct3D11::{
                ID3D11Resource, ID3D11Texture2D, D3D11_TEXTURE2D_DESC, D3D11_USAGE_DEFAULT,
            };
            use windows::core::Interface;

            let GpuTextureData::D3d11 {
                texture_ptr,
                array_slice,
                ..
            } = gpu_data;

            let source_texture: ID3D11Texture2D =
                unsafe { Interface::from_raw(*texture_ptr as *mut _) };
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
                self.device
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
                self.device_context.CopySubresourceRegion(
                    &dst_resource,
                    0,
                    0,
                    0,
                    0,
                    &src_resource,
                    *array_slice,
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
                        GpuTextureData::D3d11 {
                            texture_ptr: owned_ptr,
                            shared_handle: None,
                            array_slice: 0,
                            lease: Some(D3d11TextureLease::new(owned_ptr)),
                        },
                    )
                    .with_color_info(frame.color_info()),
                ),
            })
        })
    }

    fn with_multithread_guard<T>(&self, f: impl FnOnce() -> T) -> T {
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

fn nv12_to_bgra_via_swscale(
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
