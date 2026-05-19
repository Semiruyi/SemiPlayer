use std::error::Error;
use std::fmt;

use ffmpeg_next as ffmpeg;

use crate::demux::MediaProbeError;

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
