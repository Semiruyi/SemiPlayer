#[path = "media/decoder.rs"]
mod decoder;
#[path = "media/demux.rs"]
mod demux_impl;
#[path = "media/error.rs"]
mod error;
#[path = "media/keyframe_probe.rs"]
mod keyframe_probe;
#[path = "media/output.rs"]
mod output;
#[path = "media/probe.rs"]
mod probe;
#[path = "media/session.rs"]
mod session_impl;
#[path = "media/video_decode.rs"]
mod video_decode;

pub mod demux {
    pub use super::demux_impl::SeekDemuxDiagnosticsSnapshot;
    pub use super::keyframe_probe::{
        probe_expected_left_keyframe_pts, probe_expected_right_keyframe_pts,
    };
    pub use super::probe::{MediaInfo, MediaProbeError, StreamKind};
}

pub mod decode {
    pub use super::output::{
        DecodePolicy, DecodedOutput, DecodedOutputPoll, SeekRecoveryPolicy, SkippedAudioFrame,
        SkippedVideoFrame,
    };
    pub use super::video_decode::{
        VideoDecodeBackend, VideoDecodeDiagnosticsSnapshot, VideoDecodeFallbackReason,
    };
}

pub mod session {
    #[allow(unused_imports)]
    pub use super::session_impl::{
        open_media_with_hw_device_ctx, MediaSession, OpenedMedia, SharedMediaSession,
        SharedOpenedMedia,
    };
}

#[allow(unused_imports)]
pub use decode::{
    DecodePolicy, DecodedOutput, DecodedOutputPoll, SeekRecoveryPolicy, SkippedAudioFrame,
    SkippedVideoFrame,
};
#[allow(unused_imports)]
pub use demux::SeekDemuxDiagnosticsSnapshot;
pub use error::MediaOpenError;
#[allow(unused_imports)]
pub use demux::{
    probe_expected_left_keyframe_pts, probe_expected_right_keyframe_pts, MediaInfo,
    MediaProbeError, StreamKind,
};
#[allow(unused_imports)]
pub use session::{open_media_with_hw_device_ctx, SharedOpenedMedia};
#[allow(unused_imports)]
pub use decode::{
    VideoDecodeBackend, VideoDecodeDiagnosticsSnapshot, VideoDecodeFallbackReason,
};
