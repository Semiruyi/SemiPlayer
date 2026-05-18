use crate::render::backends::d3d11::D3d11Renderer;
use crate::render::core::frame::{
    DecodedVideoFrame, PixelFormatCategory, PresentationFrame, VideoSurfaceKind,
};
use crate::render::pipelines::{cpu_bgra, d3d11_presenter};

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
pub enum PresentationTargetProfile {
    Passthrough,
    CpuBgraCompatibility,
    D3d11BgraPresenter,
}

impl Default for PresentationTargetProfile {
    fn default() -> Self {
        Self::Passthrough
    }
}

#[allow(dead_code)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum PresentationSurfaceKindPreference {
    PreserveInput,
    CpuPacked,
    D3d11Texture2D,
}

impl Default for PresentationSurfaceKindPreference {
    fn default() -> Self {
        Self::PreserveInput
    }
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct VideoRenderRequest {
    pub target_profile: PresentationTargetProfile,
    pub presentation_pixel_format: PresentationPixelFormatPreference,
    pub presentation_surface_kind: PresentationSurfaceKindPreference,
    pub subtitles_visible: bool,
}

impl VideoRenderRequest {
    pub fn from_target_profile(
        target_profile: PresentationTargetProfile,
        subtitles_visible: bool,
    ) -> Self {
        match target_profile {
            PresentationTargetProfile::Passthrough => Self::passthrough(subtitles_visible),
            PresentationTargetProfile::CpuBgraCompatibility => {
                Self::cpu_bgra_compatibility(subtitles_visible)
            }
            PresentationTargetProfile::D3d11BgraPresenter => {
                Self::d3d11_bgra_presenter(subtitles_visible)
            }
        }
    }

    #[allow(dead_code)]
    pub fn passthrough(subtitles_visible: bool) -> Self {
        Self {
            target_profile: PresentationTargetProfile::Passthrough,
            presentation_pixel_format: PresentationPixelFormatPreference::PreserveInput,
            presentation_surface_kind: PresentationSurfaceKindPreference::PreserveInput,
            subtitles_visible,
        }
    }

    #[allow(dead_code)]
    pub fn cpu_bgra_compatibility(subtitles_visible: bool) -> Self {
        Self {
            target_profile: PresentationTargetProfile::CpuBgraCompatibility,
            presentation_pixel_format: PresentationPixelFormatPreference::Bgra8,
            presentation_surface_kind: PresentationSurfaceKindPreference::CpuPacked,
            subtitles_visible,
        }
    }

    #[allow(dead_code)]
    pub fn d3d11_bgra_presenter(subtitles_visible: bool) -> Self {
        Self {
            target_profile: PresentationTargetProfile::D3d11BgraPresenter,
            presentation_pixel_format: PresentationPixelFormatPreference::Bgra8,
            presentation_surface_kind: PresentationSurfaceKindPreference::D3d11Texture2D,
            subtitles_visible,
        }
    }
}

#[derive(Clone, Copy, Debug, Default)]
pub struct VideoRenderPipeline;

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
    pub frames: Vec<PresentationFrame>,
    pub stats: VideoRenderStats,
}

impl VideoRenderPipeline {
    #[allow(dead_code)]
    pub fn new() -> Self {
        Self
    }

    #[allow(dead_code)]
    pub fn render_frame(
        &self,
        request: VideoRenderRequest,
        frame: DecodedVideoFrame,
    ) -> PresentationFrame {
        let mut d3d11_renderer = D3d11Renderer::new();
        self.render_frame_with_d3d11_renderer(request, frame, &mut d3d11_renderer)
    }

    pub fn render_frame_with_d3d11_renderer(
        &self,
        request: VideoRenderRequest,
        frame: DecodedVideoFrame,
        d3d11_renderer: &mut D3d11Renderer,
    ) -> PresentationFrame {
        let plan = self.plan_render(request, &frame);
        self.execute_render_plan(&plan, frame, d3d11_renderer).0
    }

