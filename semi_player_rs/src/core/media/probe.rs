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
    pub video: Option<VideoStreamInfo>,
    pub audio: Option<AudioStreamInfo>,
}

#[derive(Clone, Copy, Debug)]
pub struct VideoStreamInfo {
    pub width: u32,
    pub height: u32,
    pub avg_frame_rate_num: u32,
    pub avg_frame_rate_den: u32,
}

#[derive(Clone, Copy, Debug)]
pub struct AudioStreamInfo {
    pub sample_rate: u32,
    pub channels: u16,
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
    collect_media_info(&context)
}

pub(crate) fn collect_media_info(
    context: &ffmpeg::format::context::Input,
) -> Result<MediaInfo, MediaProbeError> {
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
        let codec = ffmpeg::codec::context::Context::from_parameters(stream.parameters())
            .map_err(MediaProbeError::Decoder)?;
        let kind = map_stream_kind(codec.medium());
        let (video, audio) = match kind {
            StreamKind::Video => (
                codec.decoder().video().ok().map(|video| VideoStreamInfo {
                    width: video.width(),
                    height: video.height(),
                    avg_frame_rate_num: u32::try_from(stream.avg_frame_rate().numerator())
                        .unwrap_or(0),
                    avg_frame_rate_den: u32::try_from(stream.avg_frame_rate().denominator())
                        .unwrap_or(0),
                }),
                None,
            ),
            StreamKind::Audio => (
                None,
                codec.decoder().audio().ok().map(|audio| AudioStreamInfo {
                    sample_rate: audio.rate(),
                    channels: audio.channels(),
                }),
            ),
            _ => (None, None),
        };

        streams.push(StreamInfo {
            index: stream.index(),
            kind,
            video,
            audio,
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

impl MediaInfo {
    pub fn stream_count(&self) -> u32 {
        self.streams.len() as u32
    }

    pub fn video_stream_count(&self) -> u32 {
        self.streams
            .iter()
            .filter(|stream| stream.kind == StreamKind::Video)
            .count() as u32
    }

    pub fn audio_stream_count(&self) -> u32 {
        self.streams
            .iter()
            .filter(|stream| stream.kind == StreamKind::Audio)
            .count() as u32
    }

    pub fn subtitle_stream_count(&self) -> u32 {
        self.streams
            .iter()
            .filter(|stream| stream.kind == StreamKind::Subtitle)
            .count() as u32
    }

    pub fn best_video_stream(&self) -> Option<&StreamInfo> {
        find_stream(self, self.best_video_stream_index)
    }

    pub fn best_audio_stream(&self) -> Option<&StreamInfo> {
        find_stream(self, self.best_audio_stream_index)
    }
}

fn find_stream(media_info: &MediaInfo, index: Option<usize>) -> Option<&StreamInfo> {
    let index = index?;
    media_info
        .streams
        .iter()
        .find(|stream| stream.index == index)
}
