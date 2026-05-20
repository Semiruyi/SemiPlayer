use std::mem::ManuallyDrop;
use std::sync::Arc;

use crate::render::core::frame::{
    DecodedVideoFrame, PixelFormatCategory, PresentationFrame, VideoFrame, VideoSurface,
    VideoSurfaceStorage,
};
use crate::render::gpu::{
    GpuBackendKind, GpuRenderError, GpuRenderer, GpuRendererSnapshot, GpuTextureData,
};

use super::device::D3d11DeviceContext;
use super::interop::nv12_to_bgra_via_swscale;

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
struct RendererState {
    render_attempts: u64,
    successful_renders: u64,
    backend_unavailable_errors: u64,
}

#[derive(Debug)]
pub struct D3d11GpuRenderer {
    context: D3d11DeviceContext,
    state: RendererState,
}

impl D3d11GpuRenderer {
    pub(crate) fn new(context: D3d11DeviceContext) -> Self {
        Self {
            context,
            state: RendererState::default(),
        }
    }

    fn execute_render(&self, frame: &DecodedVideoFrame) -> Result<PresentationFrame, GpuRenderError> {
        let gpu_data = match &frame.surface.storage {
            VideoSurfaceStorage::GpuTexture(data) => data,
            VideoSurfaceStorage::CpuPacked { .. } => return Err(GpuRenderError::UnsupportedInput),
        };

        if gpu_data.backend() != GpuBackendKind::D3d11 {
            return Err(GpuRenderError::UnsupportedInput);
        }

        match frame.pixel_format() {
            PixelFormatCategory::Bgra8 => self.copy_bgra_texture(frame, gpu_data),
            PixelFormatCategory::Nv12 => self.nv12_to_cpu_bgra(frame, gpu_data),
            _ => Err(GpuRenderError::UnsupportedPixelFormat),
        }
    }

    fn copy_bgra_texture(
        &self,
        frame: &DecodedVideoFrame,
        gpu_data: &GpuTextureData,
    ) -> Result<PresentationFrame, GpuRenderError> {
        self.context.with_multithread_guard(|| {
            Ok(VideoFrame {
                pts_us: frame.pts_us,
                duration_us: frame.duration_us,
                width: frame.width,
                height: frame.height,
                is_key_frame: frame.is_key_frame,
                surface: Arc::new(
                    VideoSurface::new_gpu_texture(
                        PixelFormatCategory::Bgra8,
                        gpu_data.clone(),
                    )
                    .with_color_info(frame.color_info()),
                ),
            })
        })
    }

    fn nv12_to_cpu_bgra(
        &self,
        frame: &DecodedVideoFrame,
        gpu_data: &GpuTextureData,
    ) -> Result<PresentationFrame, GpuRenderError> {
        self.context.with_multithread_guard(|| {
            use windows::Win32::Graphics::Direct3D11::{
                ID3D11Resource, ID3D11Texture2D, D3D11_CPU_ACCESS_READ, D3D11_MAPPED_SUBRESOURCE,
                D3D11_MAP_READ, D3D11_TEXTURE2D_DESC, D3D11_USAGE_STAGING,
            };
            use windows::Win32::Graphics::Dxgi::Common::{DXGI_FORMAT, DXGI_SAMPLE_DESC};
            use windows::core::Interface;

            let width = frame.width;
            let height = frame.height;

            let staging_desc = D3D11_TEXTURE2D_DESC {
                Width: width,
                Height: height,
                MipLevels: 1,
                ArraySize: 1,
                Format: DXGI_FORMAT(103),
                SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 },
                Usage: D3D11_USAGE_STAGING,
                BindFlags: 0,
                CPUAccessFlags: D3D11_CPU_ACCESS_READ.0 as u32,
                MiscFlags: 0,
            };

            let mut staging: Option<ID3D11Texture2D> = None;
            unsafe {
                self.context
                    .device
                    .CreateTexture2D(&staging_desc, None, Some(&mut staging))
                    .map_err(|_| GpuRenderError::BackendUnavailable)?;
            }
            let staging = staging.ok_or(GpuRenderError::BackendUnavailable)?;

            let source_texture: ID3D11Texture2D =
                unsafe { Interface::from_raw(gpu_data.texture_ptr as *mut _) };
            let source = ManuallyDrop::new(source_texture);

            unsafe {
                let src_resource: ID3D11Resource = source
                    .cast()
                    .map_err(|_| GpuRenderError::BackendUnavailable)?;
                let dst_resource: ID3D11Resource = staging
                    .cast()
                    .map_err(|_| GpuRenderError::BackendUnavailable)?;
                self.context.device_context.CopySubresourceRegion(
                    &dst_resource,
                    0,
                    0,
                    0,
                    0,
                    &src_resource,
                    gpu_data.array_slice,
                    None,
                );
            }

            let mut mapped = D3D11_MAPPED_SUBRESOURCE::default();
            unsafe {
                let staging_resource: ID3D11Resource = staging
                    .cast()
                    .map_err(|_| GpuRenderError::BackendUnavailable)?;
                self.context
                    .device_context
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
                self.context.device_context.Unmap(&staging_resource, 0);
            }

            let Some(bgra_data) = bgra_data else {
                return Err(GpuRenderError::BackendUnavailable);
            };

            Ok(VideoFrame {
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
            })
        })
    }
}

impl GpuRenderer for D3d11GpuRenderer {
    fn render_frame(
        &mut self,
        frame: &DecodedVideoFrame,
    ) -> Result<PresentationFrame, GpuRenderError> {
        self.state.render_attempts = self.state.render_attempts.saturating_add(1);

        let result = self.execute_render(frame);
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