    #[allow(dead_code)]
    pub fn render_frames(
        &self,
        request: VideoRenderRequest,
        frames: impl IntoIterator<Item = DecodedVideoFrame>,
    ) -> VideoRenderBatch {
        let mut d3d11_renderer = D3d11Renderer::new();
        self.render_frames_with_d3d11_renderer(request, frames, &mut d3d11_renderer)
    }

    pub fn render_frames_with_d3d11_renderer(
        &self,
        request: VideoRenderRequest,
        frames: impl IntoIterator<Item = DecodedVideoFrame>,
        d3d11_renderer: &mut D3d11Renderer,
    ) -> VideoRenderBatch {
        let mut batch = VideoRenderBatch::default();

        for frame in frames {
            let plan = self.plan_render(request, &frame);
            match plan.path {
                VideoRenderPath::Passthrough => {
                    batch.stats.passthrough_frames =
                        batch.stats.passthrough_frames.saturating_add(1);
                }
                VideoRenderPath::PassthroughWithSubtitleIntent => {
                    batch.stats.passthrough_with_subtitle_intent_frames = batch
                        .stats
                        .passthrough_with_subtitle_intent_frames
                        .saturating_add(1);
                }
                VideoRenderPath::RequiresTransform => {
                    batch.stats.requires_transform_frames =
                        batch.stats.requires_transform_frames.saturating_add(1);
                }
            }

            let (rendered_frame, fell_back) =
                self.execute_render_plan(&plan, frame, d3d11_renderer);
            if fell_back {
                batch.stats.fallback_passthrough_frames =
                    batch.stats.fallback_passthrough_frames.saturating_add(1);
            }
            batch.frames.push(rendered_frame);
            batch.stats.rendered_frames = batch.stats.rendered_frames.saturating_add(1);
        }

        batch
    }

    fn plan_render(
        &self,
        request: VideoRenderRequest,
        frame: &DecodedVideoFrame,
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
        } else {
            VideoRenderPath::RequiresTransform
        };

