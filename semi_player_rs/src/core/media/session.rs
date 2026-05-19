use std::collections::VecDeque;
use std::sync::{Arc, Mutex};

use ffmpeg_next as ffmpeg;
use ffmpeg_next::ffi;
use ffmpeg_next::{Packet, Rescale};

use crate::core::media::decoder::{
    DecoderDrainingState, MediaPacket, OpenedAudioDecoder, OpenedVideoDecoder,
    collect_audio_frames, collect_video_frames, decode_audio_packet, decode_video_packet,
    open_audio_decoder, open_video_decoder, send_audio_decoder_eof, send_video_decoder_eof,
};
use crate::core::media::demux_impl::{SeekDemuxDiagnostics, SeekDemuxDiagnosticsSnapshot};
use crate::core::media::error::MediaOpenError;
use crate::core::media::keyframe_probe::probe_expected_left_keyframe_pts;
use crate::core::media::output::{DecodePolicy, DecodedOutput, DecodedOutputPoll};
use crate::core::media::video_decode::VideoDecodeDiagnosticsSnapshot;
use crate::util::time::MediaTimeUs;

use super::probe::{collect_media_info, MediaInfo, MediaProbeError};

pub struct MediaSession {
    path: String,
    input: ffmpeg::format::context::Input,
    info: MediaInfo,
    video_decoder: Option<OpenedVideoDecoder>,
    audio_decoder: Option<OpenedAudioDecoder>,
    pending_outputs: VecDeque<DecodedOutput>,
    draining_state: DecoderDrainingState,
    seek_diagnostics: SeekDemuxDiagnostics,
}

#[derive(Clone)]
#[allow(clippy::arc_with_non_send_sync)]
pub struct SharedMediaSession {
    inner: Arc<Mutex<MediaSession>>,
}

#[allow(dead_code)]
pub fn open_media(path: &str) -> Result<MediaSession, MediaOpenError> {
    open_media_with_hw_device_ctx(path, None)
}

pub fn open_media_with_hw_device_ctx(
    path: &str,
    hw_device_ctx: Option<*mut ffi::AVBufferRef>,
) -> Result<MediaSession, MediaOpenError> {
    ffmpeg::init()
        .map_err(MediaProbeError::FfmpegInit)
        .map_err(MediaOpenError::Probe)?;

    let input = ffmpeg::format::input(&path)
        .map_err(MediaProbeError::OpenInput)
        .map_err(MediaOpenError::Probe)?;

    MediaSession::from_input(path.to_owned(), input, hw_device_ctx)
}

impl MediaSession {
    pub fn from_input(
        path: String,
        input: ffmpeg::format::context::Input,
        hw_device_ctx: Option<*mut ffi::AVBufferRef>,
    ) -> Result<Self, MediaOpenError> {
        let info = collect_media_info(&input).map_err(MediaOpenError::Probe)?;
        let video_decoder = open_video_decoder(&input, info.best_video_stream_index, hw_device_ctx)?;
        let audio_decoder = open_audio_decoder(&input, info.best_audio_stream_index)?;

        Ok(Self {
            path,
            input,
            info,
            video_decoder,
            audio_decoder,
            pending_outputs: VecDeque::new(),
            draining_state: DecoderDrainingState::default(),
            seek_diagnostics: SeekDemuxDiagnostics::default(),
        })
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
        self.seek_diagnostics.begin();
        let probed_keyframe_pts = probe_expected_left_keyframe_pts(
            &self.path,
            self.info.best_video_stream_index,
            position_us,
        );
        self.seek_diagnostics
            .observe_expected_left_keyframe(probed_keyframe_pts);
        let position = position_us.rescale((1, 1_000_000), ffmpeg::rescale::TIME_BASE);
        if let Err(error) = self.input.seek(position, ..) {
            self.seek_diagnostics.finish();
            return Err(MediaOpenError::Seek(error));
        }
        self.flush_decoders();
        let keyframe_pts = probed_keyframe_pts
            .map(|(pts, _)| pts)
            .unwrap_or(position_us);
        Ok(keyframe_pts)
    }

    pub fn seek_diagnostics_snapshot(&self) -> SeekDemuxDiagnosticsSnapshot {
        self.seek_diagnostics.snapshot()
    }

    pub fn video_decode_diagnostics_snapshot(&self) -> VideoDecodeDiagnosticsSnapshot {
        self.video_decoder
            .as_ref()
            .map(OpenedVideoDecoder::diagnostics_snapshot)
            .unwrap_or_default()
    }

    pub fn finish_seek_diagnostics(&mut self) {
        self.seek_diagnostics.finish();
    }

    pub fn flush_decoders(&mut self) {
        if let Some(video_decoder) = self.video_decoder.as_mut() {
            video_decoder.decoder.flush();
        }

        if let Some(audio_decoder) = self.audio_decoder.as_mut() {
            audio_decoder.decoder.flush();
        }

        self.pending_outputs.clear();
        self.draining_state = DecoderDrainingState::default();
    }

