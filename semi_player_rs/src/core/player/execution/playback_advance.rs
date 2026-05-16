use crate::api::types::PlayerState;
use crate::audio::core::clock::DevicePlaybackTiming;
use crate::audio::core::output::AudioOutputChunk;
use crate::audio::core::output_controller::SharedAudioOutputController;
use crate::core::player::handle::SemiPlayerHandle;
use crate::core::player::runtime::AudioDiscardSummary;
use crate::core::player::video_sync::VideoSyncService;
const AUDIO_SYNC_BATCH_CHUNK_LIMIT: usize = 4;

pub fn advance_playback(player: &mut SemiPlayerHandle) {
    let plan = plan_playback_advance(player);
    let result = execute_playback_plan(&plan);
    finish_playback_advance(player, plan, result);
}

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
    device_timing: Option<DevicePlaybackTiming>,
}

pub(crate) fn plan_playback_advance(player: &mut SemiPlayerHandle) -> PlaybackAdvancePlan {
    let initial_playback_time_us = player.audio_clock.presentation_time_us();
    let initial_discard = player
        .runtime
        .discard_consumed_audio_frames(initial_playback_time_us);
    let audio_format = player.runtime.current_audio_format();
    let state = player.state();
    let audio_output = player.audio_output.clone();
    let (request_frame_count, max_chunks) = audio_output.with_ref(|audio_output| {
        let snapshot = audio_output.snapshot();
        if snapshot.buffered_frames >= snapshot.target_buffer_frames {
            return (0, 0);
        }

        let request_frame_count = audio_output.next_request_frame_count();
        if request_frame_count == 0 {
            return (0, 0);
        }

        let deficit_frames = snapshot
            .target_buffer_frames
            .saturating_sub(snapshot.buffered_frames);
        let max_chunks = deficit_frames
            .saturating_add(request_frame_count.saturating_sub(1))
            .saturating_div(request_frame_count)
            .clamp(1, AUDIO_SYNC_BATCH_CHUNK_LIMIT);
        (request_frame_count, max_chunks)
    });
    let audio_chunks = player
        .runtime
        .pull_audio_chunks(request_frame_count, max_chunks);

    PlaybackAdvancePlan {
        audio_output,
        state,
        initial_discard,
        audio_chunks,
        audio_format,
    }
}

pub(crate) fn execute_playback_plan(plan: &PlaybackAdvancePlan) -> PlaybackAdvanceResult {
    plan.audio_output.with_mut(|audio_output| {
        audio_output.ensure_backend_format(plan.audio_format);
        audio_output.sync_started_state(plan.state);
    });

    let configured_format = plan
        .audio_output
        .with_ref(|audio_output| audio_output.configured_format());
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

    let device_timing = plan
        .audio_output
        .with_ref(|audio_output| audio_output.playback_timing());

    PlaybackAdvanceResult {
        audio_chunks: plan.audio_chunks.clone(),
        audio_chunks_submitted,
        device_timing,
    }
}

pub(crate) fn finish_playback_advance(
    player: &mut SemiPlayerHandle,
    plan: PlaybackAdvancePlan,
    result: PlaybackAdvanceResult,
) {
    player.observe_stale_audio_discard(plan.initial_discard);
    if !result.audio_chunks_submitted && !result.audio_chunks.is_empty() {
        player
            .runtime
            .restore_audio_chunks_front(result.audio_chunks);
    }
    player.audio_clock.update_from_device(result.device_timing);
    let playback_time_us = player.audio_clock.presentation_time_us();
    let post_sync_discard = player
        .runtime
        .discard_consumed_audio_frames(playback_time_us);
    player.observe_stale_audio_discard(post_sync_discard);
    let _ = VideoSyncService::tick(player, playback_time_us);
}
