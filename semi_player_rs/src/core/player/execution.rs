use crate::api::error::{ResultCode, SEMI_E_INVALID_STATE, SEMI_OK};
use crate::core::media::DecodedOutput;
use crate::core::player::handle::SemiPlayerHandle;
use crate::core::player::schedule::ScheduledWork;
use crate::core::player::video_sync::VideoSyncService;

const DEFAULT_PUMP_ITERATIONS: u32 = 256;

pub fn execute_scheduled_work(
    player: &mut SemiPlayerHandle,
    scheduled_work: ScheduledWork,
    decode_iterations: u32,
) -> ResultCode {
    match scheduled_work {
        ScheduledWork::AdvanceAndDecode { .. } => {
            execute_playback_cycle(player, true, decode_iterations)
        }
        ScheduledWork::AdvancePlayback { .. } => {
            advance_playback(player);
            SEMI_OK
        }
        ScheduledWork::DecodeSupply => decode_supply(player, decode_iterations),
        ScheduledWork::WaitFor { .. } => SEMI_OK,
    }
}

pub fn execute_playback_cycle(
    player: &mut SemiPlayerHandle,
    should_decode: bool,
    decode_iterations: u32,
) -> ResultCode {
    advance_playback(player);

    if should_decode {
        let code = decode_supply(player, decode_iterations);
        if code != SEMI_OK {
            return code;
        }
    }

    advance_playback(player);
    SEMI_OK
}

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

pub fn decode_supply(player: &mut SemiPlayerHandle, max_iterations: u32) -> ResultCode {
    let iterations = if max_iterations == 0 {
        DEFAULT_PUMP_ITERATIONS
    } else {
        max_iterations
    };

    for _ in 0..iterations {
        let output = {
            let Some(opened_media) = player.opened_media.as_mut() else {
                return SEMI_E_INVALID_STATE;
            };

            match opened_media.next_decoded_output() {
                Ok(Some(output)) => output,
                Ok(None) => break,
                Err(_) => return SEMI_E_INVALID_STATE,
            }
        };

        match output {
            DecodedOutput::Video(frame) => {
                player.runtime.push_video_frame(frame);
                VideoSyncService::mark_dirty(player);
            }
            DecodedOutput::Audio(frame) => {
                player.runtime.push_audio_frame(frame);
            }
            DecodedOutput::EndOfStream => {
                player.runtime.mark_end_of_stream();
                break;
            }
        }

        if !player.runtime.decode_supply_status().needs_decode_supply {
            break;
        }
    }

    SEMI_OK
}

fn sync_audio_output(player: &mut SemiPlayerHandle) {
    let audio_format = player.runtime.current_audio_format();
    let state = player.state();

    player.audio_output.ensure_backend_format(audio_format);
    player.audio_output.sync_started_state(state);

    let configured_format = player.audio_output.configured_format();
    while player.audio_output.needs_more_frames() {
        let Some(chunk) = player
            .runtime
            .pull_audio_chunk(player.audio_output.next_request_frame_count())
        else {
            break;
        };

        if chunk.format() != configured_format {
            player.audio_output.clear_buffer();
            break;
        }

        player.audio_output.submit_chunk(&chunk);
    }

    let device_timing = player.audio_output.playback_timing();
    player.audio_clock.update_from_device(device_timing);
}

#[cfg(test)]
mod tests {
    use crate::audio::core::frame::{AudioFrame, AudioSampleFormatCategory};
    use crate::core::player::handle::SemiPlayerHandle;
    use crate::render::core::frame::{PixelFormatCategory, VideoFrame};

    fn audio_frame(pts_us: i64) -> AudioFrame {
        AudioFrame {
            pts_us,
            duration_us: Some(10_000),
            sample_rate: 48_000,
            channels: 2,
            sample_count: 480,
            sample_format: AudioSampleFormatCategory::F32,
            is_planar: false,
            data: vec![0.0; 480 * 2],
        }
    }

    fn video_frame(pts_us: i64) -> VideoFrame {
        VideoFrame {
            pts_us,
            duration_us: Some(33_000),
            width: 1920,
            height: 1080,
            pixel_format: PixelFormatCategory::Bgra8,
            stride: 1920 * 4,
            data: vec![0; 16],
            is_key_frame: false,
        }
    }

    #[test]
    fn queued_video_frames_count_toward_startup_buffer_target() {
        let mut player = SemiPlayerHandle::new();

        for index in 0..8 {
            player.runtime.push_audio_frame(audio_frame(index * 10_000));
        }

        player.runtime.push_video_frame(video_frame(0));
        player.runtime.push_video_frame(video_frame(33_000));
        player.runtime.push_video_frame(video_frame(66_000));

        assert!(player.runtime.decode_supply_status().has_sufficient_buffer);
    }
}
