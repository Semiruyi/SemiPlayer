mod api;
mod audio;
mod core;
mod platform;
mod render;
mod subtitle;
mod util;

use crate::api::error::{
    ResultCode, SEMI_E_DECODER_OPEN_FAILED, SEMI_E_INVALID_ARG, SEMI_E_INVALID_STATE,
    SEMI_E_MEDIA_OPEN_FAILED, SEMI_E_MEDIA_PROBE_FAILED, SEMI_E_SEEK_FAILED, SEMI_OK,
};
use crate::api::types::{
    PlayerState, SemiAudioOutputSnapshot, SemiDecodedKind, SemiDecodedOutput, SemiMediaInfo,
    SemiPlaybackSnapshot, SemiVideoDecodeBackend, SemiVideoDecodeFallbackReason,
    SemiVideoFrameInfo, SemiVideoPresentationProfile, SemiVideoSurfaceDesc,
    SemiVideoSurfaceKind,
};
use crate::core::media::{
    open_media, DecodedOutput, MediaInfo, MediaOpenError, MediaProbeError, SharedOpenedMedia,
    StreamKind, VideoDecodeBackend, VideoDecodeFallbackReason,
};
use crate::core::player::handle::SemiPlayerHandle;
use crate::core::player::pump::pump_player;
use crate::core::player::schedule::PlayerScheduleService;
use crate::core::player::video_sync::VideoSyncService;
use crate::render::core::frame::{VideoFrame, VideoSurfaceStorage};
use crate::util::time::{ms_to_us, us_to_ms};
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

fn option_index_to_i32(index: Option<usize>) -> i32 {
    index
        .and_then(|value| i32::try_from(value).ok())
        .unwrap_or(-1)
}

