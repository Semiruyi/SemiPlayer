use crate::decode::policy::{DecodePreference, VideoDecodeOpenOptions, VideoDecodeRequirements};
use crate::decode::video_decode::VideoDecodeBackend;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum VideoDecodeOutputKind {
    CpuBgra,
    GpuSurface,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) struct VideoDecodePlan {
    pub(crate) preferred_backend: VideoDecodeBackend,
    pub(crate) output_kind: VideoDecodeOutputKind,
    pub(crate) allow_fallback: bool,
    pub(crate) hardware_requested: bool,
}

pub(crate) fn plan_video_decode(options: VideoDecodeOpenOptions) -> VideoDecodePlan {
    plan_video_decode_requirements(options.requirements, options.hw_device_ctx.is_some())
}

fn plan_video_decode_requirements(
    requirements: VideoDecodeRequirements,
    has_hw_device_context: bool,
) -> VideoDecodePlan {
    match requirements.preference {
        DecodePreference::PreferCompatibility => VideoDecodePlan {
            preferred_backend: VideoDecodeBackend::SoftwareBgra,
            output_kind: VideoDecodeOutputKind::CpuBgra,
            allow_fallback: requirements.allow_fallback,
            hardware_requested: false,
        },
        DecodePreference::PreferPerformance | DecodePreference::PreferZeroCopy => {
            let hardware_requested = has_hw_device_context || requirements.require_gpu_output;
            VideoDecodePlan {
                preferred_backend: if hardware_requested {
                    VideoDecodeBackend::D3d11va
                } else {
                    VideoDecodeBackend::SoftwareBgra
                },
                output_kind: if requirements.require_gpu_output {
                    VideoDecodeOutputKind::GpuSurface
                } else if hardware_requested {
                    VideoDecodeOutputKind::GpuSurface
                } else {
                    VideoDecodeOutputKind::CpuBgra
                },
                allow_fallback: requirements.allow_fallback,
                hardware_requested,
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{plan_video_decode, VideoDecodeOutputKind};
    use crate::decode::policy::{VideoDecodeOpenOptions, VideoDecodeRequirements};
    use crate::decode::video_decode::VideoDecodeBackend;

    #[test]
    fn compatibility_prefers_software() {
        let plan = plan_video_decode(VideoDecodeOpenOptions {
            requirements: VideoDecodeRequirements::compatibility(),
            hw_device_ctx: Some(std::ptr::dangling_mut()),
        });

        assert_eq!(plan.preferred_backend, VideoDecodeBackend::SoftwareBgra);
        assert_eq!(plan.output_kind, VideoDecodeOutputKind::CpuBgra);
        assert!(!plan.hardware_requested);
    }

    #[test]
    fn performance_without_hw_context_stays_software() {
        let plan = plan_video_decode(VideoDecodeOpenOptions {
            requirements: VideoDecodeRequirements::performance(),
            hw_device_ctx: None,
        });

        assert_eq!(plan.preferred_backend, VideoDecodeBackend::SoftwareBgra);
        assert_eq!(plan.output_kind, VideoDecodeOutputKind::CpuBgra);
    }

    #[test]
    fn zero_copy_requests_gpu_output() {
        let plan = plan_video_decode(VideoDecodeOpenOptions {
            requirements: VideoDecodeRequirements::zero_copy(),
            hw_device_ctx: None,
        });

        assert_eq!(plan.preferred_backend, VideoDecodeBackend::D3d11va);
        assert_eq!(plan.output_kind, VideoDecodeOutputKind::GpuSurface);
        assert!(plan.hardware_requested);
    }
}
