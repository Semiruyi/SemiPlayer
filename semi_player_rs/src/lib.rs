mod api;
mod audio;
mod core;
mod platform;
mod render;
mod subtitle;
mod sync;
mod util;

use crate::api::error::{
    ResultCode, SEMI_E_DECODER_OPEN_FAILED, SEMI_E_INVALID_ARG, SEMI_E_INVALID_STATE,
    SEMI_E_MEDIA_OPEN_FAILED, SEMI_E_MEDIA_PROBE_FAILED, SEMI_OK,
};
use crate::api::types::{
    SemiAudioOutputSnapshot, SemiDecodedOutput, SemiMediaInfo, SemiPlaybackSnapshot,
    SemiVideoFrameInfo, SemiVideoPresentationProfile, SemiVideoSurfaceDesc,
};
use crate::core::media::decode::{
    DecodedOutput,
};
use crate::core::media::demux::MediaProbeError;
use crate::core::media::session::open_media_with_hw_device_ctx;
use crate::core::media::MediaOpenError;
use crate::core::player::handle::SemiPlayerHandle;
use crate::core::player::orchestrator;
use crate::core::player::pump::pump_player;
use crate::core::player::view::{
    build_audio_output_snapshot, build_decoded_output_view, build_media_info_view,
    build_playback_snapshot, build_video_frame_info, build_video_surface_desc,
};
use crate::util::time::us_to_ms;
use std::ffi::{c_char, c_double, c_int, CStr, CString};
use std::ptr;

fn with_player_locked<T>(
    player: *mut SemiPlayerHandle,
    f: impl FnOnce(&mut SemiPlayerHandle) -> T,
) -> Result<T, ResultCode> {
    if player.is_null() {
        return Err(SEMI_E_INVALID_ARG);
    }

    Ok(unsafe { SemiPlayerHandle::with_locked_ptr(player, f) })
}

fn with_playback_coordinated_player_locked<T>(
    player: *mut SemiPlayerHandle,
    f: impl FnOnce(&mut SemiPlayerHandle) -> T,
) -> Result<T, ResultCode> {
    if player.is_null() {
        return Err(SEMI_E_INVALID_ARG);
    }

    let phase_lock = unsafe { (&*player).playback_phase_lock() };
    let _phase_guard = phase_lock.lock().unwrap();
    Ok(unsafe { SemiPlayerHandle::with_locked_ptr(player, f) })
}

fn cstr_to_string(input: *const c_char) -> Result<String, c_int> {
    if input.is_null() {
        return Err(SEMI_E_INVALID_ARG);
    }

    let c_str = unsafe { CStr::from_ptr(input) };
    Ok(c_str.to_string_lossy().into_owned())
}

#[no_mangle]
/// # Safety
///
/// `s` must be null or a pointer previously returned by this library from
/// `CString::into_raw`, and it must not be freed more than once.
pub unsafe extern "C" fn semi_free_string(s: *mut c_char) {
    if !s.is_null() {
        unsafe { drop(CString::from_raw(s)) };
    }
}

#[no_mangle]
/// # Safety
///
/// `out_player` must be a valid, writable pointer to receive the created player handle.
pub unsafe extern "C" fn semi_player_create(out_player: *mut *mut SemiPlayerHandle) -> c_int {
    if out_player.is_null() {
        return SEMI_E_INVALID_ARG;
    }

    let player_ptr = Box::into_raw(Box::new(SemiPlayerHandle::new()));
    unsafe {
        (*player_ptr).start_workers(player_ptr);
        *out_player = player_ptr;
    }
    SEMI_OK
}

