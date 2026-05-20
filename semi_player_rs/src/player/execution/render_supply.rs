use crate::player::handle::SemiPlayerHandle;
use crate::render::core::frame::{DecodedVideoFrame, PresentationFrame};
use crate::render::core::pipeline::{VideoRenderBatch, VideoRenderRequest, VideoRenderStats};
use crate::util::debug_trace::append_trace_line;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct RenderSupplyPlan {
    request: VideoRenderRequest,
    generation: u64,
}

#[derive(Debug)]
struct RenderSupplyStage {
    request: VideoRenderRequest,
    generation: u64,
    decoded_frames: Vec<DecodedVideoFrame>,
}

#[derive(Debug)]
struct RenderSupplyExecution {
    generation: u64,
    rendered_frames: Vec<PresentationFrame>,
    result: RenderSupplyResult,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub(crate) struct RenderSupplyResult {
    pub rendered_frames: usize,
    pub passthrough_frames: usize,
    pub passthrough_with_subtitle_intent_frames: usize,
    pub requires_transform_frames: usize,
    pub fallback_passthrough_frames: usize,
}

impl RenderSupplyResult {
    pub fn has_new_presentation_frames(self) -> bool {
        self.rendered_frames > 0
    }
}

pub(crate) fn render_supply(player: &SemiPlayerHandle) -> RenderSupplyResult {
    let plan = plan_render_supply(player);
    let Some(stage) = stage_render_supply(player, plan) else {
        return RenderSupplyResult::default();
    };
    let execution = execute_render_supply(player, stage);
    commit_render_supply(player, execution)
}

fn plan_render_supply(player: &SemiPlayerHandle) -> RenderSupplyPlan {
    RenderSupplyPlan {
        request: default_render_request(player),
        generation: player.media_generation(),
    }
}

fn stage_render_supply(
    player: &SemiPlayerHandle,
    plan: RenderSupplyPlan,
) -> Option<RenderSupplyStage> {
    let decoded_frames =
        player.with_runtime_access(|mut runtime| runtime.begin_render_stage(plan.generation))?;

    Some(RenderSupplyStage {
        request: plan.request,
        generation: plan.generation,
        decoded_frames,
    })
}

fn execute_render_supply(
    player: &SemiPlayerHandle,
    stage: RenderSupplyStage,
) -> RenderSupplyExecution {
    let batch = player.with_render_access_mut(|render| {
        append_trace_line("render_supply:execute render_frames begin");
        render
            .render
            .render_frames(stage.request, stage.decoded_frames)
    });
    append_trace_line("render_supply:execute render_frames end");
    render_execution_from_batch(stage.generation, batch)
}

fn commit_render_supply(
    player: &SemiPlayerHandle,
    execution: RenderSupplyExecution,
) -> RenderSupplyResult {
    if execution.generation != player.media_generation() {
        player.with_runtime_access(|mut runtime| {
            runtime.commit_render_stage(Vec::new());
        });
        return RenderSupplyResult::default();
    }

    let has_new_presentation_frames = execution.result.has_new_presentation_frames();
    player.with_runtime_access(|mut runtime| {
        runtime.commit_render_stage(execution.rendered_frames);
        if has_new_presentation_frames {
            runtime.mark_video_sync_dirty();
        }
    });
    player.observe_render_stats(
        execution.result.rendered_frames,
        execution.result.passthrough_frames,
        execution.result.passthrough_with_subtitle_intent_frames,
        execution.result.requires_transform_frames,
        execution.result.fallback_passthrough_frames,
    );

    execution.result
}

fn render_execution_from_batch(generation: u64, batch: VideoRenderBatch) -> RenderSupplyExecution {
    RenderSupplyExecution {
        generation,
        result: render_stats_to_result(batch.stats),
        rendered_frames: batch.frames,
    }
}

fn default_render_request(player: &SemiPlayerHandle) -> VideoRenderRequest {
    VideoRenderRequest::from_target_profile(
        player.video_presentation_profile(),
        player.subtitles_visible(),
    )
}

fn render_stats_to_result(stats: VideoRenderStats) -> RenderSupplyResult {
    RenderSupplyResult {
        rendered_frames: stats.rendered_frames,
        passthrough_frames: stats.passthrough_frames,
        passthrough_with_subtitle_intent_frames: stats.passthrough_with_subtitle_intent_frames,
        requires_transform_frames: stats.requires_transform_frames,
        fallback_passthrough_frames: stats.fallback_passthrough_frames,
    }
}

#[cfg(test)]
mod tests {
    use std::sync::Arc;

    use super::{
        commit_render_supply, default_render_request, execute_render_supply, plan_render_supply,
        render_supply, stage_render_supply, RenderSupplyResult,
    };
    use crate::player::handle::SemiPlayerHandle;
    use crate::render::core::frame::{PixelFormatCategory, VideoFrame, VideoSurface};
    use crate::render::core::pipeline::VideoRenderRequest;

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

    #[test]
    fn synchronous_render_supply_promotes_all_decoded_frames() {
        let mut player = SemiPlayerHandle::new();
        player.runtime.get_mut().unwrap().runtime.push_decoded_video_frame(decoded_frame(0));
        player
            .runtime.get_mut().unwrap().runtime
            .push_decoded_video_frame(decoded_frame(33_000));

        let result = render_supply(&player);

        assert_eq!(
            result,
            RenderSupplyResult {
                rendered_frames: 2,
                passthrough_frames: 0,
                passthrough_with_subtitle_intent_frames: 2,
                requires_transform_frames: 0,
                fallback_passthrough_frames: 0,
            }
        );
        assert_eq!(player.runtime.get_mut().unwrap().runtime.decoded_video_queue_len(), 0);
        assert_eq!(player.runtime.get_mut().unwrap().runtime.presentation_video_queue_len(), 2);
        assert!(result.has_new_presentation_frames());
    }

