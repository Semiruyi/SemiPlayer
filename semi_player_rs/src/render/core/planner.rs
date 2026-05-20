use crate::render::core::converter::ConversionRequest;
use crate::render::core::frame::{
    DecodedVideoFrame, PixelFormatCategory, VideoSurfaceKind,
};
use crate::render::gpu::RenderBackendCapabilities;

#[allow(dead_code)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum PresentationPixelFormatPreference {
    PreserveInput,
    Bgra8,
}

impl Default for PresentationPixelFormatPreference {
    fn default() -> Self {
        Self::PreserveInput
    }
}

#[allow(dead_code)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum PresentationIntent {
    Passthrough,
    CpuBgraCompatibility,
    GpuBgraPresenter,
}

impl Default for PresentationIntent {
    fn default() -> Self {
        Self::Passthrough
    }
}

#[allow(dead_code)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum PresentationSurfaceKindPreference {
    PreserveInput,
    CpuPacked,
    GpuTexture,
}

impl Default for PresentationSurfaceKindPreference {
    fn default() -> Self {
        Self::PreserveInput
    }
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct VideoRenderRequest {
    pub target_intent: PresentationIntent,
    pub presentation_pixel_format: PresentationPixelFormatPreference,
    pub presentation_surface_kind: PresentationSurfaceKindPreference,
    pub subtitles_visible: bool,
}

impl VideoRenderRequest {
    pub fn from_intent(
        target_intent: PresentationIntent,
        subtitles_visible: bool,
    ) -> Self {
        match target_intent {
            PresentationIntent::Passthrough => Self::passthrough(subtitles_visible),
            PresentationIntent::CpuBgraCompatibility => {
                Self::cpu_bgra_compatibility(subtitles_visible)
            }
            PresentationIntent::GpuBgraPresenter => {
                Self::gpu_bgra_presenter(subtitles_visible)
            }
        }
    }

    #[allow(dead_code)]
    pub fn passthrough(subtitles_visible: bool) -> Self {
        Self {
            target_intent: PresentationIntent::Passthrough,
            presentation_pixel_format: PresentationPixelFormatPreference::PreserveInput,
            presentation_surface_kind: PresentationSurfaceKindPreference::PreserveInput,
            subtitles_visible,
        }
    }

    #[allow(dead_code)]
    pub fn cpu_bgra_compatibility(subtitles_visible: bool) -> Self {
        Self {
            target_intent: PresentationIntent::CpuBgraCompatibility,
            presentation_pixel_format: PresentationPixelFormatPreference::Bgra8,
            presentation_surface_kind: PresentationSurfaceKindPreference::CpuPacked,
            subtitles_visible,
        }
    }

    #[allow(dead_code)]
    pub fn gpu_bgra_presenter(subtitles_visible: bool) -> Self {
        Self {
            target_intent: PresentationIntent::GpuBgraPresenter,
            presentation_pixel_format: PresentationPixelFormatPreference::Bgra8,
            presentation_surface_kind: PresentationSurfaceKindPreference::GpuTexture,
            subtitles_visible,
        }
    }
}

#[derive(Clone, Copy, Debug, Default)]
pub struct RenderPlanner;

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct VideoRenderStats {
    pub rendered_frames: usize,
    pub passthrough_frames: usize,
    pub passthrough_with_subtitle_intent_frames: usize,
    pub requires_transform_frames: usize,
    pub fallback_passthrough_frames: usize,
}

#[derive(Debug, Default)]
pub struct VideoRenderBatch {
    pub frames: Vec<crate::render::core::frame::PresentationFrame>,
    pub stats: VideoRenderStats,
}

impl RenderPlanner {
    #[allow(dead_code)]
    pub fn new() -> Self {
        Self
    }

    pub(crate) fn plan_render(
        &self,
        request: VideoRenderRequest,
        frame: &DecodedVideoFrame,
        backend_capabilities: RenderBackendCapabilities,
    ) -> VideoRenderPlan {
        let target = self.resolve_request(request, frame);
        let input = ResolvedVideoRenderRequest {
            presentation_pixel_format: frame.pixel_format(),
            presentation_surface_kind: frame.surface_kind(),
        };
        let path = if target == input {
            if request.subtitles_visible {
                VideoRenderPath::PassthroughWithSubtitleIntent
            } else {
                VideoRenderPath::Passthrough
            }
        } else if self.can_support_transform(&input, &target, backend_capabilities) {
            VideoRenderPath::RequiresTransform
        } else {
            VideoRenderPath::UnsupportedTransform
        };

        let request = if path == VideoRenderPath::Passthrough
            || path == VideoRenderPath::PassthroughWithSubtitleIntent
        {
            ConversionRequest::Passthrough
        } else {
            ConversionRequest::Convert {
                target_pixel_format: target.presentation_pixel_format,
                target_surface_kind: target.presentation_surface_kind,
            }
        };

        VideoRenderPlan { request, path }
    }

