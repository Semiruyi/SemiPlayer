use std::ffi::c_double;
use std::sync::atomic::{AtomicU32, Ordering};

use crate::api::types::PlayerState;
use crate::audio::core::clock::AudioClock;
use crate::render::core::scheduler::VideoScheduler;
use crate::util::time::MediaTimeUs;

#[repr(C)]
pub struct SemiPlayerHandle {
    state: AtomicU32,
    pub(crate) speed: c_double,
    pub(crate) duration_us: MediaTimeUs,
    pub(crate) media_path: Option<String>,
    pub(crate) subtitles_visible: bool,
    pub(crate) video_presentation_bias_us: MediaTimeUs,
    pub(crate) audio_clock: AudioClock,
    pub(crate) video_scheduler: VideoScheduler,
}

impl SemiPlayerHandle {
    pub fn new() -> Self {
        Self {
            state: AtomicU32::new(PlayerState::Idle.as_raw()),
            speed: 1.0,
            duration_us: 0,
            media_path: None,
            subtitles_visible: true,
            video_presentation_bias_us: 0,
            audio_clock: AudioClock::new(),
            video_scheduler: VideoScheduler::new(),
        }
    }

    pub fn is_media_loaded(&self) -> bool {
        self.media_path.is_some()
    }

    pub fn reset_runtime_state(&mut self) {
        self.speed = 1.0;
        self.subtitles_visible = true;
        self.video_presentation_bias_us = 0;
        self.audio_clock.reset();
        self.video_scheduler = VideoScheduler::new();
    }

    pub fn set_state(&self, state: PlayerState) {
        self.state.store(state.as_raw(), Ordering::SeqCst);
    }

    pub fn state(&self) -> PlayerState {
        PlayerState::from_raw(self.state.load(Ordering::SeqCst)).unwrap_or(PlayerState::Idle)
    }
}
