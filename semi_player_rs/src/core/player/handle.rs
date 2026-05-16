use std::ffi::c_double;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::Mutex;

use crate::api::types::PlayerState;
use crate::audio::core::clock::AudioClock;
use crate::audio::core::output_controller::AudioOutputController;
use crate::core::media::OpenedMedia;
use crate::core::player::runtime::PlayerRuntime;
use crate::core::player::sync_worker::SyncWorkerHandle;
use crate::core::player::video_sync::VideoSyncState;
use crate::render::core::scheduler::VideoScheduler;
use crate::util::time::MediaTimeUs;

#[repr(C)]
pub struct SemiPlayerHandle {
    state: AtomicU32,
    op_lock: Mutex<()>,
    sync_worker: Option<SyncWorkerHandle>,
    pub(crate) speed: c_double,
    pub(crate) opened_media: Option<OpenedMedia>,
    pub(crate) subtitles_visible: bool,
    pub(crate) host_presentation_offset_us: MediaTimeUs,
    pub(crate) audio_clock: AudioClock,
    pub(crate) audio_output: AudioOutputController,
    pub(crate) video_scheduler: VideoScheduler,
    pub(crate) runtime: PlayerRuntime,
    pub(crate) video_sync: VideoSyncState,
}

impl SemiPlayerHandle {
    pub fn new() -> Self {
        Self {
            state: AtomicU32::new(PlayerState::Idle.as_raw()),
            op_lock: Mutex::new(()),
            sync_worker: None,
            speed: 1.0,
            opened_media: None,
            subtitles_visible: true,
            host_presentation_offset_us: 0,
            audio_clock: AudioClock::new(),
            audio_output: AudioOutputController::new(),
            video_scheduler: VideoScheduler::new(),
            runtime: PlayerRuntime::new(),
            video_sync: VideoSyncState::default(),
        }
    }

    pub fn is_media_loaded(&self) -> bool {
        self.opened_media.is_some()
    }

    pub unsafe fn with_locked_ptr<T>(
        player_ptr: *mut SemiPlayerHandle,
        f: impl FnOnce(&mut SemiPlayerHandle) -> T,
    ) -> T {
        let _guard = (&*player_ptr).op_lock.lock().unwrap();
        f(&mut *player_ptr)
    }

    pub fn start_sync_worker(&mut self, player_ptr: *mut SemiPlayerHandle) {
        if self.sync_worker.is_some() {
            return;
        }

        self.sync_worker = Some(SyncWorkerHandle::start(player_ptr));
    }

    pub fn notify_sync_worker(&self) {
        if let Some(sync_worker) = self.sync_worker.as_ref() {
            sync_worker.notify();
        }
    }

    pub fn stop_sync_worker(&mut self) {
        if let Some(mut sync_worker) = self.sync_worker.take() {
            sync_worker.stop();
        }
    }

    pub fn reset_runtime_state(&mut self) {
        self.speed = 1.0;
        self.subtitles_visible = true;
        self.host_presentation_offset_us = 0;
        self.audio_clock.reset();
        self.audio_output.stop();
        self.video_scheduler = VideoScheduler::new();
        self.runtime.clear();
        self.video_sync.reset();
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
