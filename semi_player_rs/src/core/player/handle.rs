use std::ffi::c_double;
use std::sync::atomic::{AtomicU32, Ordering};

use crate::api::types::PlayerState;
use crate::audio::core::clock::AudioClock;
use crate::audio::core::output_controller::AudioOutputController;
use crate::core::media::OpenedMedia;
use crate::core::player::runtime::PlayerRuntime;
use crate::render::core::scheduler::VideoScheduler;
use crate::util::time::MediaTimeUs;

#[repr(C)]
pub struct SemiPlayerHandle {
    state: AtomicU32,
    pub(crate) speed: c_double,
    pub(crate) opened_media: Option<OpenedMedia>,
    pub(crate) subtitles_visible: bool,
    pub(crate) host_presentation_offset_us: MediaTimeUs,
    pub(crate) audio_clock: AudioClock,
    pub(crate) audio_output: AudioOutputController,
    pub(crate) video_scheduler: VideoScheduler,
    pub(crate) runtime: PlayerRuntime,
}

impl SemiPlayerHandle {
    pub fn new() -> Self {
        Self {
            state: AtomicU32::new(PlayerState::Idle.as_raw()),
            speed: 1.0,
            opened_media: None,
            subtitles_visible: true,
            host_presentation_offset_us: 0,
            audio_clock: AudioClock::new(),
            audio_output: AudioOutputController::new(),
            video_scheduler: VideoScheduler::new(),
            runtime: PlayerRuntime::new(),
        }
    }

    pub fn is_media_loaded(&self) -> bool {
        self.opened_media.is_some()
    }

    pub fn reset_runtime_state(&mut self) {
        self.speed = 1.0;
        self.subtitles_visible = true;
        self.host_presentation_offset_us = 0;
        self.audio_clock.reset();
        self.audio_output.stop();
        self.video_scheduler = VideoScheduler::new();
        self.runtime.clear();
    }

    pub fn clear_media(&mut self) {
        self.opened_media = None;
        self.reset_runtime_state();
    }

    pub fn set_state(&self, state: PlayerState) {
        self.state.store(state.as_raw(), Ordering::SeqCst);
    }

    pub fn state(&self) -> PlayerState {
        PlayerState::from_raw(self.state.load(Ordering::SeqCst)).unwrap_or(PlayerState::Idle)
    }
}
