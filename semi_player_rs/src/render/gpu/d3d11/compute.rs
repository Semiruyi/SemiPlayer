use std::mem;

use windows::Win32::Graphics::Direct3D::D3D_SRV_DIMENSION;
use windows::Win32::Graphics::Direct3D11::{
    ID3D11Buffer, ID3D11ComputeShader, ID3D11Texture2D, ID3D11UnorderedAccessView,
    D3D11_BIND_CONSTANT_BUFFER, D3D11_BIND_SHADER_RESOURCE, D3D11_BIND_UNORDERED_ACCESS,
    D3D11_BUFFER_DESC, D3D11_TEX2D_SRV, D3D11_TEX2D_UAV, D3D11_TEXTURE2D_DESC,
    D3D11_USAGE_DEFAULT, ID3D11Device, ID3D11ShaderResourceView,
    D3D11_SHADER_RESOURCE_VIEW_DESC, D3D11_UAV_DIMENSION_TEXTURE2D,
};
use windows::Win32::Graphics::Dxgi::Common::{
    DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8_UNORM,
    DXGI_SAMPLE_DESC,
};

use crate::render::core::convert::pixel_format::YuvToRgbMatrix;

use super::device::D3d11DeviceContext;

const CS_BYTECODE: &[u8] =
    include_bytes!(concat!(env!("OUT_DIR"), "/nv12_to_bgra_cs.cso"));

#[derive(Debug)]
pub(crate) enum ComputeError {
    ShaderCreationFailed,
    ResourceCreationFailed,
}

struct CachedOutput {
    width: u32,
    height: u32,
    texture: ID3D11Texture2D,
    uav: ID3D11UnorderedAccessView,
}

pub(crate) struct Nv12ToBgraCompute {
    compute_shader: ID3D11ComputeShader,
    constant_buffer: ID3D11Buffer,
    cached_output: Option<CachedOutput>,
}

impl std::fmt::Debug for Nv12ToBgraCompute {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Nv12ToBgraCompute")
            .field("has_cached_output", &self.cached_output.is_some())
            .finish()
    }
}

impl Nv12ToBgraCompute {
    pub(crate) fn new(device: &ID3D11Device) -> Result<Self, ComputeError> {
        let mut compute_shader: Option<ID3D11ComputeShader> = None;
        unsafe {
            device
                .CreateComputeShader(CS_BYTECODE, None, Some(&mut compute_shader))
                .map_err(|_| ComputeError::ShaderCreationFailed)?
        };
        let compute_shader = compute_shader.ok_or(ComputeError::ShaderCreationFailed)?;

        let cb_byte_width = ((mem::size_of::<YuvToRgbMatrix>() + 15) & !15) as u32;
        let cb_desc = D3D11_BUFFER_DESC {
            ByteWidth: cb_byte_width,
            Usage: D3D11_USAGE_DEFAULT,
            BindFlags: D3D11_BIND_CONSTANT_BUFFER.0 as u32,
            CPUAccessFlags: 0,
            MiscFlags: 0,
            StructureByteStride: 0,
        };

        let mut constant_buffer: Option<ID3D11Buffer> = None;
        unsafe {
            device
                .CreateBuffer(&cb_desc, None, Some(&mut constant_buffer))
                .map_err(|_| ComputeError::ResourceCreationFailed)?
        };
        let constant_buffer = constant_buffer.ok_or(ComputeError::ResourceCreationFailed)?;

        Ok(Self {
            compute_shader,
            constant_buffer,
            cached_output: None,
        })
    }

    pub(crate) fn convert(
        &mut self,
        ctx: &D3d11DeviceContext,
        source_nv12: &ID3D11Texture2D,
        width: u32,
        height: u32,
        matrix: &YuvToRgbMatrix,
    ) -> Result<ID3D11Texture2D, ComputeError> {
        // Clone output resources before binding to avoid borrow conflicts
        let (output_texture, output_uav) = {
            let output = self.get_or_create_output(&ctx.device, width, height)?;
            (output.texture.clone(), output.uav.clone())
        };

        unsafe {
            let y_srv = self.create_y_srv(&ctx.device, source_nv12)?;
            let uv_srv = self.create_uv_srv(&ctx.device, source_nv12)?;

            // Update constant buffer
            ctx.device_context.UpdateSubresource(
                &self.constant_buffer,
                0,
                None,
                matrix as *const YuvToRgbMatrix as *const _,
                mem::size_of::<YuvToRgbMatrix>() as u32,
                0,
            );

            // Bind resources
            let srvs: [Option<ID3D11ShaderResourceView>; 2] = [Some(y_srv), Some(uv_srv)];
            let cbs: [Option<ID3D11Buffer>; 1] = [Some(self.constant_buffer.clone())];

            ctx.device_context.CSSetShader(&self.compute_shader, None);
            ctx.device_context.CSSetShaderResources(0, Some(&srvs));
            ctx.device_context.CSSetConstantBuffers(0, Some(&cbs));

            let uavs = [Some(output_uav)];
            let uav_counts = [0u32];
            ctx.device_context.CSSetUnorderedAccessViews(
                0,
                1,
                Some(uavs.as_ptr()),
                Some(uav_counts.as_ptr()),
            );

            // Dispatch
            let thread_groups_x = (width + 15) / 16;
            let thread_groups_y = (height + 15) / 16;
            ctx.device_context.Dispatch(thread_groups_x, thread_groups_y, 1);

            // Unbind
            let null_srvs: [Option<ID3D11ShaderResourceView>; 2] = [None, None];
            ctx.device_context.CSSetShaderResources(0, Some(&null_srvs));

            let null_uavs = [None::<ID3D11UnorderedAccessView>; 1];
            let null_counts = [0u32; 1];
            ctx.device_context.CSSetUnorderedAccessViews(
                0,
                1,
                Some(null_uavs.as_ptr()),
                Some(null_counts.as_ptr()),
            );

            Ok(output_texture)
        }
    }