    pub(crate) fn read_next_packet(&mut self) -> Result<Option<MediaPacket>, MediaOpenError> {
        let mut packet = Packet::empty();
        match packet.read(&mut self.input) {
            Ok(()) => {
                let stream_index = packet.stream();
                self.seek_diagnostics.observe_packet(
                    stream_index,
                    &packet,
                    self.info.best_video_stream_index,
                    self.info.best_audio_stream_index,
                    self.input
                        .stream(stream_index)
                        .map(|stream| stream.time_base()),
                );
                Ok(Some(MediaPacket {
                    stream_index,
                    packet,
                }))
            }
            Err(ffmpeg::Error::Eof) => Ok(None),
            Err(error) => Err(MediaOpenError::ReadPacket(error)),
        }
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
        if let Some(output) = self.pending_outputs.pop_front() {
            return Ok(DecodedOutputPoll::Output(output));
        }

        if self.draining_state.input_exhausted {
            self.collect_drained_outputs(policy)?;

            if let Some(output) = self.pending_outputs.pop_front() {
                return Ok(DecodedOutputPoll::Output(output));
            }

            if self.has_fully_drained() && !self.draining_state.end_of_stream_emitted {
                self.draining_state.end_of_stream_emitted = true;
                return Ok(DecodedOutputPoll::Output(DecodedOutput::EndOfStream));
            }

            return Ok(if self.has_fully_drained() {
                DecodedOutputPoll::Finished
            } else {
                DecodedOutputPoll::Pending
            });
        }

        let packet_budget = max_packets.max(1);
        for _ in 0..packet_budget {
            let Some(media_packet) = self.read_next_packet()? else {
                self.enter_draining_mode()?;
                return Ok(DecodedOutputPoll::Pending);
            };

            if self
                .video_decoder
                .as_ref()
                .is_some_and(|decoder| decoder.index == media_packet.stream_index)
            {
                let video_decoder = self.video_decoder.as_mut().expect("video decoder exists");
                decode_video_packet(
                    video_decoder,
                    &media_packet.packet,
                    &mut self.pending_outputs,
                    &mut self.seek_diagnostics,
                    policy,
                )?;
            } else if self
                .audio_decoder
                .as_ref()
                .is_some_and(|decoder| decoder.index == media_packet.stream_index)
            {
                let audio_decoder = self.audio_decoder.as_mut().expect("audio decoder exists");
                decode_audio_packet(
                    audio_decoder,
                    &media_packet.packet,
                    &mut self.pending_outputs,
                    &mut self.seek_diagnostics,
                    policy,
                )?;
            }

            if let Some(output) = self.pending_outputs.pop_front() {
                return Ok(DecodedOutputPoll::Output(output));
            }
        }

        Ok(DecodedOutputPoll::Pending)
    }

    fn enter_draining_mode(&mut self) -> Result<(), MediaOpenError> {
        self.draining_state.input_exhausted = true;

        if let Some(video_decoder) = self.video_decoder.as_mut() {
            send_video_decoder_eof(&mut video_decoder.decoder)?;
            self.draining_state.video_eof_sent = true;
        } else {
            self.draining_state.video_drained = true;
        }

        if let Some(audio_decoder) = self.audio_decoder.as_mut() {
            send_audio_decoder_eof(&mut audio_decoder.decoder)?;
            self.draining_state.audio_eof_sent = true;
        } else {
            self.draining_state.audio_drained = true;
        }

        Ok(())
    }

    fn collect_drained_outputs(&mut self, policy: DecodePolicy) -> Result<(), MediaOpenError> {
        if !self.draining_state.video_drained {
            if let Some(video_decoder) = self.video_decoder.as_mut() {
                self.draining_state.video_drained = collect_video_frames(
                    video_decoder,
                    &mut self.pending_outputs,
                    &mut self.seek_diagnostics,
                    true,
                    policy,
                )?;
            } else {
                self.draining_state.video_drained = true;
            }
        }

        if !self.draining_state.audio_drained {
            if let Some(audio_decoder) = self.audio_decoder.as_mut() {
                self.draining_state.audio_drained = collect_audio_frames(
                    audio_decoder,
                    &mut self.pending_outputs,
                    &mut self.seek_diagnostics,
                    true,
                    policy,
                )?;
            } else {
                self.draining_state.audio_drained = true;
            }
        }

        Ok(())
    }

    fn has_fully_drained(&self) -> bool {
        self.draining_state.video_drained && self.draining_state.audio_drained
    }
}

impl SharedMediaSession {
    #[allow(clippy::arc_with_non_send_sync)]
    pub fn new(opened_media: MediaSession) -> Self {
        Self {
            inner: Arc::new(Mutex::new(opened_media)),
        }
    }

    pub fn with_ref<T>(&self, f: impl FnOnce(&MediaSession) -> T) -> T {
        let guard = self.inner.lock().unwrap();
        f(&guard)
    }

    pub fn with_mut<T>(&self, f: impl FnOnce(&mut MediaSession) -> T) -> T {
        let mut guard = self.inner.lock().unwrap();
        f(&mut guard)
    }

    pub fn seek_diagnostics_snapshot(&self) -> SeekDemuxDiagnosticsSnapshot {
        let guard = self.inner.lock().unwrap();
        guard.seek_diagnostics_snapshot()
    }

    pub fn video_decode_diagnostics_snapshot(&self) -> VideoDecodeDiagnosticsSnapshot {
        let guard = self.inner.lock().unwrap();
        guard.video_decode_diagnostics_snapshot()
    }
}

#[allow(dead_code)]
pub type OpenedMedia = MediaSession;
#[allow(dead_code)]
pub type SharedOpenedMedia = SharedMediaSession;
