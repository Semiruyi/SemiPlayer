#[repr(u32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum PlayerState {
    Idle = 0,
    Ready = 1,
    Playing = 2,
    Paused = 3,
}

impl PlayerState {
    pub const fn as_raw(self) -> u32 {
        self as u32
    }

    pub const fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            0 => Some(Self::Idle),
            1 => Some(Self::Ready),
            2 => Some(Self::Playing),
            3 => Some(Self::Paused),
            _ => None,
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct SemiMediaInfo {
    pub duration_ms: i64,
    pub stream_count: u32,
    pub video_stream_count: u32,
    pub audio_stream_count: u32,
    pub subtitle_stream_count: u32,
    pub best_video_stream_index: i32,
    pub best_audio_stream_index: i32,
    pub best_subtitle_stream_index: i32,
    pub video_width: u32,
    pub video_height: u32,
    pub audio_sample_rate: u32,
    pub audio_channels: u16,
    pub reserved0: u16,
}

#[repr(u32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SemiDecodedKind {
    None = 0,
    Video = 1,
    Audio = 2,
    EndOfStream = 3,
}

impl SemiDecodedKind {
    pub const fn as_raw(self) -> u32 {
        self as u32
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct SemiDecodedOutput {
    pub kind: u32,
    pub pts_ms: i64,
    pub duration_ms: i64,
    pub width: u32,
    pub height: u32,
    pub sample_rate: u32,
    pub channels: u16,
    pub sample_count: u32,
    pub flags: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct SemiPlaybackSnapshot {
    pub audio_queue_len: u32,
    pub video_queue_len: u32,
    pub has_current_video_frame: u32,
    pub current_video_pts_ms: i64,
    pub current_video_duration_ms: i64,
    pub last_audio_pts_ms: i64,
    pub end_of_stream: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct SemiVideoFrameInfo {
    pub pts_ms: i64,
    pub duration_ms: i64,
    pub width: u32,
    pub height: u32,
    pub stride: u32,
    pub pixel_format: u32,
    pub byte_len: u32,
    pub flags: u32,
}
