use ffmpeg_next as ffmpeg;

use crate::util::time::MediaTimeUs;

#[derive(Clone, Debug)]
pub struct MediaInfo {
    pub duration_us: Option<MediaTimeUs>,
    pub best_video_stream_index: Option<usize>,
    pub best_audio_stream_index: Option<usize>,
    pub best_subtitle_stream_index: Option<usize>,
    pub streams: Vec<StreamInfo>,
}

#[derive(Clone, Debug)]
pub struct StreamInfo {
    pub index: usize,
    pub kind: StreamKind,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum StreamKind {
    Unknown,
    Video,
    Audio,
    Subtitle,
    Data,
    Attachment,
}

#[derive(Debug)]
pub enum MediaProbeError {
    FfmpegInit(ffmpeg::Error),
    OpenInput(ffmpeg::Error),
    Decoder(ffmpeg::Error),
}

pub fn probe_media(path: &str) -> Result<MediaInfo, MediaProbeError> {
    ffmpeg::init().map_err(MediaProbeError::FfmpegInit)?;

    let context = ffmpeg::format::input(&path).map_err(MediaProbeError::OpenInput)?;

    let best_video_stream_index = context
        .streams()
        .best(ffmpeg::media::Type::Video)
        .map(|stream| stream.index());
    let best_audio_stream_index = context
        .streams()
        .best(ffmpeg::media::Type::Audio)
        .map(|stream| stream.index());
    let best_subtitle_stream_index = context
        .streams()
        .best(ffmpeg::media::Type::Subtitle)
        .map(|stream| stream.index());

    let mut streams = Vec::new();
    for stream in context.streams() {
        let codec =
            ffmpeg::codec::context::Context::from_parameters(stream.parameters()).map_err(
                MediaProbeError::Decoder,
            )?;

        streams.push(StreamInfo {
            index: stream.index(),
            kind: map_stream_kind(codec.medium()),
        });
    }

    Ok(MediaInfo {
        duration_us: format_duration_to_us(context.duration()),
        best_video_stream_index,
        best_audio_stream_index,
        best_subtitle_stream_index,
        streams,
    })
}

fn map_stream_kind(kind: ffmpeg::media::Type) -> StreamKind {
    match kind {
        ffmpeg::media::Type::Video => StreamKind::Video,
        ffmpeg::media::Type::Audio => StreamKind::Audio,
        ffmpeg::media::Type::Subtitle => StreamKind::Subtitle,
        ffmpeg::media::Type::Data => StreamKind::Data,
        ffmpeg::media::Type::Attachment => StreamKind::Attachment,
        ffmpeg::media::Type::Unknown => StreamKind::Unknown,
    }
}

fn format_duration_to_us(duration: i64) -> Option<MediaTimeUs> {
    if duration <= 0 {
        return None;
    }

    let time_base = i64::from(ffmpeg::ffi::AV_TIME_BASE);
    Some(duration.saturating_mul(1_000_000).saturating_div(time_base))
}
