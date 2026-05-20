use crate::render::gpu::GpuBackendKind;

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum VideoDecodeBackend {
    #[default]
    Unknown,
    SoftwareBgra,
    D3d11va,
}

impl VideoDecodeBackend {
    pub const fn as_raw(self) -> u32 {
        match self {
            Self::Unknown => 0,
            Self::SoftwareBgra => 1,
            Self::D3d11va => 2,
        }
    }

    pub const fn exported_gpu_backend_kind(self) -> Option<GpuBackendKind> {
        match self {
            Self::D3d11va => Some(GpuBackendKind::D3d11),
            Self::Unknown | Self::SoftwareBgra => None,
        }
    }

    pub const fn is_hardware_accelerated(self) -> bool {
        self.exported_gpu_backend_kind().is_some()
    }
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum VideoDecodeFallbackReason {
    #[default]
    None,
    NoHardwareConfig,
    HwDeviceCreateFailed,
    HwDeviceContextBindFailed,
    HwDecoderOpenFailed,
    HwDecoderTypeMismatch,
}

impl VideoDecodeFallbackReason {
    pub const fn as_raw(self) -> u32 {
        match self {
            Self::None => 0,
            Self::NoHardwareConfig => 1,
            Self::HwDeviceCreateFailed => 2,
            Self::HwDeviceContextBindFailed => 3,
            Self::HwDecoderOpenFailed => 4,
            Self::HwDecoderTypeMismatch => 5,
        }
    }
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct VideoDecodeDiagnosticsSnapshot {
    pub backend: VideoDecodeBackend,
    pub hardware_requested: bool,
    pub hardware_active: bool,
    pub fallback_reason: VideoDecodeFallbackReason,
}
