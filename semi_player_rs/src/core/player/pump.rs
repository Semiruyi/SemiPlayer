use crate::api::error::{ResultCode, SEMI_E_INVALID_STATE, SEMI_OK};
use crate::core::player::execution::{advance_playback, decode_supply};
use crate::core::player::handle::SemiPlayerHandle;
use crate::sync::schedule::{PlayerScheduleService, ScheduledWork};

pub fn pump_player(player: &mut SemiPlayerHandle, max_iterations: u32) -> ResultCode {
    if !player.is_media_loaded() || player.media_session().is_none() {
        return SEMI_E_INVALID_STATE;
    }

    let first_pass = PlayerScheduleService::evaluate(player).scheduled_work();
    service_scheduled_work(player, first_pass);

    let decode_code = service_decode_work_if_needed(player, max_iterations);
    if decode_code != SEMI_OK {
        return decode_code;
    }

    let second_pass = PlayerScheduleService::evaluate(player).scheduled_work();
    service_scheduled_work(player, second_pass);

    SEMI_OK
}

fn service_scheduled_work(player: &mut SemiPlayerHandle, scheduled_work: ScheduledWork) {
    if scheduled_work.should_advance_playback {
        advance_playback(player);
    }
}

fn service_decode_work_if_needed(player: &mut SemiPlayerHandle, max_iterations: u32) -> ResultCode {
    if PlayerScheduleService::evaluate_decode(player).should_decode_now {
        decode_supply(player, max_iterations)
    } else {
        SEMI_OK
    }
}

#[cfg(test)]
mod tests {
    use std::sync::Arc;

    use super::service_scheduled_work;
    use crate::core::player::handle::SemiPlayerHandle;
    use crate::sync::schedule::PlayerScheduleService;
    use crate::render::core::frame::{PixelFormatCategory, VideoFrame, VideoSurface};

    fn frame(pts_us: i64, duration_us: Option<i64>) -> VideoFrame {
        VideoFrame {
            pts_us,
            duration_us,
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
    fn scheduled_pump_step_advances_video_when_playback_is_due() {
        let mut player = SemiPlayerHandle::new();
        player.audio_clock.play();
        player.runtime.push_video_frame(frame(0, Some(33_000)));
        player.runtime.push_video_frame(frame(41_000, Some(41_000)));
        player.video_sync.reset();

        let scheduled_work = PlayerScheduleService::evaluate(&player).scheduled_work();
        assert!(scheduled_work.should_advance_playback);

        service_scheduled_work(&mut player, scheduled_work);

        assert_eq!(
            player
                .runtime
                .current_video_frame()
                .map(|frame| frame.pts_us),
            Some(0)
        );
        assert_eq!(player.runtime.video_queue_len(), 1);
        assert!(!player.video_sync.is_dirty());
    }

    #[test]
    fn scheduled_pump_step_can_skip_playback_when_not_due() {
        let mut player = SemiPlayerHandle::new();
        player.audio_clock.play();
        player.runtime.push_video_frame(frame(0, Some(33_000)));
        player.runtime.push_video_frame(frame(41_000, Some(41_000)));
        let _ = player
            .runtime
            .select_video_frame(&player.video_scheduler, 0, |_| {});
        let _ = crate::sync::video_sync::VideoSyncService::tick(&mut player, 0);

        let scheduled_work = PlayerScheduleService::evaluate(&player).scheduled_work();
        assert!(!scheduled_work.should_advance_playback);

        service_scheduled_work(&mut player, scheduled_work);

        assert_eq!(
            player
                .runtime
                .current_video_frame()
                .map(|frame| frame.pts_us),
            Some(0)
        );
        assert_eq!(player.runtime.video_queue_len(), 1);
    }
}
