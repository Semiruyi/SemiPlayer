mod compute;
mod converter;
mod device;
mod interop;

use std::sync::Arc;

use crate::render::gpu::{GpuDeviceError, RenderBackend};

pub(crate) fn create_backend() -> Result<Arc<dyn RenderBackend>, GpuDeviceError> {
    let device = device::D3d11GpuDevice::new()?;
    Ok(Arc::new(device))
}
