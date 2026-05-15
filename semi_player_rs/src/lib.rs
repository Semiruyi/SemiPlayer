mod api;
mod audio;
mod core;
mod platform;
mod render;
mod subtitle;
mod util;

use std::ffi::{c_char, c_double, c_int, CStr, CString};
use std::ptr;
use crate::api::error::{
    ResultCode, SEMI_E_INVALID_ARG, SEMI_E_INVALID_STATE, SEMI_E_MEDIA_OPEN_FAILED,
    SEMI_E_MEDIA_PROBE_FAILED, SEMI_OK,
};
use crate::api::types::PlayerState;
use crate::core::media::{probe_media, MediaProbeError};
use crate::core::player::handle::SemiPlayerHandle;
use crate::util::time::{ms_to_us, us_to_ms};

fn with_player_mut<T>(
    player: *mut SemiPlayerHandle,
    f: impl FnOnce(&mut SemiPlayerHandle) -> T,
) -> Result<T, ResultCode> {
    if player.is_null() {
        return Err(SEMI_E_INVALID_ARG);
    }

    let player = unsafe { &mut *player };
    Ok(f(player))
}

fn cstr_to_string(input: *const c_char) -> Result<String, c_int> {
    if input.is_null() {
        return Err(SEMI_E_INVALID_ARG);
    }

    let c_str = unsafe { CStr::from_ptr(input) };
    Ok(c_str.to_string_lossy().into_owned())
}

#[no_mangle]
pub extern "C" fn semi_free_string(s: *mut c_char) {
    if !s.is_null() {
        unsafe { drop(CString::from_raw(s)) };
    }
}

#[no_mangle]
pub extern "C" fn semi_player_create(out_player: *mut *mut SemiPlayerHandle) -> c_int {
    if out_player.is_null() {
        return SEMI_E_INVALID_ARG;
    }

    let player = Box::new(SemiPlayerHandle::new());
    unsafe {
        *out_player = Box::into_raw(player);
    }
    SEMI_OK
}

#[no_mangle]
pub extern "C" fn semi_player_destroy(player: *mut SemiPlayerHandle) {
    if !player.is_null() {
        unsafe { drop(Box::from_raw(player)) };
    }
}

#[no_mangle]
pub extern "C" fn semi_player_open(
    player: *mut SemiPlayerHandle,
    path_utf8: *const c_char,
) -> c_int {
    let path = match cstr_to_string(path_utf8) {
        Ok(path) if !path.trim().is_empty() => path,
        Ok(_) => return SEMI_E_INVALID_ARG,
        Err(code) => return code,
    };

    let media_info = match probe_media(&path) {
        Ok(media_info) => media_info,
        Err(MediaProbeError::OpenInput(_)) => return SEMI_E_MEDIA_OPEN_FAILED,
        Err(MediaProbeError::FfmpegInit(_)) | Err(MediaProbeError::Decoder(_)) => {
            return SEMI_E_MEDIA_PROBE_FAILED;
        }
    };

    match with_player_mut(player, |player| {
        player.media_path = Some(path);
        player.duration_us = media_info.duration_us.unwrap_or(0);
        player.media_info = Some(media_info);
        player.reset_runtime_state();
        player.set_state(PlayerState::Ready);
    }) {
        Ok(_) => SEMI_OK,
        Err(code) => code,
    }
}

#[no_mangle]
pub extern "C" fn semi_player_play(player: *mut SemiPlayerHandle) -> c_int {
    match with_player_mut(player, |player| {
        if !player.is_media_loaded() {
            return SEMI_E_INVALID_STATE;
        }

        player.audio_clock.play();
        player.set_state(PlayerState::Playing);
        SEMI_OK
    }) {
        Ok(code) => code,
        Err(code) => code,
    }
}

#[no_mangle]
pub extern "C" fn semi_player_pause(player: *mut SemiPlayerHandle) -> c_int {
    match with_player_mut(player, |player| {
        if !player.is_media_loaded() {
            return SEMI_E_INVALID_STATE;
        }

        player.audio_clock.pause();
        player.set_state(PlayerState::Paused);
        SEMI_OK
    }) {
        Ok(code) => code,
        Err(code) => code,
    }
}

