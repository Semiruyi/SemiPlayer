use crate::render::core::frame::{
    DecodedVideoFrame, PixelFormatCategory, PresentationFrame, VideoColorInfo, VideoFrame,
    VideoSurface,
};
use std::sync::Arc;

#[allow(dead_code)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct D3d11TextureSurfaceDesc {
    // Raw `ID3D11Texture2D*` value owned by the decode or render backend.
    pub texture_ptr: u64,
    pub shared_handle: Option<u64>,
    pub array_slice: u32,
    pub pixel_format: PixelFormatCategory,
    pub color_info: VideoColorInfo,
}

impl D3d11TextureSurfaceDesc {
    #[allow(dead_code)]
    pub fn into_surface(self) -> VideoSurface {
        VideoSurface::new_d3d11_texture_2d(
            self.pixel_format,
            self.texture_ptr,
            self.shared_handle,
            self.array_slice,
        )
        .with_color_info(self.color_info)
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct D3d11BgraRenderRequest {
    pub array_slice: u32,
    pub texture_ptr: u64,
    pub input_pixel_format: PixelFormatCategory,
    pub input_color_info: VideoColorInfo,
    pub shared_handle: Option<u64>,
    pub width: u32,
    pub height: u32,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct D3d11BgraRenderTargetDesc {
    pub array_slice: u32,
    pub texture_ptr: u64,
    pub shared_handle: Option<u64>,
    pub width: u32,
    pub height: u32,
}

impl D3d11BgraRenderTargetDesc {
    pub fn into_surface(self) -> VideoSurface {
        VideoSurface::new_d3d11_texture_2d(
            PixelFormatCategory::Bgra8,
            self.texture_ptr,
            self.shared_handle,
            self.array_slice,
        )
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum D3d11RenderPlanKind {
    CopyBgraTexture,
    Nv12ToBgraTexture,
    Yuv420pToBgraTexture,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct D3d11RenderPlan {
    pub input: D3d11BgraRenderRequest,
    pub output: D3d11BgraRenderTargetDesc,
    pub kind: D3d11RenderPlanKind,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum D3d11RenderError {
    UnsupportedInputSurface,
    UnsupportedPixelFormat,
    BackendUnavailable,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct D3d11RendererConfig {
    pub output_pool_hint: u32,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
#[allow(dead_code)]
pub struct D3d11RendererStateSnapshot {
    pub render_attempts: u64,
    pub successful_renders: u64,
    pub backend_unavailable_errors: u64,
    pub output_pool_hint: u32,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
struct D3d11RendererState {
    render_attempts: u64,
    successful_renders: u64,
    backend_unavailable_errors: u64,
}

pub fn build_bgra_render_request(
    frame: &DecodedVideoFrame,
) -> Result<D3d11BgraRenderRequest, D3d11RenderError> {
    match &frame.surface.storage {
        crate::render::core::frame::VideoSurfaceStorage::D3d11Texture2D {
            texture_ptr,
            shared_handle,
            array_slice,
        } => Ok(D3d11BgraRenderRequest {
            array_slice: *array_slice,
            texture_ptr: *texture_ptr,
            input_pixel_format: frame.pixel_format(),
            input_color_info: frame.color_info(),
            shared_handle: *shared_handle,
            width: frame.width,
            height: frame.height,
        }),
        crate::render::core::frame::VideoSurfaceStorage::CpuPacked { .. } => {
            Err(D3d11RenderError::UnsupportedInputSurface)
        }
    }
}

pub fn build_bgra_render_target_desc(request: D3d11BgraRenderRequest) -> D3d11BgraRenderTargetDesc {
    D3d11BgraRenderTargetDesc {
        array_slice: request.array_slice,
        texture_ptr: request.texture_ptr,
        shared_handle: request.shared_handle,
        width: request.width,
        height: request.height,
    }
}

pub fn plan_bgra_texture_render(
    frame: &DecodedVideoFrame,
) -> Result<D3d11RenderPlan, D3d11RenderError> {
    let input = build_bgra_render_request(frame)?;
    let kind = match input.input_pixel_format {
        PixelFormatCategory::Bgra8 => D3d11RenderPlanKind::CopyBgraTexture,
        PixelFormatCategory::Nv12 => D3d11RenderPlanKind::Nv12ToBgraTexture,
        PixelFormatCategory::Yuv420p => D3d11RenderPlanKind::Yuv420pToBgraTexture,
        _ => return Err(D3d11RenderError::UnsupportedPixelFormat),
    };
    let output = build_bgra_render_target_desc(input);

    Ok(D3d11RenderPlan {
        input,
        output,
        kind,
    })
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct D3d11Renderer {
    config: D3d11RendererConfig,
    state: D3d11RendererState,
}

impl D3d11Renderer {
    pub fn new() -> Self {
        Self::with_config(D3d11RendererConfig::default())
    }

    pub fn with_config(config: D3d11RendererConfig) -> Self {
        Self {
            config,
            state: D3d11RendererState::default(),
        }
    }

    pub fn plan_frame(
        &self,
        frame: &DecodedVideoFrame,
    ) -> Result<D3d11RenderPlan, D3d11RenderError> {
        let _ = self.config.output_pool_hint;
        plan_bgra_texture_render(frame)
    }

    pub fn render_frame(
        &mut self,
        frame: &DecodedVideoFrame,
    ) -> Result<PresentationFrame, D3d11RenderError> {
        self.state.render_attempts = self.state.render_attempts.saturating_add(1);
        let plan = self.plan_frame(frame)?;
        match self.execute_plan(frame, plan) {
            Ok(presentation_frame) => {
                self.state.successful_renders = self.state.successful_renders.saturating_add(1);
                Ok(presentation_frame)
            }
            Err(error) => {
                if error == D3d11RenderError::BackendUnavailable {
                    self.state.backend_unavailable_errors =
                        self.state.backend_unavailable_errors.saturating_add(1);
                }
                Err(error)
            }
        }
    }

    fn execute_plan(
        &self,
        frame: &DecodedVideoFrame,
        plan: D3d11RenderPlan,
    ) -> Result<PresentationFrame, D3d11RenderError> {
        let _ = self.config.output_pool_hint;
        match plan.kind {
            D3d11RenderPlanKind::CopyBgraTexture => Ok(VideoFrame {
                pts_us: frame.pts_us,
                duration_us: frame.duration_us,
                width: frame.width,
                height: frame.height,
                is_key_frame: frame.is_key_frame,
                surface: Arc::new(plan.output.into_surface()),
            }),
            D3d11RenderPlanKind::Nv12ToBgraTexture | D3d11RenderPlanKind::Yuv420pToBgraTexture => {
                Err(D3d11RenderError::BackendUnavailable)
            }
        }
    }

    #[allow(dead_code)]
    pub fn snapshot(&self) -> D3d11RendererStateSnapshot {
        D3d11RendererStateSnapshot {
            render_attempts: self.state.render_attempts,
            successful_renders: self.state.successful_renders,
            backend_unavailable_errors: self.state.backend_unavailable_errors,
            output_pool_hint: self.config.output_pool_hint,
        }
    }
}

// Transitional compatibility helper for the current pipeline call site.
// The preferred long-term ownership is: player -> render service -> renderer instance.
#[allow(dead_code)]
pub fn try_render_to_bgra_texture(
    frame: &DecodedVideoFrame,
) -> Result<PresentationFrame, D3d11RenderError> {
    let mut renderer = D3d11Renderer::new();
    renderer.render_frame(frame)
}

#[cfg(test)]
mod tests {
    use super::{
        build_bgra_render_request, build_bgra_render_target_desc, plan_bgra_texture_render,
        try_render_to_bgra_texture, D3d11BgraRenderRequest, D3d11RenderError, D3d11RenderPlanKind,
        D3d11Renderer, D3d11RendererConfig, D3d11RendererStateSnapshot,
    };
    use crate::render::core::frame::{
        PixelFormatCategory, VideoColorInfo, VideoFrame, VideoSurface,
    };
    use std::sync::Arc;

    fn d3d11_frame(pixel_format: PixelFormatCategory) -> VideoFrame {
        VideoFrame {
            pts_us: 0,
            duration_us: Some(33_000),
            width: 1920,
            height: 1080,
            is_key_frame: false,
            surface: Arc::new(
                VideoSurface::new_d3d11_texture_2d(pixel_format, 0x1234, None, 0)
                    .with_color_info(VideoColorInfo::default()),
            ),
        }
    }

    #[test]
    fn nv12_texture_request_reaches_d3d11_backend_skeleton() {
        let frame = d3d11_frame(PixelFormatCategory::Nv12);

        let result = try_render_to_bgra_texture(&frame);

        assert!(matches!(result, Err(D3d11RenderError::BackendUnavailable)));
    }

    #[test]
    fn bgra_texture_request_can_pass_through_d3d11_renderer() {
        let frame = d3d11_frame(PixelFormatCategory::Bgra8);

        let output = try_render_to_bgra_texture(&frame).expect("bgra passthrough");

        assert_eq!(output.pixel_format(), PixelFormatCategory::Bgra8);
        assert_eq!(output.width, frame.width);
        assert_eq!(output.height, frame.height);
        assert_eq!(output.surface_kind(), frame.surface_kind());
    }

    #[test]
    fn cpu_surface_is_rejected_by_d3d11_backend() {
        let frame = VideoFrame {
            pts_us: 0,
            duration_us: Some(33_000),
            width: 2,
            height: 1,
            is_key_frame: false,
            surface: Arc::new(VideoSurface::new_cpu_packed(
                PixelFormatCategory::Bgra8,
                8,
                vec![0; 8],
            )),
        };

        let result = try_render_to_bgra_texture(&frame);

        assert!(matches!(
            result,
            Err(D3d11RenderError::UnsupportedInputSurface)
        ));
    }

    #[test]
    fn build_request_preserves_d3d11_surface_metadata() {
        let frame = VideoFrame {
            pts_us: 0,
            duration_us: Some(33_000),
            width: 1280,
            height: 720,
            is_key_frame: false,
            surface: Arc::new(
                VideoSurface::new_d3d11_texture_2d(
                    PixelFormatCategory::Nv12,
                    0x9876,
                    Some(0x5555),
                    3,
                )
                .with_color_info(VideoColorInfo::default()),
            ),
        };

        let request = build_bgra_render_request(&frame).expect("d3d11 surface request");

        assert_eq!(request.texture_ptr, 0x9876);
        assert_eq!(request.shared_handle, Some(0x5555));
        assert_eq!(request.array_slice, 3);
        assert_eq!(request.input_pixel_format, PixelFormatCategory::Nv12);
        assert_eq!(request.input_color_info, VideoColorInfo::default());
        assert_eq!(request.width, 1280);
        assert_eq!(request.height, 720);
    }

    #[test]
    fn build_target_desc_normalizes_output_to_bgra_surface() {
        let request = D3d11BgraRenderRequest {
            array_slice: 4,
            texture_ptr: 0x2000,
            input_pixel_format: PixelFormatCategory::Nv12,
            input_color_info: VideoColorInfo::default(),
            shared_handle: Some(0x3000),
            width: 640,
            height: 360,
        };

        let target = build_bgra_render_target_desc(request);
        let surface = target.into_surface();

        assert_eq!(target.texture_ptr, 0x2000);
        assert_eq!(target.shared_handle, Some(0x3000));
        assert_eq!(target.array_slice, 4);
        assert_eq!(target.width, 640);
        assert_eq!(target.height, 360);
        assert_eq!(surface.pixel_format, PixelFormatCategory::Bgra8);
    }

    #[test]
    fn nv12_texture_plans_nv12_to_bgra_transform() {
        let frame = d3d11_frame(PixelFormatCategory::Nv12);

        let plan = plan_bgra_texture_render(&frame).expect("render plan");

        assert_eq!(plan.kind, D3d11RenderPlanKind::Nv12ToBgraTexture);
        assert_eq!(plan.input.input_pixel_format, PixelFormatCategory::Nv12);
        assert_eq!(plan.output.texture_ptr, plan.input.texture_ptr);
        assert_eq!(plan.output.width, frame.width);
        assert_eq!(plan.output.height, frame.height);
    }

    #[test]
    fn bgra_texture_plans_copy_path() {
        let frame = d3d11_frame(PixelFormatCategory::Bgra8);
        let renderer = D3d11Renderer::new();

        let plan = renderer.plan_frame(&frame).expect("copy plan");

        assert_eq!(plan.kind, D3d11RenderPlanKind::CopyBgraTexture);
    }

    #[test]
    fn renderer_snapshot_reports_stateful_attempt_counters() {
        let frame = d3d11_frame(PixelFormatCategory::Nv12);
        let mut renderer = D3d11Renderer::with_config(D3d11RendererConfig {
            output_pool_hint: 3,
        });

        let result = renderer.render_frame(&frame);

        assert!(matches!(result, Err(D3d11RenderError::BackendUnavailable)));
        assert_eq!(
            renderer.snapshot(),
            D3d11RendererStateSnapshot {
                render_attempts: 1,
                successful_renders: 0,
                backend_unavailable_errors: 1,
                output_pool_hint: 3,
            }
        );
    }

    #[test]
    fn renderer_snapshot_reports_successful_copy_path() {
        let frame = d3d11_frame(PixelFormatCategory::Bgra8);
        let mut renderer = D3d11Renderer::new();

        let output = renderer.render_frame(&frame).expect("copy render");

        assert_eq!(output.pixel_format(), PixelFormatCategory::Bgra8);
        assert_eq!(
            renderer.snapshot(),
            D3d11RendererStateSnapshot {
                render_attempts: 1,
                successful_renders: 1,
                backend_unavailable_errors: 0,
                output_pool_hint: 0,
            }
        );
    }
}
