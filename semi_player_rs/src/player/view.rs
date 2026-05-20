use crate::api::types::{
    SemiAudioOutputSnapshot, SemiDecodedKind, SemiDecodedOutput, SemiMediaInfo,
    SemiPlaybackSnapshot, SemiVideoDecodeBackend, SemiVideoDecodeFallbackReason,
    SemiVideoFrameInfo, SemiVideoSurfaceDesc, SemiVideoSurfaceKind,
};
use crate::audio::core::output_controller::AudioOutputSnapshot;
use crate::decode::{DecodedOutput, VideoDecodeBackend, VideoDecodeFallbackReason};
use crate::demux::{MediaInfo, StreamKind};
use crate::player::access::PlaybackSnapshotInputs;
use crate::player::handle::SemiPlayerHandle;
use crate::render::core::frame::{VideoFrame, VideoSurfaceStorage};
use crate::util::time::us_to_ms;

fn option_index_to_i32(index: Option<usize>) -> i32 {
    index
        .and_then(|value| i32::try_from(value).ok())
        .unwrap_or(-1)
}

pub fn build_media_info_view(media_info: &MediaInfo) -> SemiMediaInfo {
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

pub fn build_decoded_output_view(output: DecodedOutput) -> SemiDecodedOutput {
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
pub fn build_playback_snapshot(player: &SemiPlayerHandle) -> SemiPlaybackSnapshot {
    build_playback_snapshot_from_inputs(player.playback_snapshot_inputs())
}

#[allow(clippy::too_many_lines)]
pub fn build_playback_snapshot_from_inputs(
    inputs: PlaybackSnapshotInputs,
) -> SemiPlaybackSnapshot {
    let runtime = inputs.runtime;
    let runtime_video = runtime.video;
    let playback_position_ms = us_to_ms(inputs.playback_position_us);
    let sync_snapshot = inputs.video_sync_snapshot;
    let sync_stats = inputs.video_sync_stats;
    let schedule_hint = inputs.schedule_hint;
    let diagnostics = inputs.diagnostics;
    let seek_demux = inputs.seek_demux;
    let video_decode = inputs.video_decode;
    let audio_output_snapshot = inputs.audio_output;
    let host_presentation_offset_us = inputs.control.host_presentation_offset_us;
    let host_presentation_offset_ms = i32::try_from(us_to_ms(host_presentation_offset_us))
        .unwrap_or_else(|_| {
            if host_presentation_offset_us.is_negative() {
                i32::MIN
            } else {
                i32::MAX
            }
        });
    let core_av_delta_ms = runtime_video
        .current_pts_us
        .map_or(0, |pts_us| playback_position_ms - us_to_ms(pts_us));
    let next_video_pts_ms = runtime_video.next_pts_us.map_or(0, us_to_ms);
    let current_to_next_video_delta_ms = runtime_video.current_to_next_delta_us.map_or(0, us_to_ms);
    let (current_video_surface_kind, current_video_surface_pixel_format) =
        (inputs.current_video_surface_kind_raw, inputs.current_video_pixel_format_raw);
    let core_sync_error_ms = sync_snapshot.core_sync_error_us / 1_000;
    let expected_end_to_end_av_delta_ms = core_av_delta_ms - i64::from(host_presentation_offset_ms);

    SemiPlaybackSnapshot {
        audio_position_ms: playback_position_ms,
        audio_queue_len: u32::try_from(runtime.audio_queue_len).unwrap_or(u32::MAX),
        video_queue_len: u32::try_from(runtime.video_queue_len).unwrap_or(u32::MAX),
        has_current_video_frame: u32::from(runtime_video.has_current_frame),
        current_video_pts_ms: runtime_video.current_pts_us.map_or(0, us_to_ms),
        current_video_duration_ms: runtime_video.current_duration_us.map_or(0, us_to_ms),
        video_decode_backend: map_video_decode_backend(video_decode.backend).as_raw(),
        video_hardware_requested: u32::from(video_decode.hardware_requested),
        video_hardware_active: u32::from(video_decode.hardware_active),
        video_decode_fallback_reason: map_video_decode_fallback_reason(
            video_decode.fallback_reason,
        )
        .as_raw(),
        current_video_surface_kind,
        current_video_surface_pixel_format,
        current_video_effective_end_ms: sync_snapshot
            .current_video_effective_end_us
            .map_or(0, us_to_ms),
        next_video_pts_ms,
        current_to_next_video_delta_ms,
        next_video_wake_deadline_ms: sync_snapshot.next_wake_deadline_us.map_or(0, us_to_ms),
        last_audio_pts_ms: runtime.last_audio_pts_us.map_or(0, us_to_ms),
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
        next_pump_deadline_ms: schedule_hint.next_pump_deadline_us.map_or(0, us_to_ms),
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
        render_requires_transform_frames_total: diagnostics.render_requires_transform_frames_total,
        render_fallback_passthrough_frames_total: diagnostics
            .render_fallback_passthrough_frames_total,
        seek_event_count: diagnostics.seek_event_count,
        seek_active: u32::from(diagnostics.seek_active),
        last_seek_target_ms: us_to_ms(diagnostics.last_seek_target_us),
        seek_api_duration_us: diagnostics.seek_api_duration_us,
        seek_lock_wait_us: diagnostics.seek_lock_wait_us,
        seek_ffmpeg_seek_us: diagnostics.seek_ffmpeg_seek_us,
        seek_reset_us: diagnostics.seek_reset_us,
        seek_first_video_decoded_us: diagnostics.seek_first_video_decoded_us,
        seek_first_video_pts_ms: diagnostic_us_to_ms(diagnostics.seek_first_video_pts_us),
        seek_first_post_target_video_decoded_us: diagnostics
            .seek_first_post_target_video_decoded_us,
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
        seek_first_video_packet_stream_kind: stream_kind_to_u32(
            seek_demux.first_video_packet_stream_kind,
        ),
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
        end_of_stream: u32::from(runtime.end_of_stream),
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

pub fn build_video_frame_info(frame: &VideoFrame) -> SemiVideoFrameInfo {
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

pub fn build_video_surface_desc(frame: &VideoFrame) -> SemiVideoSurfaceDesc {
    let (kind, texture_ptr, shared_handle, array_slice) = match &frame.surface.storage {
        VideoSurfaceStorage::CpuPacked { .. } => (SemiVideoSurfaceKind::CpuPacked, 0, 0, 0),
        VideoSurfaceStorage::GpuTexture(data) => match data.backend() {
            crate::render::gpu::GpuBackendKind::D3d11 => (
                SemiVideoSurfaceKind::D3d11Texture2D,
                data.texture_ptr,
                data.shared_handle.unwrap_or(0),
                data.array_slice,
            ),
        },
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

pub fn build_audio_output_snapshot(snapshot: AudioOutputSnapshot) -> SemiAudioOutputSnapshot {
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
        base_pts_ms: device_timing.map_or(0, |timing| us_to_ms(timing.base_pts_us)),
        device_played_frames: device_timing.map_or(0, |timing| timing.played_frames),
    }
}
