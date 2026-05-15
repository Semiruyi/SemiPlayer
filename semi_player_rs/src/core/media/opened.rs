use ffmpeg_next as ffmpeg;

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
    Seek(ffmpeg::Error),
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
        self.input.seek(position_us, ..).map_err(MediaOpenError::Seek)?;
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
    let decoder = context.decoder().video().map_err(MediaOpenError::VideoDecoder)?;

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
    let decoder = context.decoder().audio().map_err(MediaOpenError::AudioDecoder)?;

    Ok(Some(OpenedAudioDecoder {
        index: stream_index,
        decoder,
    }))
}