fn build_media_info_view(media_info: &MediaInfo) -> SemiMediaInfo {
    let best_video_stream = media_info.best_video_stream();
    let best_audio_stream = media_info.best_audio_stream();

    SemiMediaInfo {
        duration_ms: media_info.duration_us.map_or(0, us_to_ms),
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
        video_frame_rate_num: best_video_stream
            .and_then(|stream| stream.video.map(|video| video.avg_frame_rate_num))
            .unwrap_or(0),
        video_frame_rate_den: best_video_stream
            .and_then(|stream| stream.video.map(|video| video.avg_frame_rate_den))
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
            duration_ms: frame.duration_us.map_or(0, us_to_ms),
            width: frame.width,
            height: frame.height,
            sample_rate: 0,
            channels: 0,
            sample_count: 0,
            flags: u32::from(frame.is_key_frame),
        },
        DecodedOutput::SkippedVideo(frame) => SemiDecodedOutput {
            kind: SemiDecodedKind::None.as_raw(),
            pts_ms: us_to_ms(frame.pts_us),
            duration_ms: frame.duration_us.map_or(0, us_to_ms),
            width: 0,
            height: 0,
            sample_rate: 0,
            channels: 0,
            sample_count: 0,
            flags: 0,
        },
        DecodedOutput::Audio(frame) => SemiDecodedOutput {
            kind: SemiDecodedKind::Audio.as_raw(),
            pts_ms: us_to_ms(frame.pts_us),
            duration_ms: frame.duration_us.map_or(0, us_to_ms),
            width: 0,
            height: 0,
            sample_rate: frame.sample_rate,
            channels: frame.channels,
            sample_count: u32::try_from(frame.sample_count).unwrap_or(u32::MAX),
            flags: u32::from(frame.is_planar),
        },
        DecodedOutput::SkippedAudio(frame) => SemiDecodedOutput {
            kind: SemiDecodedKind::None.as_raw(),
            pts_ms: us_to_ms(frame.pts_us),
            duration_ms: frame.duration_us.map_or(0, us_to_ms),
            width: 0,
            height: 0,
            sample_rate: 0,
            channels: 0,
            sample_count: 0,
            flags: 0,
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

fn diagnostic_us_to_ms(value_us: i64) -> i64 {
    if value_us < 0 {
        value_us
    } else {
        us_to_ms(value_us)
    }
}

#[allow(clippy::too_many_lines)]
fn build_playback_snapshot(player: &SemiPlayerHandle) -> SemiPlaybackSnapshot {
    let runtime_video = player.runtime.video_snapshot();
    let last_audio_frame = player.runtime.last_audio_frame();
    let audio_position_us = player.audio_clock.presentation_time_us();
    let playback_position_ms = us_to_ms(audio_position_us);
    let sync_snapshot = VideoSyncService::evaluate(player, audio_position_us);
    let sync_stats = player.video_sync.stats();
    let schedule_hint = PlayerScheduleService::evaluate(player);
    let diagnostics = player.diagnostics_snapshot();
    let seek_demux = player
        .opened_media
        .as_ref()
        .map(crate::core::media::SharedOpenedMedia::seek_diagnostics_snapshot)
        .unwrap_or_default();
    let video_decode = player
        .opened_media
        .as_ref()
        .map(crate::core::media::SharedOpenedMedia::video_decode_diagnostics_snapshot)
        .unwrap_or_default();
    let audio_output_snapshot = player
        .audio_output
        .with_ref(crate::audio::core::output_controller::AudioOutputController::snapshot);
    let host_presentation_offset_ms = i32::try_from(us_to_ms(player.host_presentation_offset_us))
        .unwrap_or_else(|_| {
            if player.host_presentation_offset_us.is_negative() {
                i32::MIN
            } else {
                i32::MAX
            }
        });
    let core_av_delta_ms = runtime_video
        .current_pts_us
        .map_or(0, |pts_us| playback_position_ms - us_to_ms(pts_us));
    let next_video_pts_ms = runtime_video.next_pts_us.map_or(0, us_to_ms);
    let current_to_next_video_delta_ms = runtime_video
        .current_to_next_delta_us
        .map_or(0, us_to_ms);
    let (current_video_surface_kind, current_video_surface_pixel_format) = runtime_video
        .current_frame
        .as_ref()
        .map(|frame| {
            let surface_kind = match &frame.surface.storage {
                VideoSurfaceStorage::CpuPacked { .. } => SemiVideoSurfaceKind::CpuPacked,
                VideoSurfaceStorage::D3d11Texture2D { .. } => SemiVideoSurfaceKind::D3d11Texture2D,
            };
            (surface_kind.as_raw(), frame.pixel_format().as_raw())
        })
        .unwrap_or((SemiVideoSurfaceKind::Unknown.as_raw(), 0));
    let core_sync_error_ms = sync_snapshot.core_sync_error_us / 1_000;
    let expected_end_to_end_av_delta_ms = core_av_delta_ms - i64::from(host_presentation_offset_ms);

    SemiPlaybackSnapshot {
        audio_position_ms: playback_position_ms,
        audio_queue_len: u32::try_from(player.runtime.audio_queue_len()).unwrap_or(u32::MAX),
        video_queue_len: u32::try_from(player.runtime.video_queue_len()).unwrap_or(u32::MAX),
        has_current_video_frame: u32::from(runtime_video.current_frame.is_some()),
        current_video_pts_ms: runtime_video.current_pts_us.map_or(0, us_to_ms),
        current_video_duration_ms: runtime_video.current_duration_us.map_or(0, us_to_ms),
        video_decode_backend: map_video_decode_backend(video_decode.backend).as_raw(),
        video_hardware_requested: u32::from(video_decode.hardware_requested),
        video_hardware_active: u32::from(video_decode.hardware_active),
        video_decode_fallback_reason: map_video_decode_fallback_reason(video_decode.fallback_reason)
            .as_raw(),
        current_video_surface_kind,
        current_video_surface_pixel_format,
        current_video_effective_end_ms: sync_snapshot
            .current_video_effective_end_us
            .map_or(0, us_to_ms),
        next_video_pts_ms,
        current_to_next_video_delta_ms,
        next_video_wake_deadline_ms: sync_snapshot
            .next_wake_deadline_us
            .map_or(0, us_to_ms),
        last_audio_pts_ms: last_audio_frame
            .map_or(0, |frame| us_to_ms(frame.pts_us)),
        host_presentation_offset_ms,
        core_av_delta_ms,
        core_sync_error_ms,
        expected_end_to_end_av_delta_ms,
        video_sync_ticks: sync_stats.tick_count,
        video_sync_runs: sync_stats.sync_count,
        video_sync_presents: sync_stats.present_count,
        video_sync_drops: sync_stats.drop_count,
        video_sync_underflows: sync_stats.underflow_count,
        video_sync_late_hits: sync_stats.late_count,
        last_sync_presented_frames: sync_stats.last_presented_frames,
        last_sync_dropped_frames: sync_stats.last_dropped_frames,
        max_sync_presented_frames: sync_stats.max_presented_frames_in_run,
        max_sync_dropped_frames: sync_stats.max_dropped_frames_in_run,
        sync_run_present_only_count: sync_stats.run_present_only_count,
        sync_run_drop_only_count: sync_stats.run_drop_only_count,
        sync_run_present_drop_count: sync_stats.run_present_drop_count,
        sync_run_other_count: sync_stats.run_other_count,
        suggested_pump_wait_ms: us_to_ms(schedule_hint.suggested_wait_us),
        next_audio_refill_deadline_ms: schedule_hint
            .next_audio_refill_deadline_us
            .map_or(0, us_to_ms),
        next_pump_deadline_ms: schedule_hint
            .next_pump_deadline_us
            .map_or(0, us_to_ms),
        ffi_lock_wait_last_us: diagnostics.ffi_lock_wait_last_us,
        ffi_lock_wait_max_us: diagnostics.ffi_lock_wait_max_us,
        sync_worker_lock_wait_last_us: diagnostics.sync_worker_lock_wait_last_us,
        sync_worker_lock_wait_max_us: diagnostics.sync_worker_lock_wait_max_us,
        decode_worker_lock_wait_last_us: diagnostics.decode_worker_lock_wait_last_us,
        decode_worker_lock_wait_max_us: diagnostics.decode_worker_lock_wait_max_us,
        worker_deadline_slip_last_us: diagnostics.worker_deadline_slip_last_us,
        worker_deadline_slip_max_us: diagnostics.worker_deadline_slip_max_us,
        stale_audio_discard_event_count: diagnostics.stale_audio_discard_event_count,
        stale_audio_discard_frame_count: diagnostics.stale_audio_discard_frame_count,
        stale_audio_discard_last_frame_count: diagnostics.stale_audio_discard_last_frame_count,
        stale_audio_discard_last_lag_us: diagnostics.stale_audio_discard_last_lag_us,
        stale_audio_discard_max_lag_us: diagnostics.stale_audio_discard_max_lag_us,
        render_frames_total: diagnostics.render_frames_total,
        render_passthrough_frames_total: diagnostics.render_passthrough_frames_total,
        render_passthrough_with_subtitle_intent_frames_total: diagnostics
            .render_passthrough_with_subtitle_intent_frames_total,
        render_requires_transform_frames_total: diagnostics
            .render_requires_transform_frames_total,
        seek_event_count: diagnostics.seek_event_count,
        seek_active: u32::from(diagnostics.seek_active),
        last_seek_target_ms: us_to_ms(diagnostics.last_seek_target_us),
        seek_api_duration_us: diagnostics.seek_api_duration_us,
        seek_lock_wait_us: diagnostics.seek_lock_wait_us,
        seek_ffmpeg_seek_us: diagnostics.seek_ffmpeg_seek_us,
        seek_reset_us: diagnostics.seek_reset_us,
        seek_first_video_decoded_us: diagnostics.seek_first_video_decoded_us,
        seek_first_video_pts_ms: diagnostic_us_to_ms(diagnostics.seek_first_video_pts_us),
        seek_first_post_target_video_decoded_us: diagnostics.seek_first_post_target_video_decoded_us,
        seek_first_post_target_video_pts_ms: diagnostic_us_to_ms(
            diagnostics.seek_first_post_target_video_pts_us,
        ),
        seek_audio_position_at_first_post_target_video_decoded_ms: diagnostic_us_to_ms(
            diagnostics.seek_audio_position_at_first_post_target_video_decoded_us,
        ),
        seek_first_audio_decoder_output_us: diagnostics.seek_first_audio_decoder_output_us,
        seek_first_audio_decoded_us: diagnostics.seek_first_audio_decoded_us,
        seek_first_current_video_ready_us: diagnostics.seek_first_current_video_ready_us,
        seek_first_current_video_pts_ms: diagnostic_us_to_ms(
            diagnostics.seek_first_current_video_pts_us,
        ),
        seek_audio_position_at_first_current_video_ms: diagnostic_us_to_ms(
            diagnostics.seek_audio_position_at_first_current_video_us,
        ),
        seek_audio_advanced_between_post_target_decode_and_current_ms: diagnostic_us_to_ms(
            diagnostics.seek_audio_advanced_between_post_target_decode_and_current_us,
        ),
        seek_post_target_video_dropped_before_current_count: diagnostics
            .seek_post_target_video_dropped_before_current_count,
        seek_audio_output_started_before_current: u32::from(
            diagnostics.seek_audio_output_started_before_current,
        ),
        seek_audio_output_start_us: diagnostics.seek_audio_output_start_us,
        seek_target_video_ready_us: diagnostics.seek_target_video_ready_us,
        seek_target_video_pts_ms: diagnostic_us_to_ms(diagnostics.seek_target_video_pts_us),
        seek_target_audio_ready_us: diagnostics.seek_target_audio_ready_us,
        seek_stable_us: diagnostics.seek_stable_us,
        seek_pre_target_video_decoded_count: diagnostics.seek_pre_target_video_decoded_count,
        seek_pre_target_current_video_count: diagnostics.seek_pre_target_current_video_count,
        seek_first_video_packet_pts_ms: diagnostic_us_to_ms(seek_demux.first_video_packet_pts_us),
        seek_first_video_packet_dts_ms: diagnostic_us_to_ms(seek_demux.first_video_packet_dts_us),
        seek_first_video_packet_is_key: u32::from(seek_demux.first_video_packet_is_key),
        seek_first_video_packet_pos: seek_demux.first_video_packet_pos,
        seek_first_video_packet_stream_index: seek_demux.first_video_packet_stream_index,
        seek_first_video_packet_stream_kind: stream_kind_to_u32(seek_demux.first_video_packet_stream_kind),
        seek_video_packets_read: seek_demux.video_packets_read,
        seek_audio_packets_read: seek_demux.audio_packets_read,
        seek_video_frames_output: seek_demux.video_frames_output,
        seek_video_frames_skipped: seek_demux.video_frames_skipped,
        seek_audio_frames_output: seek_demux.audio_frames_output,
        seek_audio_frames_skipped: seek_demux.audio_frames_skipped,
        seek_expected_left_keyframe_pts_ms: diagnostic_us_to_ms(
            seek_demux.expected_left_keyframe_pts_us,
        ),
        seek_expected_left_keyframe_dts_ms: diagnostic_us_to_ms(
            seek_demux.expected_left_keyframe_dts_us,
        ),
        audio_output_started: u32::from(audio_output_snapshot.started),
        pending_device_frames: u32::try_from(audio_output_snapshot.pending_device_frames)
            .unwrap_or(u32::MAX),
        rendered_frames_total: audio_output_snapshot.rendered_frames_total,
        audible_frames_total: audio_output_snapshot.audible_frames_total,
        end_of_stream: u32::from(player.runtime.has_reached_end_of_stream()),
    }
}

fn stream_kind_to_u32(kind: StreamKind) -> u32 {
    match kind {
        StreamKind::Unknown => 0,
        StreamKind::Video => 1,
        StreamKind::Audio => 2,
        StreamKind::Subtitle => 3,
        StreamKind::Data => 4,
        StreamKind::Attachment => 5,
    }
}

fn map_video_decode_backend(backend: VideoDecodeBackend) -> SemiVideoDecodeBackend {
    match backend {
        VideoDecodeBackend::Unknown => SemiVideoDecodeBackend::Unknown,
        VideoDecodeBackend::SoftwareBgra => SemiVideoDecodeBackend::SoftwareBgra,
        VideoDecodeBackend::D3d11va => SemiVideoDecodeBackend::D3d11va,
    }
}

fn map_video_decode_fallback_reason(
    reason: VideoDecodeFallbackReason,
) -> SemiVideoDecodeFallbackReason {
    match reason {
        VideoDecodeFallbackReason::None => SemiVideoDecodeFallbackReason::None,
        VideoDecodeFallbackReason::NoHardwareConfig => {
            SemiVideoDecodeFallbackReason::NoHardwareConfig
        }
        VideoDecodeFallbackReason::HwDeviceCreateFailed => {
            SemiVideoDecodeFallbackReason::HwDeviceCreateFailed
        }
        VideoDecodeFallbackReason::HwDeviceContextBindFailed => {
            SemiVideoDecodeFallbackReason::HwDeviceContextBindFailed
        }
        VideoDecodeFallbackReason::HwDecoderOpenFailed => {
            SemiVideoDecodeFallbackReason::HwDecoderOpenFailed
        }
        VideoDecodeFallbackReason::HwDecoderTypeMismatch => {
            SemiVideoDecodeFallbackReason::HwDecoderTypeMismatch
        }
    }
}

fn build_video_frame_info(frame: &VideoFrame) -> SemiVideoFrameInfo {
    SemiVideoFrameInfo {
        pts_ms: us_to_ms(frame.pts_us),
        duration_ms: frame.duration_us.map_or(0, us_to_ms),
        width: frame.width,
        height: frame.height,
        stride: u32::try_from(frame.stride()).unwrap_or(u32::MAX),
        pixel_format: frame.pixel_format().as_raw(),
        byte_len: u32::try_from(frame.byte_len()).unwrap_or(u32::MAX),
        flags: u32::from(frame.is_key_frame),
    }
}

fn build_video_surface_desc(frame: &VideoFrame) -> SemiVideoSurfaceDesc {
    let (kind, texture_ptr, shared_handle, array_slice) = match &frame.surface.storage {
        VideoSurfaceStorage::CpuPacked { .. } => (SemiVideoSurfaceKind::CpuPacked, 0, 0, 0),
        VideoSurfaceStorage::D3d11Texture2D {
            texture_ptr,
            shared_handle,
            array_slice,
        } => (
            SemiVideoSurfaceKind::D3d11Texture2D,
            *texture_ptr,
            shared_handle.unwrap_or(0),
            *array_slice,
        ),
    };

    SemiVideoSurfaceDesc {
        kind: kind.as_raw(),
        pixel_format: frame.pixel_format().as_raw(),
        width: frame.width,
        height: frame.height,
        stride: u32::try_from(frame.stride()).unwrap_or(u32::MAX),
        byte_len: u32::try_from(frame.byte_len()).unwrap_or(u32::MAX),
        flags: u32::from(frame.is_key_frame),
        texture_ptr,
        shared_handle,
        array_slice,
        reserved0: 0,
    }
}

fn build_audio_output_snapshot(player: &SemiPlayerHandle) -> SemiAudioOutputSnapshot {
    let snapshot = player
        .audio_output
        .with_ref(crate::audio::core::output_controller::AudioOutputController::snapshot);
    let device_timing = snapshot.device_timing;

    SemiAudioOutputSnapshot {
        configured_sample_rate: snapshot
            .configured_format
            .map_or(0, |format| format.sample_rate),
        configured_channels: snapshot
            .configured_format
            .map_or(0, |format| format.channels),
        reserved0: 0,
        target_buffer_frames: u32::try_from(snapshot.target_buffer_frames).unwrap_or(u32::MAX),
        buffered_frames: u32::try_from(snapshot.buffered_frames).unwrap_or(u32::MAX),
        pending_device_frames: u32::try_from(snapshot.pending_device_frames).unwrap_or(u32::MAX),
        rendered_frames_total: snapshot.rendered_frames_total,
        audible_frames_total: snapshot.audible_frames_total,
        submitted_frames_total: snapshot.submitted_frames_total,
        started: u32::from(snapshot.started),
        has_device_timing: u32::from(device_timing.is_some()),
        base_pts_ms: device_timing
            .map_or(0, |timing| us_to_ms(timing.base_pts_us)),
        device_played_frames: device_timing
            .map_or(0, |timing| timing.played_frames),
    }
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

    let opened_media = match open_media(&path) {
        Ok(opened_media) => opened_media,
        Err(MediaOpenError::Probe(MediaProbeError::OpenInput(_))) => {
            return SEMI_E_MEDIA_OPEN_FAILED
        }
        Err(
            MediaOpenError::Probe(
                MediaProbeError::FfmpegInit(_) | MediaProbeError::Decoder(_),
            )
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
        player.bump_media_generation();
        player.opened_media = Some(SharedOpenedMedia::new(opened_media));
        player.reset_runtime_state();
        VideoSyncService::mark_dirty(player);
        player.set_state(PlayerState::Ready);
        player.notify_workers();
    }) {
        Ok(()) => SEMI_OK,
        Err(code) => code,
    }
}

#[no_mangle]
pub extern "C" fn semi_player_play(player: *mut SemiPlayerHandle) -> c_int {
    with_playback_coordinated_player_locked(player, |player| {
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
    })
    .unwrap_or_else(|code| code)
}

#[no_mangle]
pub extern "C" fn semi_player_pause(player: *mut SemiPlayerHandle) -> c_int {
    with_playback_coordinated_player_locked(player, |player| {
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
    })
    .unwrap_or_else(|code| code)
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
    let target_us = ms_to_us(position_ms.max(0));
    if !player.is_null() {
        unsafe { (&*player).observe_seek_requested(target_us) };
    }

    with_playback_coordinated_player_locked(player, |player| {
        player.observe_seek_lock_acquired();
        if !player.is_media_loaded() {
            player.observe_seek_aborted();
            return SEMI_E_INVALID_STATE;
        }
        if position_ms < 0 {
            player.observe_seek_aborted();
            return SEMI_E_INVALID_ARG;
        }

        let Some(opened_media) = player.opened_media.as_ref() else {
            player.observe_seek_aborted();
            return SEMI_E_INVALID_STATE;
        };

        player.observe_seek_ffmpeg_seek_started();
        if opened_media
            .with_mut(|opened_media| opened_media.seek(target_us))
            .is_err()
        {
            player.observe_seek_aborted();
            return SEMI_E_SEEK_FAILED;
        }
        player.observe_seek_ffmpeg_seek_finished();

        player.bump_media_generation();
        player.runtime.clear();
        player
            .audio_output
            .with_mut(crate::audio::core::output_controller::AudioOutputController::clear_buffer);
        player.audio_clock.seek(target_us);
        if player.state() == PlayerState::Playing {
            player.audio_clock.pause();
        }
        player.video_scheduler = crate::render::core::scheduler::VideoScheduler;
        player.video_sync.reset();
        player.begin_seek_recovery(target_us);
        player.observe_seek_reset_finished();
        player.notify_workers();
        player.observe_seek_api_completed();
        SEMI_OK
    })
    .unwrap_or_else(|code| code)
}

#[no_mangle]
pub extern "C" fn semi_player_reset(player: *mut SemiPlayerHandle) -> c_int {
    with_playback_coordinated_player_locked(player, |player| {
        player.bump_media_generation();
        player.clear_media();
        player.set_state(PlayerState::Idle);
        player.notify_workers();
        SEMI_OK
    })
    .unwrap_or_else(|code| code)
}

#[no_mangle]
pub extern "C" fn semi_player_set_speed(player: *mut SemiPlayerHandle, speed: c_double) -> c_int {
    with_playback_coordinated_player_locked(player, |player| {
        if !player.is_media_loaded() {
            return SEMI_E_INVALID_STATE;
        }
        if !speed.is_finite() || speed <= 0.0 {
            return SEMI_E_INVALID_ARG;
        }

        player.speed = speed;
        player.audio_clock.set_speed(speed);
        VideoSyncService::mark_dirty(player);
        player.notify_workers();
        SEMI_OK
    })
    .unwrap_or_else(|code| code)
}

#[no_mangle]
pub extern "C" fn semi_player_set_video_presentation_bias_ms(
    player: *mut SemiPlayerHandle,
    bias_ms: i32,
) -> c_int {
    with_playback_coordinated_player_locked(player, |player| {
        player.host_presentation_offset_us = ms_to_us(i64::from(bias_ms));
        VideoSyncService::mark_dirty(player);
        player.notify_workers();
        SEMI_OK
    })
    .unwrap_or_else(|code| code)
}

#[no_mangle]
pub extern "C" fn semi_player_set_subtitle_visible(
    player: *mut SemiPlayerHandle,
    visible: c_int,
) -> c_int {
    with_playback_coordinated_player_locked(player, |player| {
        if !player.is_media_loaded() {
            return SEMI_E_INVALID_STATE;
        }

        player.subtitles_visible = visible != 0;
        SEMI_OK
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
        if !player.is_media_loaded() {
            return SEMI_E_INVALID_STATE;
        }

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
        player.set_video_presentation_profile(profile);
        VideoSyncService::mark_dirty(player);
        player.notify_workers();
        SEMI_OK
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
        *out_duration_ms = player
            .opened_media
            .as_ref()
            .and_then(|opened_media| opened_media.with_ref(|opened_media| opened_media.duration_us()))
            .map_or(0, us_to_ms);
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

        let Some(media_info_view) = player.opened_media.as_ref().map(|opened_media| {
            opened_media.with_ref(|opened_media| build_media_info_view(opened_media.info()))
        }) else {
            return SEMI_E_INVALID_STATE;
        };

        unsafe {
            *out_media_info = media_info_view;
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

        let Some(opened_media) = player.opened_media.as_ref() else {
            return SEMI_E_INVALID_STATE;
        };

        let output =
            opened_media.with_mut(|opened_media| match opened_media.next_decoded_output() {
                Ok(Some(output)) => Ok(output),
                Ok(None) => Ok(DecodedOutput::EndOfStream),
                Err(_) => Err(SEMI_E_INVALID_STATE),
            });
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