    #[test]
    fn render_supply_reads_subtitle_visibility_from_player_state() {
        let mut player = SemiPlayerHandle::new();
        player.set_subtitles_visible(false);
        player.runtime.get_mut().unwrap().runtime.push_decoded_video_frame(decoded_frame(0));

        let result = render_supply(&player);

        assert_eq!(
            result,
            RenderSupplyResult {
                rendered_frames: 1,
                passthrough_frames: 1,
                passthrough_with_subtitle_intent_frames: 0,
                requires_transform_frames: 0,
                fallback_passthrough_frames: 0,
            }
        );
        assert_eq!(player.runtime.get_mut().unwrap().runtime.presentation_video_queue_len(), 1);
    }

    #[test]
    fn default_render_request_targets_cpu_bgra_compatibility() {
        let player = SemiPlayerHandle::new();

        let request = default_render_request(&player);

        assert_eq!(request, VideoRenderRequest::cpu_bgra_compatibility(true));
    }

    #[test]
    fn default_render_request_follows_player_profile() {
        let player = SemiPlayerHandle::new();
        player.set_video_presentation_profile(
            crate::render::core::pipeline::PresentationTargetProfile::D3d11BgraPresenter,
        );

        let request = default_render_request(&player);

        assert_eq!(request, VideoRenderRequest::d3d11_bgra_presenter(true));
    }

    #[test]
    fn gpu_texture_with_passthrough_profile_does_not_attempt_transform() {
        let mut player = SemiPlayerHandle::new();
        player.set_video_presentation_profile(
            crate::render::core::pipeline::PresentationTargetProfile::Passthrough,
        );
        player.runtime.get_mut().unwrap().runtime.push_decoded_video_frame(VideoFrame {
            pts_us: 0,
            duration_us: Some(33_000),
            width: 1920,
            height: 1080,
            is_key_frame: false,
            surface: Arc::new(VideoSurface::new_d3d11_texture_2d(
                PixelFormatCategory::Nv12,
                0x1234,
                None,
                0,
            )),
        });

        let result = render_supply(&player);

        assert_eq!(
            result,
            RenderSupplyResult {
                rendered_frames: 1,
                passthrough_frames: 0,
                passthrough_with_subtitle_intent_frames: 1,
                requires_transform_frames: 0,
                fallback_passthrough_frames: 0,
            }
        );
        assert_eq!(player.runtime.get_mut().unwrap().runtime.presentation_video_queue_len(), 1);
    }

    #[test]
    fn staged_render_supply_keeps_runtime_in_flight_until_commit() {
        let mut player = SemiPlayerHandle::new();
        player.runtime.get_mut().unwrap().runtime.push_decoded_video_frame(decoded_frame(0));
        player
            .runtime.get_mut().unwrap().runtime
            .push_decoded_video_frame(decoded_frame(33_000));

        let plan = plan_render_supply(&player);
        let stage = stage_render_supply(&player, plan).expect("render stage");

        assert_eq!(player.runtime.get_mut().unwrap().runtime.decoded_video_queue_len(), 0);
        assert_eq!(
            player
                .runtime.get_mut().unwrap().runtime
                .render_staging_status()
                .in_flight_decoded_video_queue_len,
            2
        );
        assert_eq!(
            player.runtime.get_mut().unwrap().runtime.render_staging_status().in_flight_generation,
            Some(plan.generation)
        );
        assert!(!player.runtime.get_mut().unwrap().video_sync.is_dirty());

        let execution = execute_render_supply(&player, stage);
        let result = commit_render_supply(&player, execution);

        assert!(result.has_new_presentation_frames());
        assert_eq!(
            player
                .runtime.get_mut().unwrap().runtime
                .render_staging_status()
                .in_flight_decoded_video_queue_len,
            0
        );
        assert_eq!(player.runtime.get_mut().unwrap().runtime.presentation_video_queue_len(), 2);
        assert!(player.runtime.get_mut().unwrap().video_sync.is_dirty());
    }

    #[test]
    fn stale_generation_render_execution_is_dropped_at_commit() {
        let mut player = SemiPlayerHandle::new();
        player.runtime.get_mut().unwrap().runtime.push_decoded_video_frame(decoded_frame(0));

        let plan = plan_render_supply(&player);
        let stage = stage_render_supply(&player, plan).expect("render stage");

        let _ = player.bump_media_generation();
        let execution = execute_render_supply(&player, stage);
        let result = commit_render_supply(&player, execution);

        assert_eq!(result, RenderSupplyResult::default());
        assert_eq!(player.runtime.get_mut().unwrap().runtime.presentation_video_queue_len(), 0);
        assert_eq!(player.runtime.get_mut().unwrap().runtime.decoded_video_queue_len(), 0);
        assert_eq!(
            player
                .runtime.get_mut().unwrap().runtime
                .render_staging_status()
                .in_flight_decoded_video_queue_len,
            0
        );
        assert_eq!(player.runtime.get_mut().unwrap().runtime.render_staging_status().in_flight_generation, None);
        assert!(!player.runtime.get_mut().unwrap().video_sync.is_dirty());
    }
}
