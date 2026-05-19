use std::ffi::c_double;

use crate::api::error::{ResultCode, SEMI_E_INVALID_ARG, SEMI_E_INVALID_STATE, SEMI_OK};
use crate::api::types::PlayerState;
use crate::decode::session::MediaSession;
use crate::player::handle::SemiPlayerHandle;
use crate::render::core::pipeline::PresentationTargetProfile;
use crate::util::time::{ms_to_us, MediaTimeUs};

fn reset_playback_domains_for_new_timeline(player: &mut SemiPlayerHandle) {
    player.with_runtime_access(|mut runtime| {
        runtime.clear_runtime();
        runtime.reset_video_scheduler();
        runtime.reset_video_sync();
    });
    player.audio_coord_access().reset_clock();
    player.audio_coord_access().stop_output();
}

pub fn load_media_session(player: &mut SemiPlayerHandle, media_session: MediaSession) {
    player.bump_media_generation();
    player.install_media_session(media_session);
    reset_playback_domains_for_new_timeline(player);
    player.control_access().set_state(PlayerState::Ready);
    player.with_runtime_access(|mut runtime| {
        runtime.mark_video_sync_dirty();
    });
    player.notify_workers();
}

fn mark_video_sync_dirty(player: &mut SemiPlayerHandle) {
    player.with_runtime_access(|mut runtime| {
        runtime.mark_video_sync_dirty();
    });
}

pub fn play(player: &mut SemiPlayerHandle) -> ResultCode {
    let control = player.control_access();
    let audio = player.audio_coord_access();
    if !control.is_media_loaded() {
        return SEMI_E_INVALID_STATE;
    }

    audio.play_clock();
    control.set_state(PlayerState::Playing);
    audio.sync_output_started_state(control.state());
    mark_video_sync_dirty(player);
    player.notify_workers();
    SEMI_OK
}

pub fn pause(player: &mut SemiPlayerHandle) -> ResultCode {
    let control = player.control_access();
    let audio = player.audio_coord_access();
    if !control.is_media_loaded() {
        return SEMI_E_INVALID_STATE;
    }

    audio.pause_clock();
    control.set_state(PlayerState::Paused);
    audio.sync_output_started_state(control.state());
    mark_video_sync_dirty(player);
    player.notify_workers();
    SEMI_OK
}

pub fn prepare_seek(
    player: &mut SemiPlayerHandle,
    position_ms: i64,
) -> Result<MediaTimeUs, ResultCode> {
    let target_us = ms_to_us(position_ms.max(0));
    let seek = player.seek_prepare_context();
    player.observe_seek_requested_access(target_us);
    player.observe_seek_lock_acquired_access();
    if !seek.media_loaded {
        player.observe_seek_aborted_access();
        return Err(SEMI_E_INVALID_STATE);
    }
    if position_ms < 0 {
        player.observe_seek_aborted_access();
        return Err(SEMI_E_INVALID_ARG);
    }

    Ok(target_us)
}

pub fn commit_seek(player: &mut SemiPlayerHandle, resolved_pts: MediaTimeUs) -> ResultCode {
    let seek = player.seek_commit_context();
    player.bump_media_generation();
    player.with_runtime_access(|mut runtime| {
        runtime.clear_runtime();
        runtime.reset_video_scheduler();
        runtime.reset_video_sync();
    });
    player.audio_coord_access().clear_output_buffer();
    player.audio_coord_access().seek_clock(resolved_pts);
    if seek.was_playing {
        player.audio_coord_access().pause_clock();
    }
    player.control_access().begin_seek_recovery(resolved_pts);
    player.observe_seek_reset_finished_access();
    player.notify_workers();
    player.observe_seek_api_completed_access();
    SEMI_OK
}

pub fn prepare_seek_prev_keyframe(
    player: &mut SemiPlayerHandle,
    min_offset_ms: i32,
) -> Result<MediaTimeUs, ResultCode> {
    let seek = player.seek_prepare_context();
    let current_pts_us = seek.current_video_pts_us.unwrap_or(0);
    let offset_us = ms_to_us(i64::from(min_offset_ms.max(0)));
    let probe_target = current_pts_us.saturating_sub(offset_us).max(0);

    let keyframe_pts = player
        .probe_prev_keyframe_pts_snapshot(probe_target)
        .unwrap_or(current_pts_us);

    Ok(keyframe_pts)
}

pub fn prepare_seek_next_keyframe(
    player: &mut SemiPlayerHandle,
    min_offset_ms: i32,
) -> Result<MediaTimeUs, ResultCode> {
    let seek = player.seek_prepare_context();
    let current_pts_us = seek.current_video_pts_us.unwrap_or(0);
    let offset_us = ms_to_us(i64::from(min_offset_ms.max(0)));
    let probe_target = current_pts_us.saturating_add(offset_us);

    let keyframe_pts = player
        .probe_next_keyframe_pts_snapshot(probe_target)
        .unwrap_or(current_pts_us);

    Ok(keyframe_pts)
}

pub fn reset(player: &mut SemiPlayerHandle) -> ResultCode {
    player.bump_media_generation();
    player.clear_media_session();
    reset_playback_domains_for_new_timeline(player);
    player.control_access().set_state(PlayerState::Idle);
    player.notify_workers();
    SEMI_OK
}

pub fn set_speed(player: &mut SemiPlayerHandle, speed: c_double) -> ResultCode {
    let control = player.control_access();
    let audio = player.audio_coord_access();
    if !control.is_media_loaded() {
        return SEMI_E_INVALID_STATE;
    }
    if !speed.is_finite() || speed <= 0.0 {
        return SEMI_E_INVALID_ARG;
    }

    control.set_speed_value(speed);
    audio.set_clock_speed(speed);
    mark_video_sync_dirty(player);
    player.notify_workers();
    SEMI_OK
}

pub fn set_video_presentation_bias(player: &mut SemiPlayerHandle, bias_ms: i32) -> ResultCode {
    player
        .control_access()
        .set_host_presentation_offset_us(ms_to_us(i64::from(bias_ms)));
    mark_video_sync_dirty(player);
    player.notify_workers();
    SEMI_OK
}

pub fn set_subtitle_visible(player: &mut SemiPlayerHandle, visible: bool) -> ResultCode {
    let control = player.control_access();
    if !control.is_media_loaded() {
        return SEMI_E_INVALID_STATE;
    }

    control.set_subtitles_visible(visible);
    SEMI_OK
}

pub fn set_video_presentation_profile(
    player: &mut SemiPlayerHandle,
    profile: PresentationTargetProfile,
) -> ResultCode {
    let control = player.control_access();
    if !control.is_media_loaded() {
        return SEMI_E_INVALID_STATE;
    }

    control.set_video_presentation_profile(profile);
    mark_video_sync_dirty(player);
    player.notify_workers();
    SEMI_OK
}
