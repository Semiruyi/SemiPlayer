#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum VideoDecodeBackend {
    #[default]
    Unknown,
    SoftwareBgra,
    D3d11va,
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

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct VideoDecodeDiagnosticsSnapshot {
    pub backend: VideoDecodeBackend,
    pub hardware_requested: bool,
    pub hardware_active: bool,
    pub fallback_reason: VideoDecodeFallbackReason,
}
