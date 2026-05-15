use std::collections::VecDeque;

use ffmpeg_next as ffmpeg;
use ffmpeg_next::{format, frame, Packet, Rational, Rescale};
use ffmpeg_next::software::scaling::{context::Context as ScalingContext, Flags as ScalingFlags};

use crate::audio::core::frame::AudioFrame;
use crate::audio::core::resampler::NormalizedAudioResampler;
use crate::render::core::frame::{PixelFormatCategory, VideoFrame};
use crate::util::time::MediaTimeUs;

use super::probe::{collect_media_info, MediaInfo, MediaProbeError};

pub struct OpenedMedia {
    path: String,
    input: ffmpeg::format::context::Input,
    info: MediaInfo,
    video_decoder: Option<OpenedVideoDecoder>,
    audio_decoder: Option<OpenedAudioDecoder>,
    pending_outputs: VecDeque<DecodedOutput>,
    draining_state: DecoderDrainingState,
}

pub struct OpenedVideoDecoder {
    pub index: usize,
    pub decoder: ffmpeg::decoder::Video,
    scaler: Option<ScalingContext>,
}

pub struct OpenedAudioDecoder {
    pub index: usize,
    pub decoder: ffmpeg::decoder::Audio,
    resampler: NormalizedAudioResampler,
}

