use crate::api::types::PlayerState;
use crate::audio::core::output::AudioOutputChunk;
use crate::audio::core::output_controller::SharedAudioOutputController;
use crate::player::access::{PlaybackAdvanceObserveContext, PlaybackAdvancePlanContext};
use crate::player::handle::SemiPlayerHandle;
use crate::player::runtime::AudioDiscardSummary;
use crate::sync::video_sync::VideoSyncService;
use crate::util::debug_trace::append_trace_line;

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

struct PlaybackFinalizeState {
    sync_snapshot: crate::sync::video_sync::VideoSyncSnapshot,
    started_before_sync: bool,
    started_after_sync: bool,
    should_update_clock_from_device: bool,
}

pub(crate) fn plan_playback_advance(player: &SemiPlayerHandle) -> PlaybackAdvancePlan {
    let context = player.playback_advance_plan_context();
    build_playback_advance_plan(context)
}

fn should_update_clock_from_device_for_state(state: PlayerState) -> bool {
    state == PlayerState::Playing
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
    player: &SemiPlayerHandle,
    plan: PlaybackAdvancePlan,
    result: PlaybackAdvanceResult,
) {
    finalize_audio_submission(player, &plan, result);
    let mut state = finalize_sync_domain(player);
    finalize_output_state(player, &plan, &mut state);
    finalize_clock_sync(player, &state);
    finalize_observation(player, state);
}

fn finalize_audio_submission(
    player: &SemiPlayerHandle,
    plan: &PlaybackAdvancePlan,
    result: PlaybackAdvanceResult,
) {
    player.observe_stale_audio_discard(plan.initial_discard);
    if !result.audio_chunks_submitted && !result.audio_chunks.is_empty() {
        player.with_runtime_access(|mut runtime| {
            runtime.restore_audio_chunks_front(result.audio_chunks);
        });
    }
}

fn finalize_sync_domain(player: &SemiPlayerHandle) -> PlaybackFinalizeState {
    let playback_time_us = player.playback_position_us_snapshot();
    let post_sync_discard = player
        .with_runtime_access(|mut runtime| runtime.discard_consumed_audio_frames(playback_time_us));
    player.observe_stale_audio_discard(post_sync_discard);

    let sync_snapshot = VideoSyncService::tick(player, playback_time_us);
    let started_before_sync = player.audio_coord_access().started_snapshot();
    append_trace_line(&format!(
        "playback_advance:post_sync playback={playback_time_us} started_before={} gate_audio={} current_video={} sync_err={} current_pts={}",
        started_before_sync,
        player.control_snapshot().gate_audio_until_video_ready,
        player.with_runtime_access(|runtime| runtime.has_current_video_frame()),
        sync_snapshot.core_sync_error_us,
        sync_snapshot.current_video_pts_us
    ));

    PlaybackFinalizeState {
        sync_snapshot,
        started_before_sync,
        started_after_sync: started_before_sync,
        should_update_clock_from_device: false,
    }
}

fn finalize_output_state(
    player: &SemiPlayerHandle,
    plan: &PlaybackAdvancePlan,
    state: &mut PlaybackFinalizeState,
) {
    if player.control_snapshot().gate_audio_until_video_ready {
        try_release_audio_gate(player, state, plan.state);
    } else if should_update_clock_from_device_for_state(plan.state) {
        player
            .audio_coord_access()
            .sync_output_started_state(plan.state);
        state.should_update_clock_from_device = true;
    } else {
        player
            .audio_coord_access()
            .sync_output_started_state(plan.state);
    }

    state.started_after_sync = player.audio_coord_access().started_snapshot();
    if !state.started_before_sync && state.started_after_sync {
        player.observe_seek_audio_output_started_access();
    }
}

fn try_release_audio_gate(
    player: &SemiPlayerHandle,
    state: &PlaybackFinalizeState,
    player_state: PlayerState,
) {
    if player.with_runtime_access(|runtime| runtime.has_current_video_frame())
        && state.sync_snapshot.core_sync_error_us == 0
    {
        player
            .audio_coord_access()
            .sync_output_started_state(player_state);

        let device_timing = player.audio_coord_access().playback_timing_snapshot();
        append_trace_line(&format!(
            "playback_advance:gate_release device_timing={:?} clock_before={}",
            device_timing,
            player.playback_position_us_snapshot()
        ));
        if let Some(timing) = device_timing {
            player.audio_coord_access().update_clock_from_device(Some(timing));
        }
        let _ = player.release_audio_gate_for_seek_recovery_access();
    }
}

fn finalize_clock_sync(player: &SemiPlayerHandle, state: &PlaybackFinalizeState) {
    if !state.should_update_clock_from_device {
        return;
    }

    let device_timing = player.audio_coord_access().playback_timing_snapshot();
    let clock_before_update_us = player.playback_position_us_snapshot();
    let audio_snapshot = player.audio_output_snapshot();
    let expected_present_us = device_timing.map(|timing| {
        let pending_frames_us = i64::try_from(audio_snapshot.pending_device_frames)
            .unwrap_or(i64::MAX)
            .saturating_mul(1_000_000)
            .saturating_div(i64::from(timing.sample_rate.max(1)));
        timing
            .base_pts_us
            .saturating_add(
                i64::try_from(timing.played_frames)
                    .unwrap_or(i64::MAX)
                    .saturating_mul(1_000_000)
                    .saturating_div(i64::from(timing.sample_rate.max(1))),
            )
            .saturating_add(pending_frames_us)
    });
    append_trace_line(&format!(
        "playback_advance:update_clock begin started_before={} started_after={} device_timing={:?} pending_device_frames={} expected_present_us={:?} clock_before={clock_before_update_us}",
        state.started_before_sync,
        state.started_after_sync,
        device_timing,
        audio_snapshot.pending_device_frames,
        expected_present_us
    ));
    player
        .audio_coord_access()
        .update_clock_from_device(device_timing);
    append_trace_line(&format!(
        "playback_advance:update_clock end clock_after={}",
        player.playback_position_us_snapshot()
    ));
}

fn finalize_observation(player: &SemiPlayerHandle, state: PlaybackFinalizeState) {
    let observe = player.playback_advance_observe_context();
    if let Some(current_pts_us) = observe.runtime.video.current_pts_us {
        player.observe_seek_current_video_access(
            current_pts_us,
            state.sync_snapshot.current_video_effective_end_us,
            observe.playback_position_us,
        );
    }
    if observe.audio_output.started && observe.audio_output.audible_frames_total > 0 {
        player.observe_seek_target_audio_ready_access();
    }
    observe_seek_stable_if_ready(player, observe, state.sync_snapshot);
}

#[cfg(test)]
mod tests {
    use super::should_update_clock_from_device_for_state;
    use crate::api::types::PlayerState;

    #[test]
    fn only_playing_state_follows_device_clock() {
        assert!(should_update_clock_from_device_for_state(PlayerState::Playing));
        assert!(!should_update_clock_from_device_for_state(PlayerState::Paused));
        assert!(!should_update_clock_from_device_for_state(PlayerState::Ready));
        assert!(!should_update_clock_from_device_for_state(PlayerState::Idle));
    }
}
