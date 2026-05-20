use std::mem::ManuallyDrop;
use std::sync::Arc;

use crate::render::core::converter::{ConversionRequest, FrameConverter, FrameConverterSnapshot, convert_cpu_packed_to_bgra};
use crate::render::core::frame::{
    DecodedVideoFrame, PixelFormatCategory, PresentationFrame, VideoFrame,
    VideoSurface, VideoSurfaceKind, VideoSurfaceStorage,
};
use crate::render::gpu::{GpuBackendKind, GpuTextureData};

use super::device::D3d11DeviceContext;
use super::interop::nv12_to_bgra_via_swscale;

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
struct ConverterState {
    conversion_attempts: u64,
    successful_conversions: u64,
    backend_unavailable_errors: u64,
}

#[derive(Debug)]
pub(crate) struct D3d11FrameConverter {
    context: D3d11DeviceContext,
    state: ConverterState,
}

impl D3d11FrameConverter {
    pub(crate) fn new(context: D3d11DeviceContext) -> Self {
        Self {
            context,
            state: ConverterState::default(),
        }
    }

    fn convert_gpu_nv12_to_cpu_bgra(
        &self,
        frame: DecodedVideoFrame,
        gpu_data: &GpuTextureData,
    ) -> Result<PresentationFrame, DecodedVideoFrame> {
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
            SampleDesc: DXGI_SAMPLE_DESC {
                Count: 1,
                Quality: 0,
            },
            Usage: D3D11_USAGE_STAGING,
            BindFlags: 0,
            CPUAccessFlags: D3D11_CPU_ACCESS_READ.0 as u32,
            MiscFlags: 0,
        };

        self.context.with_multithread_guard(|| {
            let mut staging: Option<ID3D11Texture2D> = None;
            unsafe {
                self.context
                    .device
                    .CreateTexture2D(&staging_desc, None, Some(&mut staging))
            }
            .map_err(|_| frame.clone())?;
            let staging = staging.ok_or_else(|| frame.clone())?;

            let source_texture: ID3D11Texture2D =
                unsafe { Interface::from_raw(gpu_data.texture_ptr as *mut _) };
            let source = ManuallyDrop::new(source_texture);

            unsafe {
                let src_resource: ID3D11Resource = source
                    .cast()
                    .map_err(|_| frame.clone())?;
                let dst_resource: ID3D11Resource = staging
                    .cast()
                    .map_err(|_| frame.clone())?;
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
                    .map_err(|_| frame.clone())?;
                self.context
                    .device_context
                    .Map(&staging_resource, 0, D3D11_MAP_READ, 0, Some(&mut mapped))
                    .map_err(|_| frame.clone())?;
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
                    .map_err(|_| frame.clone())?;
                self.context.device_context.Unmap(&staging_resource, 0);
            }

            let Some(bgra_data) = bgra_data else {
                return Err(frame);
            };

            Ok(VideoFrame {
                pts_us: frame.pts_us,
                duration_us: frame.duration_us,
                width,
                height,
                is_key_frame: frame.is_key_frame,
                surface: Arc::new(
                    VideoSurface::new_cpu_packed(PixelFormatCategory::Bgra8, width as usize * 4, bgra_data)
                        .with_color_info(frame.color_info()),
                ),
            })
        })
    }

    fn convert_gpu_bgra_passthrough(
        &self,
        frame: DecodedVideoFrame,
        gpu_data: &GpuTextureData,
    ) -> Result<PresentationFrame, DecodedVideoFrame> {
        self.context.with_multithread_guard(|| {
            Ok(VideoFrame {
                pts_us: frame.pts_us,
                duration_us: frame.duration_us,
                width: frame.width,
                height: frame.height,
                is_key_frame: frame.is_key_frame,
                surface: Arc::new(
                    VideoSurface::new_gpu_texture(PixelFormatCategory::Bgra8, gpu_data.clone())
                        .with_color_info(frame.color_info()),
                ),
            })
        })
    }
}

impl FrameConverter for D3d11FrameConverter {
    fn convert(
        &mut self,
        frame: DecodedVideoFrame,
        request: ConversionRequest,
    ) -> Result<PresentationFrame, DecodedVideoFrame> {
        self.state.conversion_attempts = self.state.conversion_attempts.saturating_add(1);

        let result = match request {
            ConversionRequest::Passthrough => Ok(frame),
            ConversionRequest::Convert {
                target_pixel_format,
                target_surface_kind,
            } => match (target_surface_kind, target_pixel_format) {
                (VideoSurfaceKind::CpuPacked, PixelFormatCategory::Bgra8) => {
                    match &frame.surface.storage {
                        VideoSurfaceStorage::GpuTexture(gpu_data) => {
                            if gpu_data.backend() != GpuBackendKind::D3d11 {
                                Err(frame)
                            } else {
                                let gpu_data = gpu_data.clone();
                                match frame.pixel_format() {
                                    PixelFormatCategory::Nv12 => {
                                        self.convert_gpu_nv12_to_cpu_bgra(frame, &gpu_data)
                                    }
                                    PixelFormatCategory::Bgra8 => {
                                        self.convert_gpu_bgra_passthrough(frame, &gpu_data)
                                    }
                                    _ => Err(frame),
                                }
                            }
                        }
                        VideoSurfaceStorage::CpuPacked { .. } => convert_cpu_packed_to_bgra(frame),
                    }
                }
                (VideoSurfaceKind::GpuTexture, PixelFormatCategory::Bgra8) => {
                    match &frame.surface.storage {
                        VideoSurfaceStorage::GpuTexture(gpu_data) => {
                            if gpu_data.backend() != GpuBackendKind::D3d11 {
                                Err(frame)
                            } else {
                                let gpu_data = gpu_data.clone();
                                match frame.pixel_format() {
                                    PixelFormatCategory::Bgra8 => {
                                        self.convert_gpu_bgra_passthrough(frame, &gpu_data)
                                    }
                                    _ => Err(frame),
                                }
                            }
                        }
                        _ => Err(frame),
                    }
                }
                _ => Err(frame),
            },
        };

        match result {
            Ok(presentation_frame) => {
                self.state.successful_conversions =
                    self.state.successful_conversions.saturating_add(1);
                Ok(presentation_frame)
            }
            Err(original_frame) => {
                self.state.backend_unavailable_errors =
                    self.state.backend_unavailable_errors.saturating_add(1);
                Err(original_frame)
            }
        }
    }

    fn snapshot(&self) -> FrameConverterSnapshot {
        FrameConverterSnapshot {
            conversion_attempts: self.state.conversion_attempts,
            successful_conversions: self.state.successful_conversions,
            backend_unavailable_errors: self.state.backend_unavailable_errors,
        }
    }
}
