use crate::api::error::{ResultCode, SEMI_E_INVALID_STATE, SEMI_OK};
use crate::core::media::DecodedOutput;
use crate::core::player::handle::SemiPlayerHandle;
use crate::util::time::add_media_time_us;

const DEFAULT_PUMP_ITERATIONS: u32 = 256;
const TARGET_AUDIO_QUEUE_LEN: usize = 8;

pub fn pump_player(player: &mut SemiPlayerHandle, max_iterations: u32) -> ResultCode {
    if !player.is_media_loaded() {
        return SEMI_E_INVALID_STATE;
    }

    if player.opened_media.is_none() {
        return SEMI_E_INVALID_STATE;
    }

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
            }
            DecodedOutput::Audio(frame) => {
                player.runtime.push_audio_frame(frame);
            }
            DecodedOutput::EndOfStream => {
                player.runtime.mark_end_of_stream();
                break;
            }
        }

        if player.runtime.audio_queue_len() >= TARGET_AUDIO_QUEUE_LEN {
            select_video_frame(player);

            if player.runtime.has_current_video_frame() {
                break;
            }
        }
    }

    select_video_frame(player);
    SEMI_OK
}

fn select_video_frame(player: &mut SemiPlayerHandle) {
    let target_video_time_us = add_media_time_us(
        player.audio_clock.presentation_time_us(),
        player.video_presentation_bias_us,
    );
    let _ = player
        .runtime
        .select_video_frame(&player.video_scheduler, target_video_time_us);
}
