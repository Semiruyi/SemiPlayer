use ffmpeg_next as ffmpeg;
use ffmpeg_next::{format, frame, Packet, Rational, Rescale};

use crate::audio::core::frame::{AudioFrame, AudioSampleFormatCategory};
use crate::render::core::frame::{PixelFormatCategory, VideoFrame};
use crate::util::time::MediaTimeUs;

use super::probe::{collect_media_info, MediaInfo, MediaProbeError};

pub struct OpenedMedia {
    path: String,
    input: ffmpeg::format::context::Input,
    info: MediaInfo,
    video_decoder: Option<OpenedVideoDecoder>,
    audio_decoder: Option<OpenedAudioDecoder>,
}

pub struct OpenedVideoDecoder {
    pub index: usize,
    pub decoder: ffmpeg::decoder::Video,
}

pub struct OpenedAudioDecoder {
    pub index: usize,
    pub decoder: ffmpeg::decoder::Audio,
}

#[derive(Debug)]
pub enum MediaOpenError {
    Probe(MediaProbeError),
    VideoDecoder(ffmpeg::Error),
    AudioDecoder(ffmpeg::Error),
    ReadPacket(ffmpeg::Error),
    SendPacket(ffmpeg::Error),
    ReceiveFrame(ffmpeg::Error),
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
            let Some(media_packet) = self.read_next_packet()? else {
                return Ok(Some(DecodedOutput::EndOfStream));
            };

            if self
                .video_decoder
                .as_ref()
                .map(|decoder| decoder.index == media_packet.stream_index)
                .unwrap_or(false)
            {
                let video_decoder = self.video_decoder.as_mut().expect("video decoder exists");
                if let Some(output) = decode_video_packet(video_decoder, &media_packet.packet)? {
                    return Ok(Some(output));
                }
                continue;
            }

            if self
                .audio_decoder
                .as_ref()
                .map(|decoder| decoder.index == media_packet.stream_index)
                .unwrap_or(false)
            {
                let audio_decoder = self.audio_decoder.as_mut().expect("audio decoder exists");
                if let Some(output) = decode_audio_packet(audio_decoder, &media_packet.packet)? {
                    return Ok(Some(output));
                }
                continue;
            }
        }
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
    }))
}

fn decode_video_packet(
    decoder: &mut OpenedVideoDecoder,
    packet: &Packet,
) -> Result<Option<DecodedOutput>, MediaOpenError> {
    match decoder.decoder.send_packet(packet) {
        Ok(()) => {}
        Err(ffmpeg::Error::Other { errno }) if errno == ffmpeg::error::EAGAIN => {}
        Err(error) => return Err(MediaOpenError::SendPacket(error)),
    }

    let mut frame = frame::Video::empty();
    match decoder.decoder.receive_frame(&mut frame) {
        Ok(()) => Ok(Some(DecodedOutput::Video(map_video_frame(
            &decoder.decoder,
            &frame,
        )))),
        Err(ffmpeg::Error::Other { errno }) if errno == ffmpeg::error::EAGAIN => Ok(None),
        Err(error) => Err(MediaOpenError::ReceiveFrame(error)),
    }
}

fn decode_audio_packet(
    decoder: &mut OpenedAudioDecoder,
    packet: &Packet,
) -> Result<Option<DecodedOutput>, MediaOpenError> {
    match decoder.decoder.send_packet(packet) {
        Ok(()) => {}
        Err(ffmpeg::Error::Other { errno }) if errno == ffmpeg::error::EAGAIN => {}
        Err(error) => return Err(MediaOpenError::SendPacket(error)),
    }

    let mut frame = frame::Audio::empty();
    match decoder.decoder.receive_frame(&mut frame) {
        Ok(()) => Ok(Some(DecodedOutput::Audio(map_audio_frame(
            &decoder.decoder,
            &frame,
        )))),
        Err(ffmpeg::Error::Other { errno }) if errno == ffmpeg::error::EAGAIN => Ok(None),
        Err(error) => Err(MediaOpenError::ReceiveFrame(error)),
    }
}

fn map_video_frame(
    decoder: &ffmpeg::decoder::Video,
    frame: &frame::Video,
) -> VideoFrame {
    let time_base = decoder.packet_time_base();
    let pts_us = frame_timestamp_us(frame.pts().or_else(|| frame.timestamp()), time_base);
    let duration_us = frame_duration_us(frame.packet().duration, time_base);

    VideoFrame {
        pts_us,
        duration_us,
        width: frame.width(),
        height: frame.height(),
        pixel_format: map_pixel_format(frame.format()),
        is_key_frame: frame.is_key(),
    }
}

fn map_audio_frame(
    decoder: &ffmpeg::decoder::Audio,
    frame: &frame::Audio,
) -> AudioFrame {
    let time_base = decoder.packet_time_base();
    let pts_us = frame_timestamp_us(frame.pts().or_else(|| frame.timestamp()), time_base);
    let duration_us = audio_duration_us(frame);

    AudioFrame {
        pts_us,
        duration_us,
        sample_rate: frame.rate(),
        channels: frame.channels(),
        sample_count: frame.samples(),
        sample_format: map_sample_format(frame.format()),
        is_planar: frame.is_planar(),
    }
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

fn map_sample_format(sample: format::Sample) -> AudioSampleFormatCategory {
    match sample {
        format::Sample::U8(_) => AudioSampleFormatCategory::U8,
        format::Sample::I16(_) => AudioSampleFormatCategory::I16,
        format::Sample::I32(_) => AudioSampleFormatCategory::I32,
        format::Sample::I64(_) => AudioSampleFormatCategory::I64,
        format::Sample::F32(_) => AudioSampleFormatCategory::F32,
        format::Sample::F64(_) => AudioSampleFormatCategory::F64,
        format::Sample::None => AudioSampleFormatCategory::Unknown,
    }
}
