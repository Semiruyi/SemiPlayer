use std::ffi::c_double;
use std::sync::atomic::{AtomicU32, Ordering};

use crate::api::types::PlayerState;
use crate::util::time::MediaTimeUs;

#[repr(C)]
pub struct SemiPlayerHandle {
    state: AtomicU32,
    pub(crate) speed: c_double,
    pub(crate) position_us: MediaTimeUs,
    pub(crate) duration_us: MediaTimeUs,
    pub(crate) media_path: Option<String>,
    pub(crate) subtitles_visible: bool,
}

impl SemiPlayerHandle {
    pub fn new() -> Self {
        Self {
            state: AtomicU32::new(PlayerState::Idle.as_raw()),
            speed: 1.0,
            position_us: 0,
            duration_us: 0,
            media_path: None,
            subtitles_visible: true,
        }
    }

    pub fn is_media_loaded(&self) -> bool {
        self.media_path.is_some()
    }

    pub fn set_state(&self, state: PlayerState) {
        self.state.store(state.as_raw(), Ordering::SeqCst);
    }

    pub fn state(&self) -> PlayerState {
        PlayerState::from_raw(self.state.load(Ordering::SeqCst)).unwrap_or(PlayerState::Idle)
    }
}
