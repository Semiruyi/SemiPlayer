use crate::core::player::handle::SemiPlayerHandle;
use crate::core::player::video_sync::VideoSyncService;

pub fn advance_playback(player: &mut SemiPlayerHandle) {
    let initial_playback_time_us = player.audio_clock.presentation_time_us();
    let initial_discard = player
        .runtime
        .discard_consumed_audio_frames(initial_playback_time_us);
    player.observe_stale_audio_discard(initial_discard);
    sync_audio_output(player);
    let playback_time_us = player.audio_clock.presentation_time_us();
    let post_sync_discard = player
        .runtime
        .discard_consumed_audio_frames(playback_time_us);
    player.observe_stale_audio_discard(post_sync_discard);
    let _ = VideoSyncService::tick(player, playback_time_us);
}

fn sync_audio_output(player: &mut SemiPlayerHandle) {
    let audio_format = player.runtime.current_audio_format();
    let state = player.state();

    player.audio_output.with_mut(|audio_output| {
        audio_output.ensure_backend_format(audio_format);
        audio_output.sync_started_state(state);
    });

    let configured_format = player
        .audio_output
        .with_ref(|audio_output| audio_output.configured_format());
    loop {
        let request_frame_count = player
            .audio_output
            .with_ref(|audio_output| {
                if audio_output.needs_more_frames() {
                    Some(audio_output.next_request_frame_count())
                } else {
                    None
                }
            });
        let Some(request_frame_count) = request_frame_count else {
            break;
        };

        let Some(chunk) = player.runtime.pull_audio_chunk(request_frame_count) else {
            break;
        };

        let format_mismatch = chunk.format() != configured_format;
        player.audio_output.with_mut(|audio_output| {
            if format_mismatch {
                audio_output.clear_buffer();
            } else {
                audio_output.submit_chunk(&chunk);
            }
        });
        if format_mismatch {
            break;
        }
    }

    let device_timing = player
        .audio_output
        .with_ref(|audio_output| audio_output.playback_timing());
    player.audio_clock.update_from_device(device_timing);
}
