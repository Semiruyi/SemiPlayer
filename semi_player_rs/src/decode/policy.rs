use ffmpeg_next::ffi;

#[allow(dead_code)]
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum DecodePreference {
    PreferCompatibility,
    #[default]
    PreferPerformance,
    PreferZeroCopy,
}

#[allow(dead_code)]
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct VideoDecodeRequirements {
    pub preference: DecodePreference,
    pub allow_fallback: bool,
    pub require_gpu_output: bool,
}

#[allow(dead_code)]
impl VideoDecodeRequirements {
    pub const fn compatibility() -> Self {
        Self {
            preference: DecodePreference::PreferCompatibility,
            allow_fallback: true,
            require_gpu_output: false,
        }
    }

    pub const fn performance() -> Self {
        Self {
            preference: DecodePreference::PreferPerformance,
            allow_fallback: true,
            require_gpu_output: false,
        }
    }

    pub const fn zero_copy() -> Self {
        Self {
            preference: DecodePreference::PreferZeroCopy,
            allow_fallback: true,
            require_gpu_output: true,
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) struct VideoDecodeOpenOptions {
    pub(crate) requirements: VideoDecodeRequirements,
    pub(crate) hw_device_ctx: Option<*mut ffi::AVBufferRef>,
}

impl Default for VideoDecodeOpenOptions {
    fn default() -> Self {
        Self {
            requirements: VideoDecodeRequirements::performance(),
            hw_device_ctx: None,
        }
    }
}
