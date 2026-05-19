use crate::audio::core::frame::AudioFrame;
use crate::render::core::frame::DecodedVideoFrame;
use crate::util::time::MediaTimeUs;

pub enum DecodedOutput {
    Video(DecodedVideoFrame),
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
