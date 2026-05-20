pub(crate) mod decoder;
mod error;
pub(crate) mod output;
mod policy;
#[path = "session/decode.rs"]
pub(crate) mod session_decode;
#[path = "session/mod.rs"]
pub(crate) mod session_impl;
#[path = "session/lifecycle.rs"]
pub(crate) mod session_lifecycle;
#[path = "session/shared.rs"]
pub(crate) mod session_shared;
#[path = "video.rs"]
pub(crate) mod video_decode;

pub mod session {
    #[allow(unused_imports)]
    pub use super::session_impl::SharedOpenedMedia;
    #[allow(unused_imports)]
    pub use super::session_impl::{
        open_media_with_hw_device_ctx, open_media_with_request,
        open_media_with_video_decode_requirements, MediaOpenRequest, MediaSession, OpenedMedia,
    };
    #[allow(unused_imports)]
    pub use super::session_shared::SharedMediaSession;
}

pub use error::MediaOpenError;
#[allow(unused_imports)]
pub use output::{
    DecodePolicy, DecodedOutput, DecodedOutputPoll, SeekRecoveryPolicy, SkippedAudioFrame,
    SkippedVideoFrame,
};
#[allow(unused_imports)]
pub use policy::{DecodePreference, VideoDecodeRequirements};
pub use video_decode::VideoDecodeDiagnosticsSnapshot;
