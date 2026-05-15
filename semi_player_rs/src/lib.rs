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
    ResultCode, SEMI_E_DECODER_OPEN_FAILED, SEMI_E_INVALID_ARG, SEMI_E_INVALID_STATE,
    SEMI_E_MEDIA_OPEN_FAILED, SEMI_E_MEDIA_PROBE_FAILED, SEMI_E_SEEK_FAILED, SEMI_OK,
};
use crate::api::types::{
    PlayerState, SemiDecodedKind, SemiDecodedOutput, SemiMediaInfo, SemiPlaybackSnapshot,
    SemiVideoFrameInfo,
};
use crate::core::media::{open_media, DecodedOutput, MediaInfo, MediaOpenError, MediaProbeError};
use crate::core::player::handle::SemiPlayerHandle;
use crate::core::player::pump::pump_player;
use crate::render::core::frame::VideoFrame;
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

fn option_index_to_i32(index: Option<usize>) -> i32 {
    index.and_then(|value| i32::try_from(value).ok()).unwrap_or(-1)
}

fn build_media_info_view(media_info: &MediaInfo) -> SemiMediaInfo {
    let best_video_stream = media_info.best_video_stream();
    let best_audio_stream = media_info.best_audio_stream();

    SemiMediaInfo {
        duration_ms: media_info.duration_us.map(us_to_ms).unwrap_or(0),
        stream_count: media_info.stream_count(),
        video_stream_count: media_info.video_stream_count(),
        audio_stream_count: media_info.audio_stream_count(),
        subtitle_stream_count: media_info.subtitle_stream_count(),
        best_video_stream_index: option_index_to_i32(media_info.best_video_stream_index),
        best_audio_stream_index: option_index_to_i32(media_info.best_audio_stream_index),
        best_subtitle_stream_index: option_index_to_i32(media_info.best_subtitle_stream_index),
        video_width: best_video_stream
            .and_then(|stream| stream.video.map(|video| video.width))
            .unwrap_or(0),
        video_height: best_video_stream
            .and_then(|stream| stream.video.map(|video| video.height))
            .unwrap_or(0),
        audio_sample_rate: best_audio_stream
            .and_then(|stream| stream.audio.map(|audio| audio.sample_rate))
            .unwrap_or(0),
        audio_channels: best_audio_stream
            .and_then(|stream| stream.audio.map(|audio| audio.channels))
            .unwrap_or(0),
        reserved0: 0,
    }
}

fn build_decoded_output_view(output: DecodedOutput) -> SemiDecodedOutput {
    match output {
        DecodedOutput::Video(frame) => SemiDecodedOutput {
            kind: SemiDecodedKind::Video.as_raw(),
            pts_ms: us_to_ms(frame.pts_us),
            duration_ms: frame.duration_us.map(us_to_ms).unwrap_or(0),
            width: frame.width,
            height: frame.height,
            sample_rate: 0,
            channels: 0,
            sample_count: 0,
            flags: u32::from(frame.is_key_frame),
        },
        DecodedOutput::Audio(frame) => SemiDecodedOutput {
            kind: SemiDecodedKind::Audio.as_raw(),
            pts_ms: us_to_ms(frame.pts_us),
            duration_ms: frame.duration_us.map(us_to_ms).unwrap_or(0),
            width: 0,
            height: 0,
            sample_rate: frame.sample_rate,
            channels: frame.channels,
            sample_count: u32::try_from(frame.sample_count).unwrap_or(u32::MAX),
            flags: u32::from(frame.is_planar),
        },
        DecodedOutput::EndOfStream => SemiDecodedOutput {
            kind: SemiDecodedKind::EndOfStream.as_raw(),
            pts_ms: 0,
            duration_ms: 0,
            width: 0,
            height: 0,
            sample_rate: 0,
            channels: 0,
            sample_count: 0,
            flags: 0,
        },
    }
}

fn build_playback_snapshot(player: &SemiPlayerHandle) -> SemiPlaybackSnapshot {
    let current_video_frame = player.runtime.current_video_frame();
    let last_audio_frame = player.runtime.last_audio_frame();

    SemiPlaybackSnapshot {
        audio_queue_len: u32::try_from(player.runtime.audio_queue_len()).unwrap_or(u32::MAX),
        video_queue_len: u32::try_from(player.runtime.video_queue_len()).unwrap_or(u32::MAX),
        has_current_video_frame: u32::from(current_video_frame.is_some()),
        current_video_pts_ms: current_video_frame.map(|frame| us_to_ms(frame.pts_us)).unwrap_or(0),
        current_video_duration_ms: current_video_frame
            .and_then(|frame| frame.duration_us.map(us_to_ms))
            .unwrap_or(0),
        last_audio_pts_ms: last_audio_frame.map(|frame| us_to_ms(frame.pts_us)).unwrap_or(0),
        end_of_stream: u32::from(player.runtime.has_reached_end_of_stream()),
    }
}

