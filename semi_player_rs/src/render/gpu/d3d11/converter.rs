use std::mem::ManuallyDrop;
use std::sync::Arc;

use crate::render::core::converter::{
    ConversionRequest, FrameConverter, FrameConverterSnapshot, convert_cpu_packed_to_bgra,
};
use crate::render::core::convert::pixel_format::YuvToRgbMatrix;
use crate::render::core::frame::{
    DecodedVideoFrame, PixelFormatCategory, PresentationFrame, VideoFrame, VideoSurface,
    VideoSurfaceKind, VideoSurfaceStorage,
};
use crate::render::gpu::{GpuBackendKind, GpuTextureData, GpuTextureLease};

use super::compute::Nv12ToBgraCompute;
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
    compute: Nv12ToBgraCompute,
    state: ConverterState,
}

impl D3d11FrameConverter {
    pub(crate) fn new(context: D3d11DeviceContext) -> Option<Self> {
        let compute = Nv12ToBgraCompute::new(&context.device).ok()?;
        Some(Self {
            context,
            compute,
            state: ConverterState::default(),
        })
    }

    fn convert_gpu_nv12_to_gpu_bgra(
        &mut self,
        frame: DecodedVideoFrame,
        gpu_data: &GpuTextureData,
    ) -> Result<PresentationFrame, DecodedVideoFrame> {
        use windows::Win32::Graphics::Direct3D11::{
            ID3D11Resource, ID3D11Texture2D, D3D11_BIND_SHADER_RESOURCE, D3D11_TEXTURE2D_DESC,
            D3D11_USAGE_DEFAULT,
        };
        use windows::Win32::Graphics::Dxgi::Common::{DXGI_FORMAT, DXGI_SAMPLE_DESC};
        use windows::core::Interface;

        let width = frame.width;
        let height = frame.height;
        let matrix = YuvToRgbMatrix::from_color_info(frame.color_info(), width, height);

        // Step 1: GPU work inside multithread guard
        let bgra_texture = self.context.with_multithread_guard(|| {
            let nv12_desc = D3D11_TEXTURE2D_DESC {
                Width: width,
                Height: height,
                MipLevels: 1,
                ArraySize: 1,
                Format: DXGI_FORMAT(103), // DXGI_FORMAT_NV12
                SampleDesc: DXGI_SAMPLE_DESC {
                    Count: 1,
                    Quality: 0,
                },
                Usage: D3D11_USAGE_DEFAULT,
                BindFlags: D3D11_BIND_SHADER_RESOURCE.0 as u32,
                CPUAccessFlags: 0,
                MiscFlags: 0,
            };

            let mut nv12_readable: Option<ID3D11Texture2D> = None;
            unsafe {
                self.context
                    .device
                    .CreateTexture2D(&nv12_desc, None, Some(&mut nv12_readable))
            }
            .map_err(|_| frame.clone())?;
            let nv12_readable = nv12_readable.ok_or_else(|| frame.clone())?;

            let source_texture: ID3D11Texture2D =
                unsafe { Interface::from_raw(gpu_data.texture_ptr as *mut _) };
            let source = ManuallyDrop::new(source_texture);

            unsafe {
                let src_resource: ID3D11Resource = source.cast().map_err(|_| frame.clone())?;
                let dst_resource: ID3D11Resource =
                    nv12_readable.cast().map_err(|_| frame.clone())?;
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

            self.compute
                .convert(&self.context, &nv12_readable, width, height, &matrix)
                .map_err(|_| frame.clone())
        })?;

        // Step 2: Debug dump (outside guard, borrows self.context independently)
        // self.dump_bgra_texture(&bgra_texture, width, height, "debug_bgra_output.bmp");

        // Step 3: Wrap as GpuTextureData
        let texture_ptr = unsafe { bgra_texture.as_raw() as usize as u64 };
        std::mem::forget(bgra_texture);

        Ok(VideoFrame {
            pts_us: frame.pts_us,
            duration_us: frame.duration_us,
            width,
            height,
            is_key_frame: frame.is_key_frame,
            surface: Arc::new(
                VideoSurface::new_gpu_texture(
                    PixelFormatCategory::Bgra8,
                    GpuTextureData::new(
                        GpuBackendKind::D3d11,
                        texture_ptr,
                        None,
                        0,
                        Some(GpuTextureLease::new(GpuBackendKind::D3d11, texture_ptr)),
                    ),
                )
                .with_color_info(frame.color_info()),
            ),
        })
    }

    fn convert_gpu_nv12_to_cpu_bgra(
        &mut self,
        frame: DecodedVideoFrame,
        gpu_data: &GpuTextureData,
    ) -> Result<PresentationFrame, DecodedVideoFrame> {
        use windows::Win32::Graphics::Direct3D11::{
            ID3D11Resource, ID3D11Texture2D, D3D11_CPU_ACCESS_READ, D3D11_MAPPED_SUBRESOURCE,
            D3D11_MAP_READ, D3D11_TEXTURE2D_DESC, D3D11_USAGE_STAGING,
        };
        use windows::Win32::Graphics::Dxgi::Common::{DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_SAMPLE_DESC};
        use windows::core::Interface;

        let width = frame.width;
        let height = frame.height;

        // Step 1: GPU NV12 → GPU BGRA via compute shader
        let gpu_result = self.convert_gpu_nv12_to_gpu_bgra(frame.clone(), gpu_data);
        let gpu_frame = gpu_result.map_err(|f| f)?;

        // Step 2: Copy BGRA GPU texture → staging → CPU
        let gpu_texture_data = match &gpu_frame.surface.storage {
            VideoSurfaceStorage::GpuTexture(data) => data.clone(),
            _ => return Err(frame),
        };

        let width = gpu_frame.width;
        let height = gpu_frame.height;
        let stride = width as usize * 4;

        self.context.with_multithread_guard(|| {
            let staging_desc = D3D11_TEXTURE2D_DESC {
                Width: width,
                Height: height,
                MipLevels: 1,
                ArraySize: 1,
                Format: DXGI_FORMAT_R8G8B8A8_UNORM,
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
                self.context
                    .device
                    .CreateTexture2D(&staging_desc, None, Some(&mut staging))
            }
            .map_err(|_| frame.clone())?;
            let staging = staging.ok_or_else(|| frame.clone())?;

            let bgra_texture: ID3D11Texture2D =
                unsafe { Interface::from_raw(gpu_texture_data.texture_ptr as *mut _) };
            let bgra_source = ManuallyDrop::new(bgra_texture);

            unsafe {
                let src_resource: ID3D11Resource = bgra_source.cast().map_err(|_| frame.clone())?;
                let dst_resource: ID3D11Resource = staging.cast().map_err(|_| frame.clone())?;
                self.context.device_context.CopySubresourceRegion(
                    &dst_resource,
                    0,
                    0,
                    0,
                    0,
                    &src_resource,
                    0,
                    None,
                );
            }

            let mut mapped = D3D11_MAPPED_SUBRESOURCE::default();
            unsafe {
                let staging_resource: ID3D11Resource = staging.cast().map_err(|_| frame.clone())?;
                self.context
                    .device_context
                    .Map(&staging_resource, 0, D3D11_MAP_READ, 0, Some(&mut mapped))
                    .map_err(|_| frame.clone())?;
            }

            let mut bgra_data = vec![0u8; stride * height as usize];
            let src_pitch = mapped.RowPitch as usize;
            let src_ptr = mapped.pData as *const u8;
            for y in 0..height as usize {
                unsafe {
                    std::ptr::copy_nonoverlapping(
                        src_ptr.add(y * src_pitch),
                        bgra_data.as_mut_ptr().add(y * stride),
                        stride,
                    );
                }
            }

            unsafe {
                let staging_resource: ID3D11Resource = staging.cast().map_err(|_| frame.clone())?;
                self.context.device_context.Unmap(&staging_resource, 0);
            }

            Ok(VideoFrame {
                pts_us: frame.pts_us,
                duration_us: frame.duration_us,
                width,
                height,
                is_key_frame: frame.is_key_frame,
                surface: Arc::new(
                    VideoSurface::new_cpu_packed(PixelFormatCategory::Bgra8, stride, bgra_data)
                        .with_color_info(frame.color_info()),
                ),
            })
        })
    }

    #[cfg(debug_assertions)]
    fn dump_bgra_texture(
        &self,
        texture: &windows::Win32::Graphics::Direct3D11::ID3D11Texture2D,
        width: u32,
        height: u32,
        path: &str,
    ) {
        use windows::Win32::Graphics::Direct3D11::{
            ID3D11Resource, ID3D11Texture2D, D3D11_CPU_ACCESS_READ, D3D11_MAPPED_SUBRESOURCE,
            D3D11_MAP_READ, D3D11_TEXTURE2D_DESC, D3D11_USAGE_STAGING,
        };
        use windows::Win32::Graphics::Dxgi::Common::{DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_SAMPLE_DESC};
        use windows::core::Interface;

        let _ = self.context.with_multithread_guard(|| -> Result<(), ()> {
            let staging_desc = D3D11_TEXTURE2D_DESC {
                Width: width,
                Height: height,
                MipLevels: 1,
                ArraySize: 1,
                Format: DXGI_FORMAT_R8G8B8A8_UNORM,
                SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 },
                Usage: D3D11_USAGE_STAGING,
                BindFlags: 0,
                CPUAccessFlags: D3D11_CPU_ACCESS_READ.0 as u32,
                MiscFlags: 0,
            };

            let mut staging: Option<ID3D11Texture2D> = None;
            unsafe {
                self.context.device.CreateTexture2D(&staging_desc, None, Some(&mut staging))
            }.map_err(|_| ())?;
            let staging = staging.ok_or(())?;

            unsafe {
                let src: ID3D11Resource = texture.cast().map_err(|_| ())?;
                let dst: ID3D11Resource = staging.cast().map_err(|_| ())?;
                self.context.device_context.CopySubresourceRegion(&dst, 0, 0, 0, 0, &src, 0, None);
            }

            let mut mapped = D3D11_MAPPED_SUBRESOURCE::default();
            unsafe {
                let res: ID3D11Resource = staging.cast().map_err(|_| ())?;
                self.context.device_context.Map(&res, 0, D3D11_MAP_READ, 0, Some(&mut mapped))
                    .map_err(|_| ())?;
            }

            let row_pitch = mapped.RowPitch as usize;
            let stride = width as usize * 4;
            let mut pixels = vec![0u8; stride * height as usize];
            let src_ptr = mapped.pData as *const u8;
            for y in 0..height as usize {
                unsafe {
                    std::ptr::copy_nonoverlapping(
                        src_ptr.add(y * row_pitch),
                        pixels.as_mut_ptr().add(y * stride),
                        stride,
                    );
                }
            }

            unsafe {
                let res: ID3D11Resource = staging.cast().map_err(|_| ())?;
                self.context.device_context.Unmap(&res, 0);
            }

            // Write BMP
            let file_size = 54 + pixels.len();
            let mut bmp = Vec::with_capacity(file_size);
            // BMP header
            bmp.extend_from_slice(b"BM");
            bmp.extend_from_slice(&(file_size as u32).to_le_bytes());
            bmp.extend_from_slice(&[0; 4]); // reserved
            bmp.extend_from_slice(&54u32.to_le_bytes()); // pixel offset
            // DIB header
            bmp.extend_from_slice(&40u32.to_le_bytes()); // header size
            bmp.extend_from_slice(&(width as i32).to_le_bytes());
            bmp.extend_from_slice(&(height as i32).to_le_bytes()); // positive = bottom-up
            bmp.extend_from_slice(&1u16.to_le_bytes()); // planes
            bmp.extend_from_slice(&32u16.to_le_bytes()); // bits per pixel
            bmp.extend_from_slice(&[0; 24]); // no compression, default
            // Pixel data — BMP is bottom-up, our data is top-down, flip rows
            for y in (0..height as usize).rev() {
                bmp.extend_from_slice(&pixels[y * stride..(y + 1) * stride]);
            }

            let _ = std::fs::write(path, bmp);
            eprintln!("[debug] dumped {}x{} BGRA to {}", width, height, path);

            Ok(())
        });
    }

    #[cfg(not(debug_assertions))]
    fn dump_bgra_texture(&self, _texture: &windows::Win32::Graphics::Direct3D11::ID3D11Texture2D, _width: u32, _height: u32, _path: &str) {}

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
                                    PixelFormatCategory::Nv12 => {
                                        self.convert_gpu_nv12_to_gpu_bgra(frame, &gpu_data)
                                    }
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
