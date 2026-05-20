use ffmpeg_next::ffi;

use crate::decode::decoder::{OpenedAudioDecoder, OpenedVideoDecoder};
use crate::decode::error::MediaOpenError;
use crate::decode::output::{DecodePolicy, DecodedOutput, DecodedOutputPoll};
use crate::decode::policy::{VideoDecodeOpenOptions, VideoDecodeRequirements};
use crate::decode::session_decode::SessionDecodeState;
use crate::decode::session_lifecycle::{
    build_media_session, open_media_with_hw_device_ctx as open_media_session_with_hw_device_ctx,
    open_media_with_video_decode_requirements as open_media_session_with_video_decode_requirements,
    seek_media_session, video_decode_diagnostics_snapshot,
};
use crate::decode::session_shared::SharedMediaSession;
use crate::decode::video_decode::VideoDecodeDiagnosticsSnapshot;
use crate::demux::demux_impl::{SeekDemuxDiagnostics, SeekDemuxDiagnosticsSnapshot};
use crate::util::time::MediaTimeUs;

use crate::demux::probe::MediaInfo;

pub struct MediaSession {
    pub(crate) path: String,
    pub(crate) input: ffmpeg_next::format::context::Input,
    pub(crate) info: MediaInfo,
    pub(crate) video_decoder: Option<OpenedVideoDecoder>,
    pub(crate) audio_decoder: Option<OpenedAudioDecoder>,
    pub(crate) decode_state: SessionDecodeState,
    pub(crate) seek_diagnostics: SeekDemuxDiagnostics,
}

#[allow(dead_code)]
pub fn open_media(path: &str) -> Result<MediaSession, MediaOpenError> {
    open_media_with_hw_device_ctx(path, None)
}

pub fn open_media_with_hw_device_ctx(
    path: &str,
    hw_device_ctx: Option<*mut ffi::AVBufferRef>,
) -> Result<MediaSession, MediaOpenError> {
    open_media_session_with_hw_device_ctx(path, hw_device_ctx)
}

pub fn open_media_with_video_decode_requirements(
    path: &str,
    requirements: VideoDecodeRequirements,
    hw_device_ctx: Option<*mut ffi::AVBufferRef>,
) -> Result<MediaSession, MediaOpenError> {
    open_media_session_with_video_decode_requirements(path, requirements, hw_device_ctx)
}

impl MediaSession {
    pub fn from_input(
        path: String,
        input: ffmpeg_next::format::context::Input,
        hw_device_ctx: Option<*mut ffi::AVBufferRef>,
    ) -> Result<Self, MediaOpenError> {
        build_media_session(
            path,
            input,
            VideoDecodeOpenOptions {
                requirements: VideoDecodeRequirements::performance(),
                hw_device_ctx,
            },
        )
    }

    pub fn from_input_with_video_decode_requirements(
        path: String,
        input: ffmpeg_next::format::context::Input,
        requirements: VideoDecodeRequirements,
        hw_device_ctx: Option<*mut ffi::AVBufferRef>,
    ) -> Result<Self, MediaOpenError> {
        build_media_session(
            path,
            input,
            VideoDecodeOpenOptions {
                requirements,
                hw_device_ctx,
            },
        )
    }

    #[allow(dead_code)]
    pub fn path(&self) -> &str {
        &self.path
    }

    pub fn info(&self) -> &MediaInfo {
        &self.info
    }

    pub fn duration_us(&self) -> Option<MediaTimeUs> {
        self.info.duration_us
    }

    #[allow(dead_code)]
    pub(crate) fn best_video_decoder(&self) -> Option<&OpenedVideoDecoder> {
        self.video_decoder.as_ref()
    }

    #[allow(dead_code)]
    pub(crate) fn best_audio_decoder(&self) -> Option<&OpenedAudioDecoder> {
        self.audio_decoder.as_ref()
    }

    pub fn seek(&mut self, position_us: MediaTimeUs) -> Result<MediaTimeUs, MediaOpenError> {
        seek_media_session(self, position_us)
    }

    pub fn seek_diagnostics_snapshot(&self) -> SeekDemuxDiagnosticsSnapshot {
        self.seek_diagnostics.snapshot()
    }

    pub fn video_decode_diagnostics_snapshot(&self) -> VideoDecodeDiagnosticsSnapshot {
        video_decode_diagnostics_snapshot(&self.video_decoder)
    }

    pub fn finish_seek_diagnostics(&mut self) {
        self.seek_diagnostics.finish();
    }

    pub fn flush_decoders(&mut self) {
        self.decode_state
            .reset(&mut self.video_decoder, &mut self.audio_decoder);
    }

    pub fn next_decoded_output(&mut self) -> Result<Option<DecodedOutput>, MediaOpenError> {
        loop {
            match self.poll_decoded_output(usize::MAX, DecodePolicy::default())? {
                DecodedOutputPoll::Output(output) => return Ok(Some(output)),
                DecodedOutputPoll::Pending => {}
                DecodedOutputPoll::Finished => return Ok(None),
            }
        }
    }

    pub fn poll_decoded_output(
        &mut self,
        max_packets: usize,
        policy: DecodePolicy,
    ) -> Result<DecodedOutputPoll, MediaOpenError> {
        self.decode_state.poll_decoded_output(
            &mut self.input,
            &self.info,
            &mut self.video_decoder,
            &mut self.audio_decoder,
            &mut self.seek_diagnostics,
            policy,
            max_packets,
        )
    }
}

#[allow(dead_code)]
pub type OpenedMedia = MediaSession;
#[allow(dead_code)]
pub type SharedOpenedMedia = SharedMediaSession;