#[no_mangle]
pub extern "C" fn semi_player_seek(
    player: *mut SemiPlayerHandle,
    position_ms: i64,
    _exact: c_int,
) -> c_int {
    match with_player_mut(player, |player| {
        if !player.is_media_loaded() {
            return SEMI_E_INVALID_STATE;
        }
        if position_ms < 0 {
            return SEMI_E_INVALID_ARG;
        }

        player.audio_clock.seek(ms_to_us(position_ms));
        player.video_scheduler = Default::default();
        SEMI_OK
    }) {
        Ok(code) => code,
        Err(code) => code,
    }
}

#[no_mangle]
pub extern "C" fn semi_player_reset(player: *mut SemiPlayerHandle) -> c_int {
    match with_player_mut(player, |player| {
        player.clear_media();
        player.set_state(PlayerState::Idle);
        SEMI_OK
    }) {
        Ok(code) => code,
        Err(code) => code,
    }
}

#[no_mangle]
pub extern "C" fn semi_player_set_speed(player: *mut SemiPlayerHandle, speed: c_double) -> c_int {
    match with_player_mut(player, |player| {
        if !player.is_media_loaded() {
            return SEMI_E_INVALID_STATE;
        }
        if !speed.is_finite() || speed <= 0.0 {
            return SEMI_E_INVALID_ARG;
        }

        player.speed = speed;
        player.audio_clock.set_speed(speed);
        SEMI_OK
    }) {
        Ok(code) => code,
        Err(code) => code,
    }
}

#[no_mangle]
pub extern "C" fn semi_player_set_video_presentation_bias_ms(
    player: *mut SemiPlayerHandle,
    bias_ms: i32,
) -> c_int {
    match with_player_mut(player, |player| {
        player.video_presentation_bias_us = ms_to_us(i64::from(bias_ms));
        SEMI_OK
    }) {
        Ok(code) => code,
        Err(code) => code,
    }
}

#[no_mangle]
pub extern "C" fn semi_player_set_subtitle_visible(
    player: *mut SemiPlayerHandle,
    visible: c_int,
) -> c_int {
    match with_player_mut(player, |player| {
        if !player.is_media_loaded() {
            return SEMI_E_INVALID_STATE;
        }

        player.subtitles_visible = visible != 0;
        SEMI_OK
    }) {
        Ok(code) => code,
        Err(code) => code,
    }
}

#[no_mangle]
pub extern "C" fn semi_player_get_state(
    player: *mut SemiPlayerHandle,
    out_state: *mut u32,
) -> c_int {
    if out_state.is_null() {
        return SEMI_E_INVALID_ARG;
    }

    match with_player_mut(player, |player| {
        unsafe {
            *out_state = player.state().as_raw();
        }
    }) {
        Ok(_) => SEMI_OK,
        Err(code) => code,
    }
}

#[no_mangle]
pub extern "C" fn semi_player_get_position_ms(
    player: *mut SemiPlayerHandle,
    out_position_ms: *mut i64,
) -> c_int {
    if out_position_ms.is_null() {
        return SEMI_E_INVALID_ARG;
    }

    match with_player_mut(player, |player| {
        unsafe {
            *out_position_ms = us_to_ms(player.audio_clock.presentation_time_us());
        }
    }) {
        Ok(_) => SEMI_OK,
        Err(code) => code,
    }
}

#[no_mangle]
pub extern "C" fn semi_player_get_duration_ms(
    player: *mut SemiPlayerHandle,
    out_duration_ms: *mut i64,
) -> c_int {
    if out_duration_ms.is_null() {
        return SEMI_E_INVALID_ARG;
    }

    match with_player_mut(player, |player| {
        unsafe {
            *out_duration_ms = us_to_ms(player.duration_us);
        }
    }) {
        Ok(_) => SEMI_OK,
        Err(code) => code,
    }
}

#[no_mangle]
pub extern "C" fn semi_ffmpeg_version_string() -> *mut c_char {
    let version = ffmpeg_next::util::version();
    match CString::new(format!("FFmpeg version: {}", version)) {
        Ok(value) => value.into_raw(),
        Err(_) => ptr::null_mut(),
    }
}