#[no_mangle]
/// # Safety
///
/// `player` must be null or a valid handle previously returned by `semi_player_create`.
/// It must not be used again after destruction.
pub unsafe extern "C" fn semi_player_destroy(player: *mut SemiPlayerHandle) {
    if !player.is_null() {
        unsafe {
            (*player).stop_workers();
            drop(Box::from_raw(player));
        };
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

    let hw_device_ctx = with_player_locked(player, |player| {
        player
            .gpu_device
            .as_ref()
            .and_then(|device| device.create_ffmpeg_hw_device_ctx().ok())
    })
    .unwrap_or(None);

    let opened_media = match open_media_with_hw_device_ctx(&path, hw_device_ctx) {
        Ok(opened_media) => opened_media,
        Err(MediaOpenError::Probe(MediaProbeError::OpenInput(_))) => {
            return SEMI_E_MEDIA_OPEN_FAILED
        }
        Err(
            MediaOpenError::Probe(MediaProbeError::FfmpegInit(_) | MediaProbeError::Decoder(_))
            | MediaOpenError::Seek(_),
        ) => {
            return SEMI_E_MEDIA_PROBE_FAILED;
        }
        Err(
            MediaOpenError::VideoDecoder(_)
            | MediaOpenError::AudioDecoder(_)
            | MediaOpenError::ReadPacket(_)
            | MediaOpenError::SendPacket(_)
            | MediaOpenError::ReceiveFrame(_)
            | MediaOpenError::ScaleFrame(_)
            | MediaOpenError::ResampleFrame(_),
        ) => {
            return SEMI_E_DECODER_OPEN_FAILED;
        }
    };

    match with_playback_coordinated_player_locked(player, |player| {
        orchestrator::load_media_session(player, opened_media);
    }) {
        Ok(()) => SEMI_OK,
        Err(code) => code,
    }
}

#[no_mangle]
pub extern "C" fn semi_player_play(player: *mut SemiPlayerHandle) -> c_int {
    with_playback_coordinated_player_locked(player, orchestrator::play).unwrap_or_else(|code| code)
}

#[no_mangle]
pub extern "C" fn semi_player_pause(player: *mut SemiPlayerHandle) -> c_int {
    with_playback_coordinated_player_locked(player, orchestrator::pause).unwrap_or_else(|code| code)
}

#[no_mangle]
/// # Safety
///
/// `player` must be a valid handle previously returned by `semi_player_create`.
pub unsafe extern "C" fn semi_player_seek(
    player: *mut SemiPlayerHandle,
    position_ms: i64,
    _exact: c_int,
) -> c_int {
    with_playback_coordinated_player_locked(player, |player| orchestrator::seek(player, position_ms))
        .unwrap_or_else(|code| code)
}

#[no_mangle]
/// # Safety
///
/// `player` must be a valid handle.
pub unsafe extern "C" fn semi_player_seek_prev_keyframe(
    player: *mut SemiPlayerHandle,
    min_offset_ms: c_int,
) -> c_int {
    with_playback_coordinated_player_locked(player, |player| {
        orchestrator::seek_prev_keyframe(player, min_offset_ms)
    })
    .unwrap_or_else(|code| code)
}

#[no_mangle]
/// # Safety
///
/// `player` must be a valid handle.
pub unsafe extern "C" fn semi_player_seek_next_keyframe(
    player: *mut SemiPlayerHandle,
    min_offset_ms: c_int,
) -> c_int {
    with_playback_coordinated_player_locked(player, |player| {
        orchestrator::seek_next_keyframe(player, min_offset_ms)
    })
    .unwrap_or_else(|code| code)
}

#[no_mangle]
pub extern "C" fn semi_player_reset(player: *mut SemiPlayerHandle) -> c_int {
    with_playback_coordinated_player_locked(player, orchestrator::reset).unwrap_or_else(|code| code)
}

#[no_mangle]
pub extern "C" fn semi_player_set_speed(player: *mut SemiPlayerHandle, speed: c_double) -> c_int {
    with_playback_coordinated_player_locked(player, |player| {
        orchestrator::set_speed(player, speed)
    })
    .unwrap_or_else(|code| code)
}

#[no_mangle]
pub extern "C" fn semi_player_set_video_presentation_bias_ms(
    player: *mut SemiPlayerHandle,
    bias_ms: i32,
) -> c_int {
    with_playback_coordinated_player_locked(player, |player| {
        orchestrator::set_video_presentation_bias(player, bias_ms)
    })
    .unwrap_or_else(|code| code)
}

#[no_mangle]
pub extern "C" fn semi_player_set_subtitle_visible(
    player: *mut SemiPlayerHandle,
    visible: c_int,
) -> c_int {
    with_playback_coordinated_player_locked(player, |player| {
        orchestrator::set_subtitle_visible(player, visible != 0)
    })
    .unwrap_or_else(|code| code)
}

#[no_mangle]
pub extern "C" fn semi_player_set_video_presentation_profile(
    player: *mut SemiPlayerHandle,
    profile: u32,
) -> c_int {
    let Some(profile) = SemiVideoPresentationProfile::from_raw(profile) else {
        return SEMI_E_INVALID_ARG;
    };

    with_playback_coordinated_player_locked(player, |player| {
        let profile = match profile {
            SemiVideoPresentationProfile::Passthrough => {
                crate::render::core::pipeline::PresentationTargetProfile::Passthrough
            }
            SemiVideoPresentationProfile::CpuBgraCompatibility => {
                crate::render::core::pipeline::PresentationTargetProfile::CpuBgraCompatibility
            }
            SemiVideoPresentationProfile::D3d11BgraPresenter => {
                crate::render::core::pipeline::PresentationTargetProfile::D3d11BgraPresenter
            }
        };
        orchestrator::set_video_presentation_profile(player, profile)
    })
    .unwrap_or_else(|code| code)
}

#[no_mangle]
/// # Safety
///
/// `player` must be a valid handle and `out_state` must be a valid, writable pointer.
pub unsafe extern "C" fn semi_player_get_state(
    player: *mut SemiPlayerHandle,
    out_state: *mut u32,
) -> c_int {
    if out_state.is_null() {
        return SEMI_E_INVALID_ARG;
    }

    match with_player_locked(player, |player| unsafe {
        *out_state = player.state().as_raw();
    }) {
        Ok(()) => SEMI_OK,
        Err(code) => code,
    }
}

#[no_mangle]
/// # Safety
///
/// `player` must be a valid handle and `out_position_ms` must be a valid, writable pointer.
pub unsafe extern "C" fn semi_player_get_position_ms(
    player: *mut SemiPlayerHandle,
    out_position_ms: *mut i64,
) -> c_int {
    if out_position_ms.is_null() {
        return SEMI_E_INVALID_ARG;
    }

    match with_player_locked(player, |player| unsafe {
        *out_position_ms = us_to_ms(player.audio_clock.presentation_time_us());
    }) {
        Ok(()) => SEMI_OK,
        Err(code) => code,
    }
}

#[no_mangle]
#[allow(clippy::redundant_closure_for_method_calls)]
/// # Safety
///
/// `player` must be a valid handle and `out_duration_ms` must be a valid, writable pointer.
pub unsafe extern "C" fn semi_player_get_duration_ms(
    player: *mut SemiPlayerHandle,
    out_duration_ms: *mut i64,
) -> c_int {
    if out_duration_ms.is_null() {
        return SEMI_E_INVALID_ARG;
    }

    match with_player_locked(player, |player| unsafe {
        *out_duration_ms = player.media_duration_us().map_or(0, us_to_ms);
    }) {
        Ok(()) => SEMI_OK,
        Err(code) => code,
    }
}

#[no_mangle]
/// # Safety
///
/// `player` must be a valid handle and `out_media_info` must be a valid, writable pointer.
pub unsafe extern "C" fn semi_player_get_media_info(
    player: *mut SemiPlayerHandle,
    out_media_info: *mut SemiMediaInfo,
) -> c_int {
    if out_media_info.is_null() {
        return SEMI_E_INVALID_ARG;
    }

    with_player_locked(player, |player| {
        if !player.is_media_loaded() {
            return SEMI_E_INVALID_STATE;
        }

        let Some(media_info) = player.media_info() else {
            return SEMI_E_INVALID_STATE;
        };

        unsafe {
            *out_media_info = build_media_info_view(&media_info);
        }

        SEMI_OK
    })
    .unwrap_or_else(|code| code)
}

#[no_mangle]
/// # Safety
///
/// `player` must be a valid handle and `out_output` must be a valid, writable pointer.
pub unsafe extern "C" fn semi_player_debug_decode_next(
    player: *mut SemiPlayerHandle,
    out_output: *mut SemiDecodedOutput,
) -> c_int {
    if out_output.is_null() {
        return SEMI_E_INVALID_ARG;
    }

    with_player_locked(player, |player| {
        if !player.is_media_loaded() {
            return SEMI_E_INVALID_STATE;
        }

        let output = match player.next_debug_decoded_output() {
            Ok(Some(output)) => Ok(output),
            Ok(None) => Ok(DecodedOutput::EndOfStream),
            Err(_) => Err(SEMI_E_INVALID_STATE),
        };
        let output = match output {
            Ok(output) => output,
            Err(code) => return code,
        };

        unsafe {
            *out_output = build_decoded_output_view(output);
        }

        SEMI_OK
    })
    .unwrap_or_else(|code| code)
}

#[no_mangle]
pub extern "C" fn semi_player_pump(player: *mut SemiPlayerHandle, max_iterations: u32) -> c_int {
    with_playback_coordinated_player_locked(player, |player| {
        let code = pump_player(player, max_iterations);
        player.notify_workers();
        code
    })
    .unwrap_or_else(|code| code)
}

#[no_mangle]
/// # Safety
///
/// `player` must be a valid handle and `out_snapshot` must be a valid, writable pointer.
pub unsafe extern "C" fn semi_player_get_playback_snapshot(
    player: *mut SemiPlayerHandle,
    out_snapshot: *mut SemiPlaybackSnapshot,
) -> c_int {
    if out_snapshot.is_null() {
        return SEMI_E_INVALID_ARG;
    }

    with_player_locked(player, |player| {
        if !player.is_media_loaded() {
            return SEMI_E_INVALID_STATE;
        }

        unsafe {
            *out_snapshot = build_playback_snapshot(player);
        }

        SEMI_OK
    })
    .unwrap_or_else(|code| code)
}

#[no_mangle]
/// # Safety
///
/// `player` must be a valid handle and `out_snapshot` must be a valid, writable pointer.
pub unsafe extern "C" fn semi_player_get_audio_output_snapshot(
    player: *mut SemiPlayerHandle,
    out_snapshot: *mut SemiAudioOutputSnapshot,
) -> c_int {
    if out_snapshot.is_null() {
        return SEMI_E_INVALID_ARG;
    }

    with_player_locked(player, |player| {
        if !player.is_media_loaded() {
            return SEMI_E_INVALID_STATE;
        }

        unsafe {
            *out_snapshot = build_audio_output_snapshot(player);
        }

        SEMI_OK
    })
    .unwrap_or_else(|code| code)
}

#[no_mangle]
/// # Safety
///
/// `player` must be a valid handle and `out_frame_info` must be a valid, writable pointer.
pub unsafe extern "C" fn semi_player_get_current_video_frame_info(
    player: *mut SemiPlayerHandle,
    out_frame_info: *mut SemiVideoFrameInfo,
) -> c_int {
    if out_frame_info.is_null() {
        return SEMI_E_INVALID_ARG;
    }

    with_player_locked(player, |player| {
        if !player.is_media_loaded() {
            return SEMI_E_INVALID_STATE;
        }

        let Some(frame) = player.runtime.current_video_frame() else {
            return SEMI_E_INVALID_STATE;
        };

        unsafe {
            *out_frame_info = build_video_frame_info(frame);
        }

        SEMI_OK
    })
    .unwrap_or_else(|code| code)
}

#[no_mangle]
/// # Safety
///
/// `player` must be a valid handle and `out_surface_desc` must be a valid, writable pointer.
pub unsafe extern "C" fn semi_player_get_current_video_surface_desc(
    player: *mut SemiPlayerHandle,
    out_surface_desc: *mut SemiVideoSurfaceDesc,
) -> c_int {
    if out_surface_desc.is_null() {
        return SEMI_E_INVALID_ARG;
    }

    with_player_locked(player, |player| {
        if !player.is_media_loaded() {
            return SEMI_E_INVALID_STATE;
        }

        let Some(frame) = player.runtime.current_video_frame() else {
            return SEMI_E_INVALID_STATE;
        };

        unsafe {
            *out_surface_desc = build_video_surface_desc(frame);
        }

        SEMI_OK
    })
    .unwrap_or_else(|code| code)
}

#[no_mangle]
/// # Safety
///
/// `player` must be a valid handle. `destination` must be a valid writable buffer of at least
/// `destination_len` bytes.
pub unsafe extern "C" fn semi_player_copy_current_video_frame_bgra(
    player: *mut SemiPlayerHandle,
    destination: *mut u8,
    destination_len: u32,
) -> c_int {
    if destination.is_null() {
        return SEMI_E_INVALID_ARG;
    }

    with_player_locked(player, |player| {
        if !player.is_media_loaded() {
            return SEMI_E_INVALID_STATE;
        }

        let Some(frame) = player.runtime.current_video_frame() else {
            return SEMI_E_INVALID_STATE;
        };

        let required_len = frame.byte_len();
        let destination_len = usize::try_from(destination_len).unwrap_or(usize::MAX);
        if destination_len < required_len {
            return SEMI_E_INVALID_ARG;
        }

        let Some(data) = frame.cpu_packed_data() else {
            return SEMI_E_INVALID_STATE;
        };

        unsafe {
            std::ptr::copy_nonoverlapping(data.as_ptr(), destination, required_len);
        }

        SEMI_OK
    })
    .unwrap_or_else(|code| code)
}

#[no_mangle]
pub extern "C" fn semi_ffmpeg_version_string() -> *mut c_char {
    let version = ffmpeg_next::util::version();
    match CString::new(format!("FFmpeg version: {version}")) {
        Ok(value) => value.into_raw(),
        Err(_) => ptr::null_mut(),
    }
}