fn build_video_frame_info(frame: &VideoFrame) -> SemiVideoFrameInfo {
    SemiVideoFrameInfo {
        pts_ms: us_to_ms(frame.pts_us),
        duration_ms: frame.duration_us.map(us_to_ms).unwrap_or(0),
        width: frame.width,
        height: frame.height,
        stride: u32::try_from(frame.stride).unwrap_or(u32::MAX),
        pixel_format: frame.pixel_format.as_raw(),
        byte_len: u32::try_from(frame.byte_len()).unwrap_or(u32::MAX),
        flags: u32::from(frame.is_key_frame),
    }
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

    let opened_media = match open_media(&path) {
        Ok(opened_media) => opened_media,
        Err(MediaOpenError::Probe(MediaProbeError::OpenInput(_))) => return SEMI_E_MEDIA_OPEN_FAILED,
        Err(MediaOpenError::Probe(MediaProbeError::FfmpegInit(_)))
        | Err(MediaOpenError::Probe(MediaProbeError::Decoder(_))) => {
            return SEMI_E_MEDIA_PROBE_FAILED;
        }
        Err(MediaOpenError::VideoDecoder(_)) | Err(MediaOpenError::AudioDecoder(_)) => {
            return SEMI_E_DECODER_OPEN_FAILED;
        }
        Err(MediaOpenError::ReadPacket(_))
        | Err(MediaOpenError::SendPacket(_))
        | Err(MediaOpenError::ReceiveFrame(_))
        | Err(MediaOpenError::ScaleFrame(_))
        | Err(MediaOpenError::ResampleFrame(_)) => {
            return SEMI_E_DECODER_OPEN_FAILED;
        }
        Err(MediaOpenError::Seek(_)) => {
            return SEMI_E_MEDIA_PROBE_FAILED;
        }
    };

    match with_player_mut(player, |player| {
        player.opened_media = Some(opened_media);
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

        let target_us = ms_to_us(position_ms);
        let Some(opened_media) = player.opened_media.as_mut() else {
            return SEMI_E_INVALID_STATE;
        };

        if opened_media.seek(target_us).is_err() {
            return SEMI_E_SEEK_FAILED;
        }

        player.runtime.clear();
        player.audio_clock.seek(target_us);
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
            *out_duration_ms = player
                .opened_media
                .as_ref()
                .and_then(|opened_media| opened_media.duration_us())
                .map(us_to_ms)
                .unwrap_or(0);
        }
    }) {
        Ok(_) => SEMI_OK,
        Err(code) => code,
    }
}

#[no_mangle]
pub extern "C" fn semi_player_get_media_info(
    player: *mut SemiPlayerHandle,
    out_media_info: *mut SemiMediaInfo,
) -> c_int {
    if out_media_info.is_null() {
        return SEMI_E_INVALID_ARG;
    }

    match with_player_mut(player, |player| {
        if !player.is_media_loaded() {
            return SEMI_E_INVALID_STATE;
        }

        let Some(media_info) = player.opened_media.as_ref().map(|opened_media| opened_media.info()) else {
            return SEMI_E_INVALID_STATE;
        };

        unsafe {
            *out_media_info = build_media_info_view(media_info);
        }

        SEMI_OK
    }) {
        Ok(code) => code,
        Err(code) => code,
    }
}

#[no_mangle]
pub extern "C" fn semi_player_debug_decode_next(
    player: *mut SemiPlayerHandle,
    out_output: *mut SemiDecodedOutput,
) -> c_int {
    if out_output.is_null() {
        return SEMI_E_INVALID_ARG;
    }

    match with_player_mut(player, |player| {
        if !player.is_media_loaded() {
            return SEMI_E_INVALID_STATE;
        }

        let Some(opened_media) = player.opened_media.as_mut() else {
            return SEMI_E_INVALID_STATE;
        };

        let output = match opened_media.next_decoded_output() {
            Ok(Some(output)) => output,
            Ok(None) => DecodedOutput::EndOfStream,
            Err(_) => return SEMI_E_INVALID_STATE,
        };

        unsafe {
            *out_output = build_decoded_output_view(output);
        }

        SEMI_OK
    }) {
        Ok(code) => code,
        Err(code) => code,
    }
}

#[no_mangle]
pub extern "C" fn semi_player_pump(
    player: *mut SemiPlayerHandle,
    max_iterations: u32,
) -> c_int {
    match with_player_mut(player, |player| pump_player(player, max_iterations)) {
        Ok(code) => code,
        Err(code) => code,
    }
}

#[no_mangle]
pub extern "C" fn semi_player_get_playback_snapshot(
    player: *mut SemiPlayerHandle,
    out_snapshot: *mut SemiPlaybackSnapshot,
) -> c_int {
    if out_snapshot.is_null() {
        return SEMI_E_INVALID_ARG;
    }

    match with_player_mut(player, |player| {
        if !player.is_media_loaded() {
            return SEMI_E_INVALID_STATE;
        }

        unsafe {
            *out_snapshot = build_playback_snapshot(player);
        }

        SEMI_OK
    }) {
        Ok(code) => code,
        Err(code) => code,
    }
}

#[no_mangle]
pub extern "C" fn semi_player_get_current_video_frame_info(
    player: *mut SemiPlayerHandle,
    out_frame_info: *mut SemiVideoFrameInfo,
) -> c_int {
    if out_frame_info.is_null() {
        return SEMI_E_INVALID_ARG;
    }

    match with_player_mut(player, |player| {
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
    }) {
        Ok(code) => code,
        Err(code) => code,
    }
}

#[no_mangle]
pub extern "C" fn semi_player_copy_current_video_frame_bgra(
    player: *mut SemiPlayerHandle,
    destination: *mut u8,
    destination_len: u32,
) -> c_int {
    if destination.is_null() {
        return SEMI_E_INVALID_ARG;
    }

    match with_player_mut(player, |player| {
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

        unsafe {
            std::ptr::copy_nonoverlapping(frame.data.as_ptr(), destination, required_len);
        }

        SEMI_OK
    }) {
        Ok(code) => code,
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