        VideoRenderPlan { target, path }
    }

    fn resolve_request(
        &self,
        request: VideoRenderRequest,
        frame: &DecodedVideoFrame,
    ) -> ResolvedVideoRenderRequest {
        let profile_pixel_format = match request.target_profile {
            PresentationTargetProfile::Passthrough => {
                PresentationPixelFormatPreference::PreserveInput
            }
            PresentationTargetProfile::CpuBgraCompatibility => {
                PresentationPixelFormatPreference::Bgra8
            }
            PresentationTargetProfile::D3d11BgraPresenter => {
                PresentationPixelFormatPreference::Bgra8
            }
        };
        let profile_surface_kind = match request.target_profile {
            PresentationTargetProfile::Passthrough => {
                PresentationSurfaceKindPreference::PreserveInput
            }
            PresentationTargetProfile::CpuBgraCompatibility => {
                PresentationSurfaceKindPreference::CpuPacked
            }
            PresentationTargetProfile::D3d11BgraPresenter => {
                PresentationSurfaceKindPreference::D3d11Texture2D
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
                PresentationSurfaceKindPreference::D3d11Texture2D => {
                    VideoSurfaceKind::D3d11Texture2D
                }
            },
        }
    }

    fn try_render_transform(
        &self,
        plan: &VideoRenderPlan,
        frame: DecodedVideoFrame,
        d3d11_renderer: &mut D3d11Renderer,
    ) -> Result<PresentationFrame, DecodedVideoFrame> {
        match (
            plan.target.presentation_surface_kind,
            plan.target.presentation_pixel_format,
        ) {
            (VideoSurfaceKind::CpuPacked, PixelFormatCategory::Bgra8) => {
                cpu_bgra::try_render(frame)
            }
            (VideoSurfaceKind::D3d11Texture2D, PixelFormatCategory::Bgra8) => {
                d3d11_presenter::try_render(frame, d3d11_renderer)
            }
            _ => Err(frame),
        }
    }

    fn execute_render_plan(
        &self,
        plan: &VideoRenderPlan,
        frame: DecodedVideoFrame,
        d3d11_renderer: &mut D3d11Renderer,
    ) -> (PresentationFrame, bool) {
        match plan.path {
            VideoRenderPath::Passthrough | VideoRenderPath::PassthroughWithSubtitleIntent => {
                (frame, false)
            }
            VideoRenderPath::RequiresTransform => {
                match self.try_render_transform(plan, frame, d3d11_renderer) {
                    Ok(transformed_frame) => (transformed_frame, false),
                    Err(frame) => {
                        let _target_pixel_format = plan.target.presentation_pixel_format;
                        let _target_surface_kind = plan.target.presentation_surface_kind;
                        (frame, true)
                    }
                }
            }
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum VideoRenderPath {
    Passthrough,
    PassthroughWithSubtitleIntent,
    RequiresTransform,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct VideoRenderPlan {
    target: ResolvedVideoRenderRequest,
    path: VideoRenderPath,
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
        PresentationPixelFormatPreference, PresentationSurfaceKindPreference,
        PresentationTargetProfile, VideoRenderPath, VideoRenderPipeline, VideoRenderRequest,
        VideoRenderStats,
    };
    use crate::render::core::frame::{
        PixelFormatCategory, VideoFrame, VideoSurface, VideoSurfaceKind,
    };

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
    fn passthrough_pipeline_preserves_timing_and_surface_shape() {
        let pipeline = VideoRenderPipeline::new();
        let input = decoded_frame(33_000);

        let output = pipeline.render_frame(VideoRenderRequest::default(), input.clone());

        assert_eq!(output.pts_us, input.pts_us);
        assert_eq!(output.duration_us, input.duration_us);
        assert_eq!(output.width, input.width);
        assert_eq!(output.height, input.height);
        assert_eq!(output.pixel_format(), input.pixel_format());
        assert_eq!(output.byte_len(), input.byte_len());
    }

    #[test]
    fn passthrough_request_plans_passthrough_path() {
        let pipeline = VideoRenderPipeline::new();
        let input = decoded_frame(10_000);

        let plan = pipeline.plan_render(VideoRenderRequest::passthrough(true), &input);

        assert_eq!(plan.path, VideoRenderPath::PassthroughWithSubtitleIntent);
        assert_eq!(plan.target.presentation_pixel_format, input.pixel_format());
        assert_eq!(plan.target.presentation_surface_kind, input.surface_kind());
    }

    #[test]
    fn request_can_express_bgra_output_preference_without_changing_current_passthrough() {
        let pipeline = VideoRenderPipeline::new();
        let input = decoded_frame(66_000);

        let output = pipeline.render_frame(
            VideoRenderRequest {
                target_profile: PresentationTargetProfile::Passthrough,
                presentation_pixel_format: PresentationPixelFormatPreference::Bgra8,
                presentation_surface_kind: PresentationSurfaceKindPreference::PreserveInput,
                subtitles_visible: true,
            },
            input.clone(),
        );

        assert_eq!(output.pts_us, input.pts_us);
        assert_eq!(output.pixel_format(), input.pixel_format());
    }

    #[test]
    fn request_can_express_d3d11_surface_preference_without_changing_current_passthrough() {
        let pipeline = VideoRenderPipeline::new();
        let input = decoded_frame(99_000);

        let output = pipeline.render_frame(
            VideoRenderRequest {
                target_profile: PresentationTargetProfile::Passthrough,
                presentation_pixel_format: PresentationPixelFormatPreference::PreserveInput,
                presentation_surface_kind: PresentationSurfaceKindPreference::D3d11Texture2D,
                subtitles_visible: false,
            },
            input.clone(),
        );

        assert_eq!(output.pts_us, input.pts_us);
        assert_eq!(output.surface_kind(), VideoSurfaceKind::CpuPacked);
    }

    #[test]
    fn cpu_bgra_compatibility_profile_resolves_to_cpu_bgra_targets() {
        let pipeline = VideoRenderPipeline::new();
        let input = decoded_frame(123_000);

        let resolved =
            pipeline.resolve_request(VideoRenderRequest::cpu_bgra_compatibility(true), &input);

        assert_eq!(
            resolved.presentation_pixel_format,
            PixelFormatCategory::Bgra8
        );
        assert_eq!(
            resolved.presentation_surface_kind,
            VideoSurfaceKind::CpuPacked
        );
    }

    #[test]
    fn cpu_bgra_compatibility_stays_passthrough_for_cpu_bgra_input() {
        let pipeline = VideoRenderPipeline::new();
        let input = decoded_frame(123_000);

        let plan = pipeline.plan_render(VideoRenderRequest::cpu_bgra_compatibility(true), &input);

        assert_eq!(plan.path, VideoRenderPath::PassthroughWithSubtitleIntent);
    }

    #[test]
    fn cpu_bgra_compatibility_transforms_rgba_cpu_input_to_bgra() {
        let pipeline = VideoRenderPipeline::new();
        let input = rgba_frame(123_000);

        let output =
            pipeline.render_frame(VideoRenderRequest::cpu_bgra_compatibility(false), input);

        assert_eq!(output.pixel_format(), PixelFormatCategory::Bgra8);
        assert_eq!(output.surface_kind(), VideoSurfaceKind::CpuPacked);
        assert_eq!(
            output.cpu_packed_data(),
            Some([3, 2, 1, 4, 30, 20, 10, 40].as_slice())
        );
    }

    #[test]
    fn d3d11_presenter_profile_resolves_to_d3d11_bgra_targets() {
        let pipeline = VideoRenderPipeline::new();
        let input = decoded_frame(123_000);

        let resolved =
            pipeline.resolve_request(VideoRenderRequest::d3d11_bgra_presenter(false), &input);

        assert_eq!(
            resolved.presentation_pixel_format,
            PixelFormatCategory::Bgra8
        );
        assert_eq!(
            resolved.presentation_surface_kind,
            VideoSurfaceKind::D3d11Texture2D
        );
    }

    #[test]
    fn d3d11_presenter_profile_marks_transform_requirement_for_cpu_input() {
        let pipeline = VideoRenderPipeline::new();
        let input = decoded_frame(123_000);

        let plan = pipeline.plan_render(VideoRenderRequest::d3d11_bgra_presenter(false), &input);

        assert_eq!(plan.path, VideoRenderPath::RequiresTransform);
    }

    #[test]
    fn explicit_surface_preference_can_override_profile_default() {
        let pipeline = VideoRenderPipeline::new();
        let input = decoded_frame(123_000);

        let resolved = pipeline.resolve_request(
            VideoRenderRequest {
                target_profile: PresentationTargetProfile::D3d11BgraPresenter,
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

    #[test]
    fn render_batch_reports_transform_requirements() {
        let pipeline = VideoRenderPipeline::new();
        let frames = vec![decoded_frame(0), decoded_frame(33_000)];

        let batch = pipeline.render_frames(VideoRenderRequest::d3d11_bgra_presenter(false), frames);

        assert_eq!(batch.frames.len(), 2);
        assert_eq!(
            batch.stats,
            VideoRenderStats {
                rendered_frames: 2,
                passthrough_frames: 0,
                passthrough_with_subtitle_intent_frames: 0,
                requires_transform_frames: 2,
                fallback_passthrough_frames: 2,
            }
        );
    }

    #[test]
    fn render_batch_does_not_count_supported_cpu_transform_as_fallback() {
        let pipeline = VideoRenderPipeline::new();
        let frames = vec![rgba_frame(0)];

        let batch =
            pipeline.render_frames(VideoRenderRequest::cpu_bgra_compatibility(false), frames);

        assert_eq!(batch.frames.len(), 1);
        assert_eq!(batch.frames[0].pixel_format(), PixelFormatCategory::Bgra8);
        assert_eq!(
            batch.stats,
            VideoRenderStats {
                rendered_frames: 1,
                passthrough_frames: 0,
                passthrough_with_subtitle_intent_frames: 0,
                requires_transform_frames: 1,
                fallback_passthrough_frames: 0,
            }
        );
    }
}
