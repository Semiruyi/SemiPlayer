use crate::api::types::PlayerState;
use crate::audio::core::output::AudioOutputChunk;
use crate::audio::core::output_controller::SharedAudioOutputController;
use crate::player::access::{PlaybackAdvanceObserveContext, PlaybackAdvancePlanContext};
use crate::player::handle::SemiPlayerHandle;
use crate::player::runtime::AudioDiscardSummary;
use crate::sync::video_sync::VideoSyncService;

pub(crate) struct PlaybackAdvancePlan {
    audio_output: SharedAudioOutputController,
    state: PlayerState,
    initial_discard: AudioDiscardSummary,
    audio_chunks: Vec<AudioOutputChunk>,
    audio_format: Option<crate::audio::core::output::AudioStreamFormat>,
}

pub(crate) struct PlaybackAdvanceResult {
    audio_chunks: Vec<AudioOutputChunk>,
    audio_chunks_submitted: bool,
}

pub(crate) fn plan_playback_advance(player: &mut SemiPlayerHandle) -> PlaybackAdvancePlan {
    let context = player.playback_advance_plan_context();
    build_playback_advance_plan(context)
}

fn build_playback_advance_plan(context: PlaybackAdvancePlanContext) -> PlaybackAdvancePlan {
    let _ = (
        context.initial_playback_time_us,
        context.request_frame_count,
        context.max_chunks,
    );

    PlaybackAdvancePlan {
        audio_output: context.audio_output,
        state: context.state,
        initial_discard: context.initial_discard,
        audio_chunks: context.audio_chunks,
        audio_format: context.audio_format,
    }
}

fn observe_seek_stable_if_ready(
    player: &SemiPlayerHandle,
    observe: PlaybackAdvanceObserveContext,
    sync_snapshot: crate::sync::video_sync::VideoSyncSnapshot,
) {
    let has_current_video_frame = observe.runtime.video.has_current_frame;
    if sync_snapshot.current_video_pts_us == 0 && !has_current_video_frame {
        return;
    }

    let audio_snapshot = observe.audio_output;
    let decode_status = observe.runtime.decode_supply;
    let state = PlayerState::from_raw(observe.control.state_raw)
        .unwrap_or(crate::api::types::PlayerState::Idle);

    let should_observe = match state {
        PlayerState::Playing => {
            audio_snapshot.started
                && audio_snapshot.audible_frames_total > 0
                && decode_status.has_sufficient_presentation_buffer
                && sync_snapshot.core_sync_error_us == 0
        }
        PlayerState::Ready | PlayerState::Paused => sync_snapshot.core_sync_error_us == 0,
        PlayerState::Idle => false,
    };

    if should_observe {
        player.observe_seek_stable_access();
    }
}

pub(crate) fn execute_playback_plan(plan: &PlaybackAdvancePlan) -> PlaybackAdvanceResult {
    plan.audio_output
        .with_mut(|audio_output| audio_output.ensure_backend_format(plan.audio_format));

    let configured_format = plan
        .audio_output
        .with_ref(crate::audio::core::output_controller::AudioOutputController::configured_format);
    let audio_chunks_submitted =
        !plan.audio_chunks.is_empty() && plan.audio_chunks[0].format() == configured_format;

    plan.audio_output.with_mut(|audio_output| {
        if !audio_chunks_submitted && !plan.audio_chunks.is_empty() {
            audio_output.clear_buffer();
        } else {
            for chunk in &plan.audio_chunks {
                audio_output.submit_chunk(chunk);
            }
        }
    });

    PlaybackAdvanceResult {
        audio_chunks: plan.audio_chunks.clone(),
        audio_chunks_submitted,
    }
}

#[allow(clippy::needless_pass_by_value)]
pub(crate) fn finish_playback_advance(
    player: &mut SemiPlayerHandle,
    plan: PlaybackAdvancePlan,
    result: PlaybackAdvanceResult,
) {
    player.observe_stale_audio_discard(plan.initial_discard);
    if !result.audio_chunks_submitted && !result.audio_chunks.is_empty() {
        player.with_runtime_access(|mut runtime| {
            runtime.restore_audio_chunks_front(result.audio_chunks);
        });
    }
    let playback_time_us = player.playback_position_us_snapshot();
    let post_sync_discard = player
        .with_runtime_access(|mut runtime| runtime.discard_consumed_audio_frames(playback_time_us));
    player.observe_stale_audio_discard(post_sync_discard);
    let sync_snapshot = VideoSyncService::tick(player, playback_time_us);

    let started_before_sync = player.audio_coord_access().started_snapshot();
    let mut should_update_clock_from_device = false;

    if player.control_snapshot().gate_audio_until_video_ready {
        if player.with_runtime_access(|runtime| runtime.has_current_video_frame())
            && sync_snapshot.core_sync_error_us == 0
        {
            player
                .audio_coord_access()
                .sync_output_started_state(plan.state);

            let device_timing = player.audio_coord_access().playback_timing_snapshot();
            if device_timing.is_some() {
                player.audio_coord_access().play_clock();
                player
                    .audio_coord_access()
                    .update_clock_from_device(device_timing);
                let _ = player.release_audio_gate_for_seek_recovery_access();
            }
        }
    } else {
        player
            .audio_coord_access()
            .sync_output_started_state(plan.state);
        should_update_clock_from_device = true;
    }

    let started_after_sync = player.audio_coord_access().started_snapshot();
    if !started_before_sync && started_after_sync {
        player.observe_seek_audio_output_started_access();
    }

    if should_update_clock_from_device {
        let device_timing = player.audio_coord_access().playback_timing_snapshot();
        player
            .audio_coord_access()
            .update_clock_from_device(device_timing);
    }

    let observe = player.playback_advance_observe_context();
    if let Some(current_pts_us) = observe.runtime.video.current_pts_us {
        player.observe_seek_current_video_access(
            current_pts_us,
            sync_snapshot.current_video_effective_end_us,
            observe.playback_position_us,
        );
    }
    if observe.audio_output.started && observe.audio_output.audible_frames_total > 0 {
        player.observe_seek_target_audio_ready_access();
    }
    observe_seek_stable_if_ready(player, observe, sync_snapshot);
}