    fn resolve_request(
        &self,
        request: VideoRenderRequest,
        frame: &DecodedVideoFrame,
    ) -> ResolvedVideoRenderRequest {
        let profile_pixel_format = match request.target_intent {
            PresentationIntent::Passthrough => {
                PresentationPixelFormatPreference::PreserveInput
            }
            PresentationIntent::CpuBgraCompatibility
            | PresentationIntent::GpuBgraPresenter => {
                PresentationPixelFormatPreference::Bgra8
            }
        };
        let profile_surface_kind = match request.target_intent {
            PresentationIntent::Passthrough => {
                PresentationSurfaceKindPreference::PreserveInput
            }
            PresentationIntent::CpuBgraCompatibility => {
                PresentationSurfaceKindPreference::CpuPacked
            }
            PresentationIntent::GpuBgraPresenter => {
                PresentationSurfaceKindPreference::GpuTexture
            }
        };
        let pixel_format_preference =
            merge_pixel_format_preference(profile_pixel_format, request.presentation_pixel_format);
        let surface_kind_preference =
            merge_surface_kind_preference(profile_surface_kind, request.presentation_surface_kind);

        ResolvedVideoRenderRequest {
            presentation_pixel_format: match pixel_format_preference {
                PresentationPixelFormatPreference::PreserveInput => frame.pixel_format(),
                PresentationPixelFormatPreference::Bgra8 => PixelFormatCategory::Bgra8,
            },
            presentation_surface_kind: match surface_kind_preference {
                PresentationSurfaceKindPreference::PreserveInput => frame.surface_kind(),
                PresentationSurfaceKindPreference::CpuPacked => VideoSurfaceKind::CpuPacked,
                PresentationSurfaceKindPreference::GpuTexture => VideoSurfaceKind::GpuTexture,
            },
        }
    }

