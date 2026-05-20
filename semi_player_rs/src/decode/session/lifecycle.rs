use ffmpeg_next as ffmpeg;
use ffmpeg_next::ffi;
use ffmpeg_next::Rescale;

use crate::decode::decoder::{
    open_audio_decoder, open_video_decoder_with_options, OpenedAudioDecoder, OpenedVideoDecoder,
};
use crate::decode::error::MediaOpenError;
use crate::decode::policy::{VideoDecodeOpenOptions, VideoDecodeRequirements};
use crate::decode::session_decode::SessionDecodeState;
use crate::decode::session_impl::MediaSession;
use crate::decode::video_decode::VideoDecodeDiagnosticsSnapshot;
use crate::demux::demux_impl::SeekDemuxDiagnostics;
use crate::demux::keyframe_probe::probe_expected_left_keyframe_pts;
use crate::demux::probe::{collect_media_info, MediaInfo, MediaProbeError};
use crate::util::time::MediaTimeUs;

pub(crate) fn open_media_with_hw_device_ctx(
    path: &str,
    hw_device_ctx: Option<*mut ffi::AVBufferRef>,
) -> Result<MediaSession, MediaOpenError> {
    open_media_with_video_decode_requirements(
        path,
        VideoDecodeRequirements::performance(),
        hw_device_ctx,
    )
}

pub(crate) fn open_media_with_video_decode_requirements(
    path: &str,
    requirements: VideoDecodeRequirements,
    hw_device_ctx: Option<*mut ffi::AVBufferRef>,
) -> Result<MediaSession, MediaOpenError> {
    ffmpeg::init()
        .map_err(MediaProbeError::FfmpegInit)
        .map_err(MediaOpenError::Probe)?;

    let input = ffmpeg::format::input(&path)
        .map_err(MediaProbeError::OpenInput)
        .map_err(MediaOpenError::Probe)?;

    build_media_session(
        path.to_owned(),
        input,
        VideoDecodeOpenOptions {
            requirements,
            hw_device_ctx,
        },
    )
}

pub(crate) fn build_media_session(
    path: String,
    input: ffmpeg::format::context::Input,
    video_decode_options: VideoDecodeOpenOptions,
) -> Result<MediaSession, MediaOpenError> {
    let info = collect_media_info(&input).map_err(MediaOpenError::Probe)?;
    let video_decoder =
        open_video_decoder_with_options(&input, info.best_video_stream_index, video_decode_options)?;
    let audio_decoder = open_audio_decoder(&input, info.best_audio_stream_index)?;

    Ok(MediaSession {
        path,
        input,
        info,
        video_decoder,
        audio_decoder,
        decode_state: SessionDecodeState::default(),
        seek_diagnostics: SeekDemuxDiagnostics::default(),
    })
}

pub(crate) fn seek_media_session(
    session: &mut MediaSession,
    position_us: MediaTimeUs,
) -> Result<MediaTimeUs, MediaOpenError> {
    session.seek_diagnostics.begin();
    let probed_keyframe_pts = probe_expected_left_keyframe_pts(
        &session.path,
        session.info.best_video_stream_index,
        position_us,
    );
    session
        .seek_diagnostics
        .observe_expected_left_keyframe(probed_keyframe_pts);
    let position = position_us.rescale((1, 1_000_000), ffmpeg::rescale::TIME_BASE);
    if let Err(error) = session.input.seek(position, ..) {
        session.seek_diagnostics.finish();
        return Err(MediaOpenError::Seek(error));
    }
    session.flush_decoders();
    Ok(probed_keyframe_pts
        .map(|(pts, _)| pts)
        .unwrap_or(position_us))
}

pub(crate) fn video_decode_diagnostics_snapshot(
    video_decoder: &Option<OpenedVideoDecoder>,
) -> VideoDecodeDiagnosticsSnapshot {
    video_decoder
        .as_ref()
        .map(OpenedVideoDecoder::diagnostics_snapshot)
        .unwrap_or_default()
}

#[allow(dead_code)]
pub(crate) fn media_info_duration_us(info: &MediaInfo) -> Option<MediaTimeUs> {
    info.duration_us
}

#[allow(dead_code)]
pub(crate) fn best_audio_decoder_ref(
    audio_decoder: &Option<OpenedAudioDecoder>,
) -> Option<&OpenedAudioDecoder> {
    audio_decoder.as_ref()
}
