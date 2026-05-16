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
    pub video_frame_rate_num: u32,
    pub video_frame_rate_den: u32,
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
    pub audio_position_ms: i64,
    pub audio_queue_len: u32,
    pub video_queue_len: u32,
    pub has_current_video_frame: u32,
    pub current_video_pts_ms: i64,
    pub current_video_duration_ms: i64,
    pub current_video_effective_end_ms: i64,
    pub next_video_pts_ms: i64,
    pub current_to_next_video_delta_ms: i64,
    pub next_video_wake_deadline_ms: i64,
    pub last_audio_pts_ms: i64,
    pub host_presentation_offset_ms: i32,
    pub core_av_delta_ms: i64,
    pub core_sync_error_ms: i64,
    pub expected_end_to_end_av_delta_ms: i64,
    pub video_sync_ticks: u64,
    pub video_sync_runs: u64,
    pub video_sync_presents: u64,
    pub video_sync_drops: u64,
    pub video_sync_underflows: u64,
    pub video_sync_late_hits: u64,
    pub last_sync_presented_frames: u64,
    pub last_sync_dropped_frames: u64,
    pub max_sync_presented_frames: u64,
    pub max_sync_dropped_frames: u64,
    pub sync_run_present_only_count: u64,
    pub sync_run_drop_only_count: u64,
    pub sync_run_present_drop_count: u64,
    pub sync_run_other_count: u64,
    pub suggested_pump_wait_ms: i64,
    pub next_audio_refill_deadline_ms: i64,
    pub next_pump_deadline_ms: i64,
    pub ffi_lock_wait_last_us: i64,
    pub ffi_lock_wait_max_us: i64,
    pub worker_lock_wait_last_us: i64,
    pub worker_lock_wait_max_us: i64,
    pub worker_deadline_slip_last_us: i64,
    pub worker_deadline_slip_max_us: i64,
    pub stale_audio_discard_event_count: u64,
    pub stale_audio_discard_frame_count: u64,
    pub stale_audio_discard_last_frame_count: u64,
    pub stale_audio_discard_last_lag_us: i64,
    pub stale_audio_discard_max_lag_us: i64,
    pub audio_output_started: u32,
    pub pending_device_frames: u32,
    pub rendered_frames_total: u64,
    pub audible_frames_total: u64,
    pub end_of_stream: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct SemiAudioOutputSnapshot {
    pub configured_sample_rate: u32,
    pub configured_channels: u16,
    pub reserved0: u16,
    pub target_buffer_frames: u32,
    pub buffered_frames: u32,
    pub pending_device_frames: u32,
    pub rendered_frames_total: u64,
    pub audible_frames_total: u64,
    pub submitted_frames_total: u64,
    pub started: u32,
    pub has_device_timing: u32,
    pub base_pts_ms: i64,
    pub device_played_frames: u64,
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