#[derive(Default)]
struct DecoderDrainingState {
    input_exhausted: bool,
    video_eof_sent: bool,
    audio_eof_sent: bool,
    video_drained: bool,
    audio_drained: bool,
    end_of_stream_emitted: bool,
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

pub enum DecodedOutput {
    Video(VideoFrame),
    Audio(AudioFrame),
    EndOfStream,
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
        })
    }

    pub fn path(&self) -> &str {
        &self.path
    }

    pub fn info(&self) -> &MediaInfo {
        &self.info
    }

    pub fn duration_us(&self) -> Option<MediaTimeUs> {
        self.info.duration_us
    }

    pub fn best_video_decoder(&self) -> Option<&OpenedVideoDecoder> {
        self.video_decoder.as_ref()
    }

    pub fn best_audio_decoder(&self) -> Option<&OpenedAudioDecoder> {
        self.audio_decoder.as_ref()
    }

    pub fn seek(&mut self, position_us: MediaTimeUs) -> Result<(), MediaOpenError> {
        let position = position_us.rescale((1, 1_000_000), ffmpeg::rescale::TIME_BASE);
        self.input.seek(position, ..).map_err(MediaOpenError::Seek)?;
        self.flush_decoders();
        Ok(())
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
            Ok(()) => Ok(Some(MediaPacket {
                stream_index: packet.stream(),
                packet,
            })),
            Err(ffmpeg::Error::Eof) => Ok(None),
            Err(error) => Err(MediaOpenError::ReadPacket(error)),
        }
    }

    pub fn next_decoded_output(&mut self) -> Result<Option<DecodedOutput>, MediaOpenError> {
        loop {
            if let Some(output) = self.pending_outputs.pop_front() {
                return Ok(Some(output));
            }

            if self.draining_state.input_exhausted {
                self.collect_drained_outputs()?;

                if let Some(output) = self.pending_outputs.pop_front() {
                    return Ok(Some(output));
                }

                if self.has_fully_drained() && !self.draining_state.end_of_stream_emitted {
                    self.draining_state.end_of_stream_emitted = true;
                    return Ok(Some(DecodedOutput::EndOfStream));
                }

                return Ok(None);
            }

            let Some(media_packet) = self.read_next_packet()? else {
                self.enter_draining_mode()?;
                continue;
            };

            if self
                .video_decoder
                .as_ref()
                .map(|decoder| decoder.index == media_packet.stream_index)
                .unwrap_or(false)
            {
                let video_decoder = self.video_decoder.as_mut().expect("video decoder exists");
                decode_video_packet(video_decoder, &media_packet.packet, &mut self.pending_outputs)?;
                continue;
            }

            if self
                .audio_decoder
                .as_ref()
                .map(|decoder| decoder.index == media_packet.stream_index)
                .unwrap_or(false)
            {
                let audio_decoder = self.audio_decoder.as_mut().expect("audio decoder exists");
                decode_audio_packet(audio_decoder, &media_packet.packet, &mut self.pending_outputs)?;
                continue;
            }
        }
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

    fn collect_drained_outputs(&mut self) -> Result<(), MediaOpenError> {
        if !self.draining_state.video_drained {
            if let Some(video_decoder) = self.video_decoder.as_mut() {
                self.draining_state.video_drained =
                    collect_video_frames(video_decoder, &mut self.pending_outputs, true)?;
            } else {
                self.draining_state.video_drained = true;
            }
        }

        if !self.draining_state.audio_drained {
            if let Some(audio_decoder) = self.audio_decoder.as_mut() {
                self.draining_state.audio_drained =
                    collect_audio_frames(audio_decoder, &mut self.pending_outputs, true)?;
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
) -> Result<(), MediaOpenError> {
    loop {
        match decoder.decoder.send_packet(packet) {
            Ok(()) => {
                let _ = collect_video_frames(decoder, outputs, false)?;
                return Ok(());
            }
            Err(ffmpeg::Error::Other { errno }) if errno == ffmpeg::error::EAGAIN => {
                let output_count_before = outputs.len();
                let drained = collect_video_frames(decoder, outputs, false)?;
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
) -> Result<(), MediaOpenError> {
    loop {
        match decoder.decoder.send_packet(packet) {
            Ok(()) => {
                let _ = collect_audio_frames(decoder, outputs, false)?;
                return Ok(());
            }
            Err(ffmpeg::Error::Other { errno }) if errno == ffmpeg::error::EAGAIN => {
                let output_count_before = outputs.len();
                let drained = collect_audio_frames(decoder, outputs, false)?;
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
) -> Result<bool, MediaOpenError> {
    let mut reached_decoder_eof = false;

    loop {
        let mut frame = frame::Video::empty();
        match decoder.decoder.receive_frame(&mut frame) {
            Ok(()) => outputs.push_back(DecodedOutput::Video(map_video_frame(decoder, &frame)?)),
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
) -> Result<bool, MediaOpenError> {
    let mut reached_decoder_eof = false;

    loop {
        let mut frame = frame::Audio::empty();
        match decoder.decoder.receive_frame(&mut frame) {
            Ok(()) => outputs.push_back(DecodedOutput::Audio(map_audio_frame(decoder, &frame)?)),
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
        Ok(()) => Ok(()),
        Err(ffmpeg::Error::Eof) => Ok(()),
        Err(error) => Err(MediaOpenError::SendPacket(error)),
    }
}

fn send_audio_decoder_eof(decoder: &mut ffmpeg::decoder::Audio) -> Result<(), MediaOpenError> {
    match decoder.send_eof() {
        Ok(()) => Ok(()),
        Err(ffmpeg::Error::Eof) => Ok(()),
        Err(error) => Err(MediaOpenError::SendPacket(error)),
    }
}

fn map_video_frame(
    decoder: &mut OpenedVideoDecoder,
    frame: &frame::Video,
) -> Result<VideoFrame, MediaOpenError> {
    let time_base = decoder.decoder.packet_time_base();
    let pts_us = frame_timestamp_us(frame.pts().or_else(|| frame.timestamp()), time_base);
    let duration_us = frame_duration_us(frame.packet().duration, time_base);
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
) -> Result<AudioFrame, MediaOpenError> {
    let time_base = decoder.decoder.packet_time_base();
    let pts_us = frame_timestamp_us(frame.pts().or_else(|| frame.timestamp()), time_base);
    let duration_us = audio_duration_us(frame);

    decoder
        .resampler
        .convert(&decoder.decoder, frame, pts_us, duration_us)
}

fn frame_timestamp_us(timestamp: Option<i64>, time_base: Rational) -> MediaTimeUs {
    timestamp
        .map(|value| value.rescale(time_base, (1, 1_000_000)))
        .unwrap_or(0)
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
    Some(samples.saturating_mul(1_000_000).saturating_div(i64::from(frame.rate())))
}

fn map_pixel_format(pixel: format::Pixel) -> PixelFormatCategory {
    match pixel {
        format::Pixel::YUV420P => PixelFormatCategory::Yuv420p,
        format::Pixel::NV12 => PixelFormatCategory::Nv12,
        format::Pixel::RGBA => PixelFormatCategory::Rgba8,
        format::Pixel::BGRA => PixelFormatCategory::Bgra8,
        format::Pixel::GRAY8 => PixelFormatCategory::Gray8,
        _ => PixelFormatCategory::Unknown,
    }
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
        .map(|scaler| {
            scaler.input().format != input.format()
                || scaler.input().width != input.width()
                || scaler.input().height != input.height()
                || scaler.output().format != format::Pixel::BGRA
                || scaler.output().width != input.width()
                || scaler.output().height != input.height()
        })
        .unwrap_or(true);

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