    unsafe fn create_y_srv(
        &self,
        device: &ID3D11Device,
        texture: &ID3D11Texture2D,
    ) -> Result<ID3D11ShaderResourceView, ComputeError> {
        let desc = D3D11_SHADER_RESOURCE_VIEW_DESC {
            Format: DXGI_FORMAT_R8_UNORM,
            ViewDimension: D3D_SRV_DIMENSION(4), // D3D11_SRV_DIMENSION_TEXTURE2D
            Anonymous: windows::Win32::Graphics::Direct3D11::D3D11_SHADER_RESOURCE_VIEW_DESC_0 {
                Texture2D: D3D11_TEX2D_SRV {
                    MostDetailedMip: 0,
                    MipLevels: 1,
                },
            },
        };
        let mut srv: Option<ID3D11ShaderResourceView> = None;
        device
            .CreateShaderResourceView(texture, Some(&desc), Some(&mut srv))
            .map_err(|_| ComputeError::ResourceCreationFailed)?;
        srv.ok_or(ComputeError::ResourceCreationFailed)
    }

    unsafe fn create_uv_srv(
        &self,
        device: &ID3D11Device,
        texture: &ID3D11Texture2D,
    ) -> Result<ID3D11ShaderResourceView, ComputeError> {
        let desc = D3D11_SHADER_RESOURCE_VIEW_DESC {
            Format: DXGI_FORMAT_R8G8_UNORM,
            ViewDimension: D3D_SRV_DIMENSION(4), // D3D11_SRV_DIMENSION_TEXTURE2D
            Anonymous: windows::Win32::Graphics::Direct3D11::D3D11_SHADER_RESOURCE_VIEW_DESC_0 {
                Texture2D: D3D11_TEX2D_SRV {
                    MostDetailedMip: 0,
                    MipLevels: 1,
                },
            },
        };
        let mut srv: Option<ID3D11ShaderResourceView> = None;
        device
            .CreateShaderResourceView(texture, Some(&desc), Some(&mut srv))
            .map_err(|_| ComputeError::ResourceCreationFailed)?;
        srv.ok_or(ComputeError::ResourceCreationFailed)
    }

    fn get_or_create_output(
        &mut self,
        device: &ID3D11Device,
        width: u32,
        height: u32,
    ) -> Result<&CachedOutput, ComputeError> {
        if let Some(ref cache) = self.cached_output {
            if cache.width == width && cache.height == height {
                return Ok(self.cached_output.as_ref().unwrap());
            }
        }

        let texture = unsafe { self.create_output_texture(device, width, height)? };
        let uav = unsafe { self.create_output_uav(device, &texture)? };

        self.cached_output = Some(CachedOutput {
            width,
            height,
            texture,
            uav,
        });
        Ok(self.cached_output.as_ref().unwrap())
    }

    unsafe fn create_output_texture(
        &self,
        device: &ID3D11Device,
        width: u32,
        height: u32,
    ) -> Result<ID3D11Texture2D, ComputeError> {
        let desc = D3D11_TEXTURE2D_DESC {
            Width: width,
            Height: height,
            MipLevels: 1,
            ArraySize: 1,
            Format: DXGI_FORMAT_R8G8B8A8_UNORM,
            SampleDesc: DXGI_SAMPLE_DESC {
                Count: 1,
                Quality: 0,
            },
            Usage: D3D11_USAGE_DEFAULT,
            BindFlags: (D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE).0 as u32,
            CPUAccessFlags: 0,
            MiscFlags: 0,
        };
        let mut texture: Option<ID3D11Texture2D> = None;
        device
            .CreateTexture2D(&desc, None, Some(&mut texture))
            .map_err(|_| ComputeError::ResourceCreationFailed)?;
        texture.ok_or(ComputeError::ResourceCreationFailed)
    }

    unsafe fn create_output_uav(
        &self,
        device: &ID3D11Device,
        texture: &ID3D11Texture2D,
    ) -> Result<ID3D11UnorderedAccessView, ComputeError> {
        let desc = windows::Win32::Graphics::Direct3D11::D3D11_UNORDERED_ACCESS_VIEW_DESC {
            Format: DXGI_FORMAT_R8G8B8A8_UNORM,
            ViewDimension: D3D11_UAV_DIMENSION_TEXTURE2D,
            Anonymous: windows::Win32::Graphics::Direct3D11::D3D11_UNORDERED_ACCESS_VIEW_DESC_0 {
                Texture2D: D3D11_TEX2D_UAV { MipSlice: 0 },
            },
        };
        let mut uav: Option<ID3D11UnorderedAccessView> = None;
        device
            .CreateUnorderedAccessView(texture, Some(&desc), Some(&mut uav))
            .map_err(|_| ComputeError::ResourceCreationFailed)?;
        uav.ok_or(ComputeError::ResourceCreationFailed)
    }
}
