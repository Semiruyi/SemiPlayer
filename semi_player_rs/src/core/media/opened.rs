use std::collections::VecDeque;
use std::error::Error;
use std::fmt;
use std::sync::{Arc, Mutex};

use ffmpeg_next as ffmpeg;
use ffmpeg_next::software::scaling::{context::Context as ScalingContext, Flags as ScalingFlags};
use ffmpeg_next::{format, frame, Packet, Rational, Rescale};

use crate::audio::core::frame::AudioFrame;
use crate::audio::core::resampler::NormalizedAudioResampler;
use crate::render::core::frame::{PixelFormatCategory, VideoFrame};
use crate::util::time::MediaTimeUs;

use super::probe::{collect_media_info, MediaInfo, MediaProbeError, StreamKind};

pub struct OpenedMedia {
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
pub struct SharedOpenedMedia {
    inner: Arc<Mutex<OpenedMedia>>,
}

pub struct OpenedVideoDecoder {
    pub index: usize,
    pub decoder: ffmpeg::decoder::Video,
    scaler: Option<ScalingContext>,
    estimated_frame_duration_us: Option<MediaTimeUs>,
}

pub struct OpenedAudioDecoder {
    pub index: usize,
    pub decoder: ffmpeg::decoder::Audio,
    resampler: NormalizedAudioResampler,
}

#[derive(Default)]
#[allow(clippy::struct_excessive_bools)]
struct DecoderDrainingState {
    input_exhausted: bool,
    video_eof_sent: bool,
    audio_eof_sent: bool,
    video_drained: bool,
    audio_drained: bool,
    end_of_stream_emitted: bool,
}

#[derive(Clone, Copy, Debug, Default)]
pub struct SeekDemuxDiagnosticsSnapshot {
    pub first_video_packet_pts_us: MediaTimeUs,
    pub first_video_packet_dts_us: MediaTimeUs,
    pub first_video_packet_is_key: bool,
    pub first_video_packet_pos: i64,
    pub first_video_packet_stream_index: i64,
    pub first_video_packet_stream_kind: StreamKind,
    pub video_packets_read: u64,
    pub audio_packets_read: u64,
    pub expected_left_keyframe_pts_us: MediaTimeUs,
    pub expected_left_keyframe_dts_us: MediaTimeUs,
}

#[derive(Debug, Default)]
struct SeekDemuxDiagnostics {
    active: bool,
    first_video_packet_pts_us: Option<MediaTimeUs>,
    first_video_packet_dts_us: Option<MediaTimeUs>,
    first_video_packet_is_key: bool,
    first_video_packet_pos: Option<i64>,
    first_video_packet_stream_index: Option<i64>,
    first_video_packet_stream_kind: StreamKind,
    video_packets_read: u64,
    audio_packets_read: u64,
    expected_left_keyframe_pts_us: Option<MediaTimeUs>,
    expected_left_keyframe_dts_us: Option<MediaTimeUs>,
    last_completed: SeekDemuxDiagnosticsSnapshot,
}

#[derive(Debug)]
pub enum MediaOpenError {
    Probe(MediaProbeError),
    VideoDecoder(ffmpeg::Error),
    AudioDecoder(ffmpeg::Error),
    ReadPacket(ffmpeg::Error),
    SendPacket(ffmpeg::Error),
    ReceiveFrame(ffmpeg::Error),
    ScaleFrame(ffmpeg::Error),
    ResampleFrame(ffmpeg::Error),
    Seek(ffmpeg::Error),
}

impl fmt::Display for MediaOpenError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Probe(error) => write!(f, "failed to probe media: {error}"),
            Self::VideoDecoder(error) => write!(f, "failed to open video decoder: {error}"),
            Self::AudioDecoder(error) => write!(f, "failed to open audio decoder: {error}"),
            Self::ReadPacket(error) => write!(f, "failed to read media packet: {error}"),
            Self::SendPacket(error) => write!(f, "failed to send packet to decoder: {error}"),
            Self::ReceiveFrame(error) => write!(f, "failed to receive decoded frame: {error}"),
            Self::ScaleFrame(error) => write!(f, "failed to scale video frame: {error}"),
            Self::ResampleFrame(error) => write!(f, "failed to resample audio frame: {error}"),
            Self::Seek(error) => write!(f, "failed to seek media input: {error}"),
        }
    }
}

