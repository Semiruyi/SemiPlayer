use std::ffi::c_double;

use crate::api::error::{
    ResultCode, SEMI_E_INVALID_ARG, SEMI_E_INVALID_STATE, SEMI_E_SEEK_FAILED, SEMI_OK,
};
use crate::api::types::PlayerState;
use crate::audio::core::output_controller::AudioOutputController;
use crate::decode::session::MediaSession;
use crate::player::handle::SemiPlayerHandle;
use crate::render::core::pipeline::PresentationTargetProfile;
use crate::sync::video_scheduler::VideoScheduler;
use crate::sync::video_sync::VideoSyncService;
use crate::util::time::{ms_to_us, MediaTimeUs};

pub fn load_media_session(player: &mut SemiPlayerHandle, media_session: MediaSession) {
    player.bump_media_generation();
    player.install_media_session(media_session);
    player.reset_runtime_state();
    VideoSyncService::mark_dirty(player);
    player.set_state(PlayerState::Ready);
    player.notify_workers();
}

pub fn play(player: &mut SemiPlayerHandle) -> ResultCode {
    if !player.is_media_loaded() {
        return SEMI_E_INVALID_STATE;
    }

    player.audio_clock.play();
    player.set_state(PlayerState::Playing);
    player
        .audio_output
        .with_mut(|audio_output| audio_output.sync_started_state(player.state()));
    VideoSyncService::mark_dirty(player);
    player.notify_workers();
    SEMI_OK
}

pub fn pause(player: &mut SemiPlayerHandle) -> ResultCode {
    if !player.is_media_loaded() {
        return SEMI_E_INVALID_STATE;
    }

    player.audio_clock.pause();
    player.set_state(PlayerState::Paused);
    player
        .audio_output
        .with_mut(|audio_output| audio_output.sync_started_state(player.state()));
    VideoSyncService::mark_dirty(player);
    player.notify_workers();
    SEMI_OK
}

pub fn seek(player: &mut SemiPlayerHandle, position_ms: i64) -> ResultCode {
    let target_us = ms_to_us(position_ms.max(0));
    player.observe_seek_requested(target_us);
    player.observe_seek_lock_acquired();
    if !player.is_media_loaded() {
        player.observe_seek_aborted();
        return SEMI_E_INVALID_STATE;
    }
    if position_ms < 0 {
        player.observe_seek_aborted();
        return SEMI_E_INVALID_ARG;
    }

    seek_to_keyframe(player, target_us)
}

pub fn seek_prev_keyframe(player: &mut SemiPlayerHandle, min_offset_ms: i32) -> ResultCode {
    let current_pts_us = player
        .runtime
        .current_video_frame()
        .map(|frame| frame.pts_us)
        .unwrap_or(0);
    let offset_us = ms_to_us(i64::from(min_offset_ms.max(0)));
    let probe_target = current_pts_us.saturating_sub(offset_us).max(0);

    let keyframe_pts = player
        .probe_prev_keyframe_pts(probe_target)
        .unwrap_or(current_pts_us);

    seek_to_keyframe(player, keyframe_pts)
}

pub fn seek_next_keyframe(player: &mut SemiPlayerHandle, min_offset_ms: i32) -> ResultCode {
    let current_pts_us = player
        .runtime
        .current_video_frame()
        .map(|frame| frame.pts_us)
        .unwrap_or(0);
    let offset_us = ms_to_us(i64::from(min_offset_ms.max(0)));
    let probe_target = current_pts_us.saturating_add(offset_us);

    let keyframe_pts = player
        .probe_next_keyframe_pts(probe_target)
        .unwrap_or(current_pts_us);

    seek_to_keyframe(player, keyframe_pts)
}

pub fn reset(player: &mut SemiPlayerHandle) -> ResultCode {
    player.bump_media_generation();
    player.clear_media();
    player.set_state(PlayerState::Idle);
    player.notify_workers();
    SEMI_OK
}

pub fn set_speed(player: &mut SemiPlayerHandle, speed: c_double) -> ResultCode {
    if !player.is_media_loaded() {
        return SEMI_E_INVALID_STATE;
    }
    if !speed.is_finite() || speed <= 0.0 {
        return SEMI_E_INVALID_ARG;
    }

    player.set_speed_value(speed);
    player.audio_clock.set_speed(speed);
    VideoSyncService::mark_dirty(player);
    player.notify_workers();
    SEMI_OK
}

pub fn set_video_presentation_bias(player: &mut SemiPlayerHandle, bias_ms: i32) -> ResultCode {
    player.set_host_presentation_offset_us(ms_to_us(i64::from(bias_ms)));
    VideoSyncService::mark_dirty(player);
    player.notify_workers();
    SEMI_OK
}

pub fn set_subtitle_visible(player: &mut SemiPlayerHandle, visible: bool) -> ResultCode {
    if !player.is_media_loaded() {
        return SEMI_E_INVALID_STATE;
    }

    player.set_subtitles_visible(visible);
    SEMI_OK
}

pub fn set_video_presentation_profile(
    player: &mut SemiPlayerHandle,
    profile: PresentationTargetProfile,
) -> ResultCode {
    if !player.is_media_loaded() {
        return SEMI_E_INVALID_STATE;
    }

    player.set_video_presentation_profile(profile);
    VideoSyncService::mark_dirty(player);
    player.notify_workers();
    SEMI_OK
}

fn seek_to_keyframe(player: &mut SemiPlayerHandle, keyframe_pts: MediaTimeUs) -> ResultCode {
    player.observe_seek_ffmpeg_seek_started();
    let resolved_pts = match player.seek_media(keyframe_pts) {
        Ok(pts) => pts,
        Err(_) => {
            player.observe_seek_aborted();
            return SEMI_E_SEEK_FAILED;
        }
    };
    player.observe_seek_ffmpeg_seek_finished();

    player.bump_media_generation();
    player.runtime.clear();
    player
        .audio_output
        .with_mut(AudioOutputController::clear_buffer);
    player.audio_clock.seek(resolved_pts);
    if player.state() == PlayerState::Playing {
        player.audio_clock.pause();
    }
    player.video_scheduler = VideoScheduler;
    player.video_sync.reset();
    player.begin_seek_recovery(resolved_pts);
    player.observe_seek_reset_finished();
    player.notify_workers();
    player.observe_seek_api_completed();
    SEMI_OK
}