    fn can_support_transform(
        &self,
        input: &ResolvedVideoRenderRequest,
        target: &ResolvedVideoRenderRequest,
        backend_capabilities: RenderBackendCapabilities,
    ) -> bool {
        match (
            input.presentation_surface_kind,
            input.presentation_pixel_format,
            target.presentation_surface_kind,
            target.presentation_pixel_format,
        ) {
            (_, _, VideoSurfaceKind::CpuPacked, PixelFormatCategory::Bgra8)
                if input.presentation_surface_kind != VideoSurfaceKind::GpuTexture =>
            {
                true
            }
            (VideoSurfaceKind::GpuTexture, _, VideoSurfaceKind::CpuPacked, PixelFormatCategory::Bgra8) => {
                backend_capabilities.supports_nv12_cpu_bgra_conversion
                    || backend_capabilities.supports_gpu_bgra_presentation
            }
            (_, _, VideoSurfaceKind::GpuTexture, PixelFormatCategory::Bgra8) => {
                backend_capabilities.supports_gpu_bgra_presentation
            }
            _ => false,
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum VideoRenderPath {
    Passthrough,
    PassthroughWithSubtitleIntent,
    RequiresTransform,
    UnsupportedTransform,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) struct VideoRenderPlan {
    pub request: ConversionRequest,
    pub path: VideoRenderPath,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct ResolvedVideoRenderRequest {
    presentation_pixel_format: PixelFormatCategory,
    presentation_surface_kind: VideoSurfaceKind,
}

fn merge_pixel_format_preference(
    profile: PresentationPixelFormatPreference,
    explicit: PresentationPixelFormatPreference,
) -> PresentationPixelFormatPreference {
    match explicit {
        PresentationPixelFormatPreference::PreserveInput => profile,
        _ => explicit,
    }
}

fn merge_surface_kind_preference(
    profile: PresentationSurfaceKindPreference,
    explicit: PresentationSurfaceKindPreference,
) -> PresentationSurfaceKindPreference {
    match explicit {
        PresentationSurfaceKindPreference::PreserveInput => profile,
        _ => explicit,
    }
}

#[cfg(test)]
mod tests {
    use std::sync::Arc;

    use super::{
        ConversionRequest, PresentationPixelFormatPreference, PresentationSurfaceKindPreference,
        PresentationIntent, VideoRenderPath, RenderPlanner, VideoRenderRequest,
    };
    use crate::render::core::frame::{
        PixelFormatCategory, VideoFrame, VideoSurface, VideoSurfaceKind,
    };
    use crate::render::gpu::RenderBackendCapabilities;

    fn no_backend_capabilities() -> RenderBackendCapabilities {
        RenderBackendCapabilities::default()
    }

    fn decoded_frame(pts_us: i64) -> VideoFrame {
        VideoFrame {
            pts_us,
            duration_us: Some(33_000),
            width: 1920,
            height: 1080,
            is_key_frame: false,
            surface: Arc::new(VideoSurface::new_cpu_packed(
                PixelFormatCategory::Bgra8,
                1920 * 4,
                vec![0; 16],
            )),
        }
    }

    fn rgba_frame(pts_us: i64) -> VideoFrame {
        VideoFrame {
            pts_us,
            duration_us: Some(33_000),
            width: 2,
            height: 1,
            is_key_frame: false,
            surface: Arc::new(VideoSurface::new_cpu_packed(
                PixelFormatCategory::Rgba8,
                8,
                vec![1, 2, 3, 4, 10, 20, 30, 40],
            )),
        }
    }

    #[test]
    fn passthrough_request_plans_passthrough_path() {
        let pipeline = RenderPlanner::new();
        let input = decoded_frame(10_000);

        let plan = pipeline.plan_render(
            VideoRenderRequest::passthrough(true),
            &input,
            no_backend_capabilities(),
        );

        assert_eq!(plan.path, VideoRenderPath::PassthroughWithSubtitleIntent);
        assert_eq!(plan.request, ConversionRequest::Passthrough);
    }

    #[test]
    fn request_can_express_bgra_output_preference_without_changing_current_passthrough() {
        let pipeline = RenderPlanner::new();
        let input = decoded_frame(66_000);

        // Passthrough intent + Bgra8 preference on already-BGRA CPU input →
        // resolved target matches input, so it stays passthrough
        let plan = pipeline.plan_render(
            VideoRenderRequest {
                target_intent: PresentationIntent::Passthrough,
                presentation_pixel_format: PresentationPixelFormatPreference::Bgra8,
                presentation_surface_kind: PresentationSurfaceKindPreference::PreserveInput,
                subtitles_visible: true,
            },
            &input,
            no_backend_capabilities(),
        );

        assert_eq!(plan.path, VideoRenderPath::PassthroughWithSubtitleIntent);
        assert_eq!(plan.request, ConversionRequest::Passthrough);
    }

    #[test]
    fn request_can_express_gpu_surface_preference_without_changing_current_passthrough() {
        let pipeline = RenderPlanner::new();
        let input = decoded_frame(99_000);

        // Passthrough intent + GpuTexture preference, but input is CpuPacked and
        // no backend supports GPU transform → UnsupportedTransform
        let plan = pipeline.plan_render(
            VideoRenderRequest {
                target_intent: PresentationIntent::Passthrough,
                presentation_pixel_format: PresentationPixelFormatPreference::PreserveInput,
                presentation_surface_kind: PresentationSurfaceKindPreference::GpuTexture,
                subtitles_visible: false,
            },
            &input,
            no_backend_capabilities(),
        );

        // Input is CpuPacked BGRA, target is GpuTexture BGRA — mismatch, no backend → unsupported
        assert_eq!(plan.path, VideoRenderPath::UnsupportedTransform);
    }

    #[test]
    fn cpu_bgra_compatibility_stays_passthrough_for_cpu_bgra_input() {
        let pipeline = RenderPlanner::new();
        let input = decoded_frame(123_000);

        let plan = pipeline.plan_render(
            VideoRenderRequest::cpu_bgra_compatibility(true),
            &input,
            no_backend_capabilities(),
        );

        assert_eq!(plan.path, VideoRenderPath::PassthroughWithSubtitleIntent);
    }

    #[test]
    fn cpu_bgra_compatibility_plans_convert_for_mismatched_format() {
        let pipeline = RenderPlanner::new();
        let input = rgba_frame(123_000);

        let plan = pipeline.plan_render(
            VideoRenderRequest::cpu_bgra_compatibility(false),
            &input,
            no_backend_capabilities(),
        );

        assert_eq!(plan.path, VideoRenderPath::RequiresTransform);
        assert_eq!(
            plan.request,
            ConversionRequest::Convert {
                target_pixel_format: PixelFormatCategory::Bgra8,
                target_surface_kind: VideoSurfaceKind::CpuPacked,
            }
        );
    }

    #[test]
    fn gpu_presenter_profile_marks_transform_requirement_for_cpu_input() {
        let pipeline = RenderPlanner::new();
        let input = decoded_frame(123_000);

        let plan = pipeline.plan_render(
            VideoRenderRequest::gpu_bgra_presenter(false),
            &input,
            no_backend_capabilities(),
        );

        assert_eq!(plan.path, VideoRenderPath::UnsupportedTransform);
    }

    #[test]
    fn explicit_surface_preference_can_override_profile_default() {
        let pipeline = RenderPlanner::new();
        let input = decoded_frame(123_000);

        let resolved = pipeline.resolve_request(
            VideoRenderRequest {
                target_intent: PresentationIntent::GpuBgraPresenter,
                presentation_pixel_format: PresentationPixelFormatPreference::PreserveInput,
                presentation_surface_kind: PresentationSurfaceKindPreference::CpuPacked,
                subtitles_visible: true,
            },
            &input,
        );

        assert_eq!(
            resolved.presentation_pixel_format,
            PixelFormatCategory::Bgra8
        );
        assert_eq!(
            resolved.presentation_surface_kind,
            VideoSurfaceKind::CpuPacked
        );
    }
}