impl Error for MediaOpenError {
    fn source(&self) -> Option<&(dyn Error + 'static)> {
        match self {
            Self::Probe(error) => Some(error),
            Self::VideoDecoder(error)
            | Self::AudioDecoder(error)
            | Self::ReadPacket(error)
            | Self::SendPacket(error)
            | Self::ReceiveFrame(error)
            | Self::ScaleFrame(error)
            | Self::ResampleFrame(error)
            | Self::Seek(error) => Some(error),
        }
    }
}

pub enum DecodedOutput {
    Video(VideoFrame),
    SkippedVideo(SkippedVideoFrame),
    Audio(AudioFrame),
    SkippedAudio(SkippedAudioFrame),
    EndOfStream,
}

pub enum DecodedOutputPoll {
    Output(DecodedOutput),
    Pending,
    Finished,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct DecodePolicy {
    pub seek_recovery: Option<SeekRecoveryPolicy>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct SeekRecoveryPolicy {
    pub target_video_us: MediaTimeUs,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct SkippedVideoFrame {
    pub pts_us: MediaTimeUs,
    pub duration_us: Option<MediaTimeUs>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct SkippedAudioFrame {
    pub pts_us: MediaTimeUs,
    pub duration_us: Option<MediaTimeUs>,
}

pub fn open_media(path: &str) -> Result<OpenedMedia, MediaOpenError> {
    ffmpeg::init()
        .map_err(MediaProbeError::FfmpegInit)
        .map_err(MediaOpenError::Probe)?;

    let input = ffmpeg::format::input(&path)
        .map_err(MediaProbeError::OpenInput)
        .map_err(MediaOpenError::Probe)?;

    OpenedMedia::from_input(path.to_owned(), input)
}

impl OpenedMedia {
    pub fn from_input(
        path: String,
        input: ffmpeg::format::context::Input,
    ) -> Result<Self, MediaOpenError> {
        let info = collect_media_info(&input).map_err(MediaOpenError::Probe)?;
        let video_decoder = open_video_decoder(&input, info.best_video_stream_index)?;
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
    pub fn best_video_decoder(&self) -> Option<&OpenedVideoDecoder> {
        self.video_decoder.as_ref()
    }

    #[allow(dead_code)]
    pub fn best_audio_decoder(&self) -> Option<&OpenedAudioDecoder> {
        self.audio_decoder.as_ref()
    }

    pub fn seek(&mut self, position_us: MediaTimeUs) -> Result<(), MediaOpenError> {
        self.seek_diagnostics.begin();
        self.seek_diagnostics.observe_expected_left_keyframe(
            probe_expected_left_keyframe_pts(&self.path, self.info.best_video_stream_index, position_us),
        );
        let position = position_us.rescale((1, 1_000_000), ffmpeg::rescale::TIME_BASE);
        if let Err(error) = self.input.seek(position, ..) {
            self.seek_diagnostics.finish();
            return Err(MediaOpenError::Seek(error));
        }
        self.flush_decoders();
        Ok(())
    }

    pub fn seek_diagnostics_snapshot(&self) -> SeekDemuxDiagnosticsSnapshot {
        self.seek_diagnostics.snapshot()
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

    pub fn read_next_packet(&mut self) -> Result<Option<MediaPacket>, MediaOpenError> {
        let mut packet = Packet::empty();
        match packet.read(&mut self.input) {
            Ok(()) => {
                let stream_index = packet.stream();
                self.seek_diagnostics.observe_packet(
                    stream_index,
                    &packet,
                    self.info.best_video_stream_index,
                    self.info.best_audio_stream_index,
                    self.input.stream(stream_index).map(|stream| stream.time_base()),
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
                self.draining_state.video_drained =
                    collect_video_frames(
                        video_decoder,
                        &mut self.pending_outputs,
                        true,
                        policy,
                    )?;
            } else {
                self.draining_state.video_drained = true;
            }
        }

        if !self.draining_state.audio_drained {
            if let Some(audio_decoder) = self.audio_decoder.as_mut() {
                self.draining_state.audio_drained =
                    collect_audio_frames(audio_decoder, &mut self.pending_outputs, true, policy)?;
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

impl SeekDemuxDiagnostics {
    fn begin(&mut self) {
        self.active = true;
        self.first_video_packet_pts_us = None;
        self.first_video_packet_dts_us = None;
        self.first_video_packet_is_key = false;
        self.first_video_packet_pos = None;
        self.first_video_packet_stream_index = None;
        self.first_video_packet_stream_kind = StreamKind::Unknown;
        self.video_packets_read = 0;
        self.audio_packets_read = 0;
        self.expected_left_keyframe_pts_us = None;
        self.expected_left_keyframe_dts_us = None;
        self.last_completed = SeekDemuxDiagnosticsSnapshot::default();
    }

    fn observe_packet(
        &mut self,
        stream_index: usize,
        packet: &Packet,
        best_video_stream_index: Option<usize>,
        best_audio_stream_index: Option<usize>,
        time_base: Option<Rational>,
    ) {
        if !self.active {
            return;
        }

        if Some(stream_index) == best_video_stream_index {
            self.video_packets_read = self.video_packets_read.saturating_add(1);
            if self.first_video_packet_pts_us.is_none() {
                self.first_video_packet_pts_us =
                    Some(packet_timestamp_us(packet.pts(), time_base));
                self.first_video_packet_dts_us =
                    Some(packet_timestamp_us(packet.dts(), time_base));
                self.first_video_packet_is_key = packet.is_key();
                self.first_video_packet_pos = i64::try_from(packet.position()).ok();
                self.first_video_packet_stream_index = i64::try_from(stream_index).ok();
                self.first_video_packet_stream_kind = StreamKind::Video;
            }
        } else if Some(stream_index) == best_audio_stream_index {
            self.audio_packets_read = self.audio_packets_read.saturating_add(1);
        }
    }

    fn observe_expected_left_keyframe(
        &mut self,
        expected_left_keyframe: Option<(MediaTimeUs, MediaTimeUs)>,
    ) {
        if let Some((pts_us, dts_us)) = expected_left_keyframe {
            self.expected_left_keyframe_pts_us = Some(pts_us);
            self.expected_left_keyframe_dts_us = Some(dts_us);
        }
    }

    fn finish(&mut self) {
        if !self.active {
            return;
        }

        self.last_completed = self.snapshot();
        self.active = false;
    }

    #[allow(clippy::similar_names)]
    fn snapshot(&self) -> SeekDemuxDiagnosticsSnapshot {
        let first_pts = self.first_video_packet_pts_us.unwrap_or(-1);
        let first_dts = self.first_video_packet_dts_us.unwrap_or(-1);
        let first_pos = self.first_video_packet_pos.unwrap_or(-1);
        let first_stream_index = self.first_video_packet_stream_index.unwrap_or(-1);
        let video_packets_read = self.video_packets_read;
        let audio_packets_read = self.audio_packets_read;
        let expected_keyframe_pts = self.expected_left_keyframe_pts_us.unwrap_or(-1);
        let expected_keyframe_dts = self.expected_left_keyframe_dts_us.unwrap_or(-1);

        if self.active {
            SeekDemuxDiagnosticsSnapshot {
                first_video_packet_pts_us: first_pts,
                first_video_packet_dts_us: first_dts,
                first_video_packet_is_key: self.first_video_packet_is_key,
                first_video_packet_pos: first_pos,
                first_video_packet_stream_index: first_stream_index,
                first_video_packet_stream_kind: self.first_video_packet_stream_kind,
                video_packets_read,
                audio_packets_read,
                expected_left_keyframe_pts_us: expected_keyframe_pts,
                expected_left_keyframe_dts_us: expected_keyframe_dts,
            }
        } else {
            self.last_completed
        }
    }
}

fn probe_expected_left_keyframe_pts(
    path: &str,
    best_video_stream_index: Option<usize>,
    target_us: MediaTimeUs,
) -> Option<(MediaTimeUs, MediaTimeUs)> {
    const VIDEO_PACKET_SCAN_LIMIT: usize = 512;

    let video_stream_index = best_video_stream_index?;
    let mut input = ffmpeg::format::input(path).ok()?;
    let target = target_us.rescale((1, 1_000_000), ffmpeg::rescale::TIME_BASE);
    // Diagnostic-only heuristic: reopen the input and scan nearby main-video packets
    // so we can compare the player's actual anchor against an expected left keyframe.
    let _ = input.seek(target, ..target);

    let mut best: Option<(MediaTimeUs, MediaTimeUs)> = None;
    let mut seen_past_target = false;
    let mut video_packets_scanned = 0usize;

    for (stream, packet) in input.packets() {
        if stream.index() != video_stream_index {
            continue;
        }

        video_packets_scanned = video_packets_scanned.saturating_add(1);
        let time_base = stream.time_base();
        let pts_us = packet_timestamp_us(packet.pts(), Some(time_base));
        let dts_us = packet_timestamp_us(packet.dts(), Some(time_base));

        if pts_us > target_us && dts_us > target_us {
            seen_past_target = true;
            if best.is_some() {
                break;
            }
        }

        if packet.is_key() && pts_us >= 0 && pts_us <= target_us {
            best = Some((pts_us, dts_us));
        }

        if seen_past_target && best.is_some() {
            break;
        }

        if video_packets_scanned >= VIDEO_PACKET_SCAN_LIMIT {
            break;
        }
    }

    best
}

impl SharedOpenedMedia {
    #[allow(clippy::arc_with_non_send_sync)]
    pub fn new(opened_media: OpenedMedia) -> Self {
        Self {
            inner: Arc::new(Mutex::new(opened_media)),
        }
    }

    pub fn with_ref<T>(&self, f: impl FnOnce(&OpenedMedia) -> T) -> T {
        let guard = self.inner.lock().unwrap();
        f(&guard)
    }

    pub fn with_mut<T>(&self, f: impl FnOnce(&mut OpenedMedia) -> T) -> T {
        let mut guard = self.inner.lock().unwrap();
        f(&mut guard)
    }

    pub fn seek_diagnostics_snapshot(&self) -> SeekDemuxDiagnosticsSnapshot {
        let guard = self.inner.lock().unwrap();
        guard.seek_diagnostics_snapshot()
    }
}

pub struct MediaPacket {
    pub stream_index: usize,
    pub packet: Packet,
}

fn open_video_decoder(
    input: &ffmpeg::format::context::Input,
    stream_index: Option<usize>,
) -> Result<Option<OpenedVideoDecoder>, MediaOpenError> {
    let Some(stream_index) = stream_index else {
        return Ok(None);
    };

    let stream = input
        .stream(stream_index)
        .ok_or(ffmpeg::Error::StreamNotFound)
        .map_err(MediaOpenError::VideoDecoder)?;
    let context = ffmpeg::codec::context::Context::from_parameters(stream.parameters())
        .map_err(MediaOpenError::VideoDecoder)?;
    let mut decoder = context.decoder();
    decoder.set_packet_time_base(stream.time_base());
    let decoder = decoder.video().map_err(MediaOpenError::VideoDecoder)?;

    Ok(Some(OpenedVideoDecoder {
        index: stream_index,
        decoder,
        scaler: None,
        estimated_frame_duration_us: estimate_stream_frame_duration_us(stream.avg_frame_rate()),
    }))
}

fn open_audio_decoder(
    input: &ffmpeg::format::context::Input,
    stream_index: Option<usize>,
) -> Result<Option<OpenedAudioDecoder>, MediaOpenError> {
    let Some(stream_index) = stream_index else {
        return Ok(None);
    };

    let stream = input
        .stream(stream_index)
        .ok_or(ffmpeg::Error::StreamNotFound)
        .map_err(MediaOpenError::AudioDecoder)?;
    let context = ffmpeg::codec::context::Context::from_parameters(stream.parameters())
        .map_err(MediaOpenError::AudioDecoder)?;
    let mut decoder = context.decoder();
    decoder.set_packet_time_base(stream.time_base());
    let decoder = decoder.audio().map_err(MediaOpenError::AudioDecoder)?;

    Ok(Some(OpenedAudioDecoder {
        index: stream_index,
        decoder,
        resampler: NormalizedAudioResampler::new(),
    }))
}

fn decode_video_packet(
    decoder: &mut OpenedVideoDecoder,
    packet: &Packet,
    outputs: &mut VecDeque<DecodedOutput>,
    policy: DecodePolicy,
) -> Result<(), MediaOpenError> {
    loop {
        match decoder.decoder.send_packet(packet) {
            Ok(()) => {
                let _ = collect_video_frames(decoder, outputs, false, policy)?;
                return Ok(());
            }
            Err(ffmpeg::Error::Other { errno }) if errno == ffmpeg::error::EAGAIN => {
                let output_count_before = outputs.len();
                let drained = collect_video_frames(decoder, outputs, false, policy)?;
                if drained || outputs.len() == output_count_before {
                    return Err(MediaOpenError::SendPacket(ffmpeg::Error::Other {
                        errno: ffmpeg::error::EAGAIN,
                    }));
                }
            }
            Err(error) => return Err(MediaOpenError::SendPacket(error)),
        }
    }
}

fn decode_audio_packet(
    decoder: &mut OpenedAudioDecoder,
    packet: &Packet,
    outputs: &mut VecDeque<DecodedOutput>,
    policy: DecodePolicy,
) -> Result<(), MediaOpenError> {
    loop {
        match decoder.decoder.send_packet(packet) {
            Ok(()) => {
                let _ = collect_audio_frames(decoder, outputs, false, policy)?;
                return Ok(());
            }
            Err(ffmpeg::Error::Other { errno }) if errno == ffmpeg::error::EAGAIN => {
                let output_count_before = outputs.len();
                let drained = collect_audio_frames(decoder, outputs, false, policy)?;
                if drained || outputs.len() == output_count_before {
                    return Err(MediaOpenError::SendPacket(ffmpeg::Error::Other {
                        errno: ffmpeg::error::EAGAIN,
                    }));
                }
            }
            Err(error) => return Err(MediaOpenError::SendPacket(error)),
        }
    }
}

fn collect_video_frames(
    decoder: &mut OpenedVideoDecoder,
    outputs: &mut VecDeque<DecodedOutput>,
    draining: bool,
    policy: DecodePolicy,
) -> Result<bool, MediaOpenError> {
    let mut reached_decoder_eof = false;

    loop {
        let mut frame = frame::Video::empty();
        match decoder.decoder.receive_frame(&mut frame) {
            Ok(()) => {
                let time_base = decoder.decoder.packet_time_base();
                let pts_us = frame_timestamp_us(frame.pts().or_else(|| frame.timestamp()), time_base);
                let duration_us = frame_duration_us(frame.packet().duration, time_base)
                    .or(decoder.estimated_frame_duration_us);

                if should_skip_video_frame_for_seek_recovery(policy, pts_us, duration_us) {
                    outputs.push_back(DecodedOutput::SkippedVideo(SkippedVideoFrame {
                        pts_us,
                        duration_us,
                    }));
                } else {
                    outputs.push_back(DecodedOutput::Video(map_video_frame(
                        decoder,
                        &frame,
                        pts_us,
                        duration_us,
                    )?));
                }
            }
            Err(ffmpeg::Error::Other { errno }) if errno == ffmpeg::error::EAGAIN => break,
            Err(ffmpeg::Error::Eof) => {
                reached_decoder_eof = true;
                break;
            }
            Err(error) => return Err(MediaOpenError::ReceiveFrame(error)),
        }
    }

    Ok(draining && reached_decoder_eof)
}

fn collect_audio_frames(
    decoder: &mut OpenedAudioDecoder,
    outputs: &mut VecDeque<DecodedOutput>,
    draining: bool,
    policy: DecodePolicy,
) -> Result<bool, MediaOpenError> {
    let mut reached_decoder_eof = false;

    loop {
        let mut frame = frame::Audio::empty();
        match decoder.decoder.receive_frame(&mut frame) {
            Ok(()) => {
                let time_base = decoder.decoder.packet_time_base();
                let pts_us = frame_timestamp_us(frame.pts().or_else(|| frame.timestamp()), time_base);
                let duration_us = audio_duration_us(&frame);

                if should_skip_audio_frame_for_seek_recovery(policy, pts_us, duration_us) {
                    outputs.push_back(DecodedOutput::SkippedAudio(SkippedAudioFrame {
                        pts_us,
                        duration_us,
                    }));
                    continue;
                }

                outputs.push_back(DecodedOutput::Audio(map_audio_frame(
                    decoder,
                    &frame,
                    pts_us,
                    duration_us,
                )?));
            }
            Err(ffmpeg::Error::Other { errno }) if errno == ffmpeg::error::EAGAIN => break,
            Err(ffmpeg::Error::Eof) => {
                reached_decoder_eof = true;
                break;
            }
            Err(error) => return Err(MediaOpenError::ReceiveFrame(error)),
        }
    }

    Ok(draining && reached_decoder_eof)
}

fn send_video_decoder_eof(decoder: &mut ffmpeg::decoder::Video) -> Result<(), MediaOpenError> {
    match decoder.send_eof() {
        Ok(()) | Err(ffmpeg::Error::Eof) => Ok(()),
        Err(error) => Err(MediaOpenError::SendPacket(error)),
    }
}

fn send_audio_decoder_eof(decoder: &mut ffmpeg::decoder::Audio) -> Result<(), MediaOpenError> {
    match decoder.send_eof() {
        Ok(()) | Err(ffmpeg::Error::Eof) => Ok(()),
        Err(error) => Err(MediaOpenError::SendPacket(error)),
    }
}

fn map_video_frame(
    decoder: &mut OpenedVideoDecoder,
    frame: &frame::Video,
    pts_us: MediaTimeUs,
    duration_us: Option<MediaTimeUs>,
) -> Result<VideoFrame, MediaOpenError> {
    let converted = convert_video_frame_to_bgra(decoder, frame)?;
    let stride = converted.stride(0);
    let data = copy_packed_plane(&converted);

    Ok(VideoFrame {
        pts_us,
        duration_us,
        width: converted.width(),
        height: converted.height(),
        pixel_format: PixelFormatCategory::Bgra8,
        stride,
        data,
        is_key_frame: frame.is_key(),
    })
}

fn map_audio_frame(
    decoder: &mut OpenedAudioDecoder,
    frame: &frame::Audio,
    pts_us: MediaTimeUs,
    duration_us: Option<MediaTimeUs>,
) -> Result<AudioFrame, MediaOpenError> {
    decoder
        .resampler
        .convert(&decoder.decoder, frame, pts_us, duration_us)
}

fn frame_timestamp_us(timestamp: Option<i64>, time_base: Rational) -> MediaTimeUs {
    timestamp
        .map_or(0, |value| value.rescale(time_base, (1, 1_000_000)))
}

fn packet_timestamp_us(timestamp: Option<i64>, time_base: Option<Rational>) -> MediaTimeUs {
    match (timestamp, time_base) {
        (Some(value), Some(time_base)) => value.rescale(time_base, (1, 1_000_000)),
        _ => -1,
    }
}

fn frame_duration_us(duration: i64, time_base: Rational) -> Option<MediaTimeUs> {
    if duration <= 0 {
        return None;
    }

    Some(duration.rescale(time_base, (1, 1_000_000)))
}

fn audio_duration_us(frame: &frame::Audio) -> Option<MediaTimeUs> {
    if frame.rate() == 0 || frame.samples() == 0 {
        return None;
    }

    let samples = i64::try_from(frame.samples()).ok()?;
    Some(
        samples
            .saturating_mul(1_000_000)
            .saturating_div(i64::from(frame.rate())),
    )
}

fn estimate_stream_frame_duration_us(frame_rate: Rational) -> Option<MediaTimeUs> {
    let numerator = i64::from(frame_rate.numerator());
    let denominator = i64::from(frame_rate.denominator());
    if numerator <= 0 || denominator <= 0 {
        return None;
    }

    Some(
        denominator
            .saturating_mul(1_000_000)
            .saturating_div(numerator),
    )
}

fn should_skip_video_frame_for_seek_recovery(
    policy: DecodePolicy,
    pts_us: MediaTimeUs,
    duration_us: Option<MediaTimeUs>,
) -> bool {
    let Some(seek_recovery) = policy.seek_recovery else {
        return false;
    };

    let Some(end_us) = duration_us.and_then(|duration_us| pts_us.checked_add(duration_us)) else {
        return false;
    };

    end_us <= seek_recovery.target_video_us
}

fn should_skip_audio_frame_for_seek_recovery(
    policy: DecodePolicy,
    pts_us: MediaTimeUs,
    duration_us: Option<MediaTimeUs>,
) -> bool {
    let Some(seek_recovery) = policy.seek_recovery else {
        return false;
    };

    let Some(end_us) = duration_us.and_then(|duration_us| pts_us.checked_add(duration_us)) else {
        return false;
    };

    end_us <= seek_recovery.target_video_us
}

fn convert_video_frame_to_bgra(
    decoder: &mut OpenedVideoDecoder,
    input: &frame::Video,
) -> Result<frame::Video, MediaOpenError> {
    ensure_video_scaler(decoder, input)?;

    let mut output = frame::Video::empty();
    decoder
        .scaler
        .as_mut()
        .expect("video scaler initialized")
        .run(input, &mut output)
        .map_err(MediaOpenError::ScaleFrame)?;

    Ok(output)
}

fn ensure_video_scaler(
    decoder: &mut OpenedVideoDecoder,
    input: &frame::Video,
) -> Result<(), MediaOpenError> {
    let needs_rebuild = decoder
        .scaler
        .as_ref()
        .is_none_or(|scaler| {
            scaler.input().format != input.format()
                || scaler.input().width != input.width()
                || scaler.input().height != input.height()
                || scaler.output().format != format::Pixel::BGRA
                || scaler.output().width != input.width()
                || scaler.output().height != input.height()
        });

    if needs_rebuild {
        decoder.scaler = Some(
            ScalingContext::get(
                input.format(),
                input.width(),
                input.height(),
                format::Pixel::BGRA,
                input.width(),
                input.height(),
                ScalingFlags::BILINEAR,
            )
            .map_err(MediaOpenError::ScaleFrame)?,
        );
    }

    Ok(())
}

fn copy_packed_plane(frame: &frame::Video) -> Vec<u8> {
    let stride = frame.stride(0);
    let height = usize::try_from(frame.height()).unwrap_or(0);
    let byte_len = stride.saturating_mul(height);
    let data = frame.data(0);

    data[..byte_len.min(data.len())].to_vec()
}

#[cfg(test)]
mod tests {
    use super::{
        should_skip_audio_frame_for_seek_recovery, should_skip_video_frame_for_seek_recovery,
        DecodePolicy, SeekRecoveryPolicy,
    };

    #[test]
    fn seek_recovery_skips_frame_that_ends_before_target() {
        let policy = DecodePolicy {
            seek_recovery: Some(SeekRecoveryPolicy {
                target_video_us: 10_000,
            }),
        };

        assert!(should_skip_video_frame_for_seek_recovery(
            policy,
            5_000,
            Some(4_000),
        ));
    }

    #[test]
    fn seek_recovery_keeps_frame_that_covers_target() {
        let policy = DecodePolicy {
            seek_recovery: Some(SeekRecoveryPolicy {
                target_video_us: 10_000,
            }),
        };

        assert!(!should_skip_video_frame_for_seek_recovery(
            policy,
            5_000,
            Some(6_000),
        ));
    }

    #[test]
    fn seek_recovery_skips_audio_frame_that_ends_before_target() {
        let policy = DecodePolicy {
            seek_recovery: Some(SeekRecoveryPolicy {
                target_video_us: 10_000,
            }),
        };

        assert!(should_skip_audio_frame_for_seek_recovery(
            policy,
            5_000,
            Some(4_000),
        ));
    }

    #[test]
    fn seek_recovery_keeps_audio_frame_that_covers_target() {
        let policy = DecodePolicy {
            seek_recovery: Some(SeekRecoveryPolicy {
                target_video_us: 10_000,
            }),
        };

        assert!(!should_skip_audio_frame_for_seek_recovery(
            policy,
            5_000,
            Some(6_000),
        ));
    }
}
