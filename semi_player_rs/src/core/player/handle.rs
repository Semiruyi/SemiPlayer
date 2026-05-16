use std::ffi::c_double;
use std::sync::atomic::{AtomicI64, AtomicU32, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Instant;

use crate::api::types::PlayerState;
use crate::audio::core::clock::AudioClock;
use crate::audio::core::output_controller::SharedAudioOutputController;
use crate::core::media::SharedOpenedMedia;
use crate::core::player::decode_worker::DecodeWorkerHandle;
use crate::core::player::runtime::{AudioDiscardSummary, PlayerRuntime};
use crate::core::player::schedule::PlayerScheduleService;
use crate::core::player::sync_worker::SyncWorkerHandle;
use crate::core::player::video_sync::VideoSyncState;
use crate::render::core::scheduler::VideoScheduler;
use crate::util::time::MediaTimeUs;

#[derive(Clone, Copy, Debug, Default)]
pub struct PlayerDiagnosticsSnapshot {
    pub ffi_lock_wait_last_us: MediaTimeUs,
    pub ffi_lock_wait_max_us: MediaTimeUs,
    pub sync_worker_lock_wait_last_us: MediaTimeUs,
    pub sync_worker_lock_wait_max_us: MediaTimeUs,
    pub decode_worker_lock_wait_last_us: MediaTimeUs,
    pub decode_worker_lock_wait_max_us: MediaTimeUs,
    pub worker_deadline_slip_last_us: MediaTimeUs,
    pub worker_deadline_slip_max_us: MediaTimeUs,
    pub stale_audio_discard_event_count: u64,
    pub stale_audio_discard_frame_count: u64,
    pub stale_audio_discard_last_frame_count: u64,
    pub stale_audio_discard_last_lag_us: MediaTimeUs,
    pub stale_audio_discard_max_lag_us: MediaTimeUs,
    pub seek_event_count: u64,
    pub seek_active: bool,
    pub last_seek_target_us: MediaTimeUs,
    pub seek_api_duration_us: MediaTimeUs,
    pub seek_lock_wait_us: MediaTimeUs,
    pub seek_ffmpeg_seek_us: MediaTimeUs,
    pub seek_reset_us: MediaTimeUs,
    pub seek_first_video_decoded_us: MediaTimeUs,
    pub seek_first_audio_decoded_us: MediaTimeUs,
    pub seek_target_video_ready_us: MediaTimeUs,
    pub seek_target_audio_ready_us: MediaTimeUs,
    pub seek_stable_us: MediaTimeUs,
}

#[derive(Default)]
struct PlayerDiagnostics {
    ffi_lock_wait_last_us: AtomicI64,
    ffi_lock_wait_max_us: AtomicI64,
    sync_worker_lock_wait_last_us: AtomicI64,
    sync_worker_lock_wait_max_us: AtomicI64,
    decode_worker_lock_wait_last_us: AtomicI64,
    decode_worker_lock_wait_max_us: AtomicI64,
    worker_deadline_slip_last_us: AtomicI64,
    worker_deadline_slip_max_us: AtomicI64,
    stale_audio_discard_event_count: AtomicU64,
    stale_audio_discard_frame_count: AtomicU64,
    stale_audio_discard_last_frame_count: AtomicU64,
    stale_audio_discard_last_lag_us: AtomicI64,
    stale_audio_discard_max_lag_us: AtomicI64,
    seek: Mutex<SeekDiagnosticsState>,
}

#[derive(Clone, Copy, Debug, Default)]
struct SeekDiagnosticsSnapshot {
    seek_event_count: u64,
    seek_active: bool,
    last_seek_target_us: MediaTimeUs,
    seek_api_duration_us: MediaTimeUs,
    seek_lock_wait_us: MediaTimeUs,
    seek_ffmpeg_seek_us: MediaTimeUs,
    seek_reset_us: MediaTimeUs,
    seek_first_video_decoded_us: MediaTimeUs,
    seek_first_audio_decoded_us: MediaTimeUs,
    seek_target_video_ready_us: MediaTimeUs,
    seek_target_audio_ready_us: MediaTimeUs,
    seek_stable_us: MediaTimeUs,
}

#[derive(Debug)]
struct SeekObservation {
    requested_at: Instant,
    target_us: MediaTimeUs,
    seek_api_duration_us: Option<MediaTimeUs>,
    seek_lock_wait_us: Option<MediaTimeUs>,
    seek_ffmpeg_seek_us: Option<MediaTimeUs>,
    seek_reset_us: Option<MediaTimeUs>,
    seek_first_video_decoded_us: Option<MediaTimeUs>,
    seek_first_audio_decoded_us: Option<MediaTimeUs>,
    seek_target_video_ready_us: Option<MediaTimeUs>,
    seek_target_audio_ready_us: Option<MediaTimeUs>,
    seek_stable_us: Option<MediaTimeUs>,
}

#[derive(Debug, Default)]
struct SeekDiagnosticsState {
    seek_event_count: u64,
    active: Option<SeekObservation>,
    last_completed: Option<SeekDiagnosticsSnapshot>,
}

#[derive(Clone, Copy, Debug)]
pub(crate) enum LockOwner {
    Ffi,
    SyncWorker,
    DecodeWorker,
}

#[repr(C)]
pub struct SemiPlayerHandle {
    state: AtomicU32,
    op_lock: Mutex<()>,
    playback_phase_lock: Arc<Mutex<()>>,
    sync_worker: Option<SyncWorkerHandle>,
    decode_worker: Option<DecodeWorkerHandle>,
    diagnostics: PlayerDiagnostics,
    media_generation: AtomicU64,
    pub(crate) speed: c_double,
    pub(crate) opened_media: Option<SharedOpenedMedia>,
    pub(crate) subtitles_visible: bool,
    pub(crate) host_presentation_offset_us: MediaTimeUs,
    pub(crate) audio_clock: AudioClock,
    pub(crate) audio_output: SharedAudioOutputController,
    pub(crate) video_scheduler: VideoScheduler,
    pub(crate) runtime: PlayerRuntime,
    pub(crate) video_sync: VideoSyncState,
}

impl SemiPlayerHandle {
    pub fn new() -> Self {
        Self {
            state: AtomicU32::new(PlayerState::Idle.as_raw()),
            op_lock: Mutex::new(()),
            playback_phase_lock: Arc::new(Mutex::new(())),
            sync_worker: None,
            decode_worker: None,
            diagnostics: PlayerDiagnostics::default(),
            media_generation: AtomicU64::new(0),
            speed: 1.0,
            opened_media: None,
            subtitles_visible: true,
            host_presentation_offset_us: 0,
            audio_clock: AudioClock::new(),
            audio_output: SharedAudioOutputController::default(),
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
        Self::with_locked_ptr_as(player_ptr, LockOwner::Ffi, f)
    }

    pub(crate) unsafe fn with_locked_ptr_as<T>(
        player_ptr: *mut SemiPlayerHandle,
        owner: LockOwner,
        f: impl FnOnce(&mut SemiPlayerHandle) -> T,
    ) -> T {
        let player_ref = &*player_ptr;
        let wait_start = Instant::now();
        let _guard = player_ref.op_lock.lock().unwrap();
        let wait_us = i64::try_from(wait_start.elapsed().as_micros()).unwrap_or(i64::MAX);
        player_ref.diagnostics.observe_lock_wait(owner, wait_us);
        f(&mut *player_ptr)
    }

    pub fn start_workers(&mut self, player_ptr: *mut SemiPlayerHandle) {
        if self.sync_worker.is_some() {
            return;
        }

        self.sync_worker = Some(SyncWorkerHandle::start(player_ptr));
        self.decode_worker = Some(DecodeWorkerHandle::start(player_ptr));
    }

    pub fn notify_workers(&self) {
        if let Some(sync_worker) = self.sync_worker.as_ref() {
            sync_worker.notify();
        }

        self.request_decode_if_needed();
    }

    pub fn stop_workers(&mut self) {
        if let Some(mut sync_worker) = self.sync_worker.take() {
            sync_worker.stop();
        }

        if let Some(mut decode_worker) = self.decode_worker.take() {
            decode_worker.stop();
        }
    }

    pub fn notify_sync_worker(&self) {
        if let Some(sync_worker) = self.sync_worker.as_ref() {
            sync_worker.notify();
        }
    }

    pub fn notify_decode_worker(&self) {
        self.request_decode_if_needed();
    }

    pub fn request_decode_if_needed(&self) {
        let hint = PlayerScheduleService::evaluate_decode(self);
        if !hint.should_decode_now {
            return;
        }

        if let Some(decode_worker) = self.decode_worker.as_ref() {
            decode_worker.request_decode();
        }
    }

    pub fn reset_runtime_state(&mut self) {
        self.speed = 1.0;
        self.subtitles_visible = true;
        self.host_presentation_offset_us = 0;
        self.audio_clock.reset();
        self.audio_output
            .with_mut(|audio_output| audio_output.stop());
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

    pub fn diagnostics_snapshot(&self) -> PlayerDiagnosticsSnapshot {
        self.diagnostics.snapshot()
    }

    pub fn playback_phase_lock(&self) -> Arc<Mutex<()>> {
        Arc::clone(&self.playback_phase_lock)
    }

    pub fn media_generation(&self) -> u64 {
        self.media_generation.load(Ordering::SeqCst)
    }

    pub fn bump_media_generation(&self) -> u64 {
        self.media_generation.fetch_add(1, Ordering::SeqCst) + 1
    }

    pub fn observe_worker_deadline_slip(&self, slip_us: MediaTimeUs) {
        self.diagnostics.observe_worker_deadline_slip(slip_us);
    }

    pub fn observe_stale_audio_discard(&self, discard: AudioDiscardSummary) {
        self.diagnostics.observe_stale_audio_discard(discard);
    }

    pub fn observe_seek_requested(&self, target_us: MediaTimeUs) {
        self.diagnostics.observe_seek_requested(target_us);
    }

    pub fn observe_seek_lock_acquired(&self) {
        self.diagnostics.observe_seek_lock_acquired();
    }

    pub fn observe_seek_ffmpeg_seek_started(&self) {
        self.diagnostics.observe_seek_ffmpeg_seek_started();
    }

    pub fn observe_seek_ffmpeg_seek_finished(&self) {
        self.diagnostics.observe_seek_ffmpeg_seek_finished();
    }

    pub fn observe_seek_reset_finished(&self) {
        self.diagnostics.observe_seek_reset_finished();
    }

    pub fn observe_seek_api_completed(&self) {
        self.diagnostics.observe_seek_api_completed();
    }

    pub fn observe_seek_aborted(&self) {
        self.diagnostics.observe_seek_aborted();
    }

    pub fn observe_seek_first_video_decoded(&self) {
        self.diagnostics.observe_seek_first_video_decoded();
    }

    pub fn observe_seek_first_audio_decoded(&self) {
        self.diagnostics.observe_seek_first_audio_decoded();
    }

    pub fn observe_seek_target_video_ready(&self) {
        self.diagnostics.observe_seek_target_video_ready();
    }

    pub fn observe_seek_target_audio_ready(&self) {
        self.diagnostics.observe_seek_target_audio_ready();
    }

    pub fn observe_seek_stable(&self) {
        self.diagnostics.observe_seek_stable();
    }
}

impl PlayerDiagnostics {
    fn observe_lock_wait(&self, owner: LockOwner, wait_us: MediaTimeUs) {
        match owner {
            LockOwner::Ffi => {
                self.ffi_lock_wait_last_us.store(wait_us, Ordering::Relaxed);
                update_atomic_max(&self.ffi_lock_wait_max_us, wait_us);
            }
            LockOwner::SyncWorker => {
                self.sync_worker_lock_wait_last_us
                    .store(wait_us, Ordering::Relaxed);
                update_atomic_max(&self.sync_worker_lock_wait_max_us, wait_us);
            }
            LockOwner::DecodeWorker => {
                self.decode_worker_lock_wait_last_us
                    .store(wait_us, Ordering::Relaxed);
                update_atomic_max(&self.decode_worker_lock_wait_max_us, wait_us);
            }
        }
    }

    fn observe_worker_deadline_slip(&self, slip_us: MediaTimeUs) {
        self.worker_deadline_slip_last_us
            .store(slip_us, Ordering::Relaxed);
        update_atomic_max(&self.worker_deadline_slip_max_us, slip_us);
    }

    fn observe_stale_audio_discard(&self, discard: AudioDiscardSummary) {
        if discard.removed_frames == 0 {
            return;
        }

        self.stale_audio_discard_event_count
            .fetch_add(1, Ordering::Relaxed);
        self.stale_audio_discard_frame_count.fetch_add(
            u64::try_from(discard.removed_frames).unwrap_or(u64::MAX),
            Ordering::Relaxed,
        );
        self.stale_audio_discard_last_frame_count.store(
            u64::try_from(discard.removed_frames).unwrap_or(u64::MAX),
            Ordering::Relaxed,
        );
        self.stale_audio_discard_last_lag_us
            .store(discard.max_lag_us, Ordering::Relaxed);
        update_atomic_max(&self.stale_audio_discard_max_lag_us, discard.max_lag_us);
    }

    fn observe_seek_requested(&self, target_us: MediaTimeUs) {
        let mut seek = self.seek.lock().unwrap();
        seek.seek_event_count = seek.seek_event_count.saturating_add(1);
        seek.active = Some(SeekObservation::new(target_us));
    }

    fn observe_seek_lock_acquired(&self) {
        self.with_active_seek(|seek| {
            seek.seek_lock_wait_us = Some(seek.elapsed_us());
        });
    }

    fn observe_seek_ffmpeg_seek_started(&self) {
        self.with_active_seek(|seek| {
            if seek.seek_lock_wait_us.is_none() {
                seek.seek_lock_wait_us = Some(seek.elapsed_us());
            }
        });
    }

    fn observe_seek_ffmpeg_seek_finished(&self) {
        self.with_active_seek(|seek| {
            seek.seek_ffmpeg_seek_us = Some(seek.elapsed_us());
        });
    }

    fn observe_seek_reset_finished(&self) {
        self.with_active_seek(|seek| {
            seek.seek_reset_us = Some(seek.elapsed_us());
        });
    }

    fn observe_seek_api_completed(&self) {
        self.with_active_seek(|seek| {
            seek.seek_api_duration_us = Some(seek.elapsed_us());
        });
    }

    fn observe_seek_aborted(&self) {
        let mut seek = self.seek.lock().unwrap();
        let Some(mut active) = seek.active.take() else {
            return;
        };

        if active.seek_api_duration_us.is_none() {
            active.seek_api_duration_us = Some(active.elapsed_us());
        }
        seek.last_completed = Some(active.snapshot(seek.seek_event_count, false));
    }

    fn observe_seek_first_video_decoded(&self) {
        self.with_active_seek(|seek| {
            if seek.seek_first_video_decoded_us.is_none() {
                seek.seek_first_video_decoded_us = Some(seek.elapsed_us());
            }
        });
    }

    fn observe_seek_first_audio_decoded(&self) {
        self.with_active_seek(|seek| {
            if seek.seek_first_audio_decoded_us.is_none() {
                seek.seek_first_audio_decoded_us = Some(seek.elapsed_us());
            }
        });
    }

    fn observe_seek_target_video_ready(&self) {
        self.with_active_seek(|seek| {
            if seek.seek_target_video_ready_us.is_none() {
                seek.seek_target_video_ready_us = Some(seek.elapsed_us());
            }
        });
    }

    fn observe_seek_target_audio_ready(&self) {
        self.with_active_seek(|seek| {
            if seek.seek_target_audio_ready_us.is_none() {
                seek.seek_target_audio_ready_us = Some(seek.elapsed_us());
            }
        });
    }

    fn observe_seek_stable(&self) {
        let mut seek = self.seek.lock().unwrap();
        let Some(mut active) = seek.active.take() else {
            return;
        };

        if active.seek_stable_us.is_none() {
            active.seek_stable_us = Some(active.elapsed_us());
        }

        seek.last_completed = Some(active.snapshot(seek.seek_event_count, false));
    }

    fn with_active_seek(&self, f: impl FnOnce(&mut SeekObservation)) {
        let mut seek = self.seek.lock().unwrap();
        if let Some(active) = seek.active.as_mut() {
            f(active);
        }
    }

    fn snapshot(&self) -> PlayerDiagnosticsSnapshot {
        let seek_snapshot = self.seek.lock().unwrap().snapshot();

        PlayerDiagnosticsSnapshot {
            ffi_lock_wait_last_us: self.ffi_lock_wait_last_us.load(Ordering::Relaxed),
            ffi_lock_wait_max_us: self.ffi_lock_wait_max_us.load(Ordering::Relaxed),
            sync_worker_lock_wait_last_us: self
                .sync_worker_lock_wait_last_us
                .load(Ordering::Relaxed),
            sync_worker_lock_wait_max_us: self.sync_worker_lock_wait_max_us.load(Ordering::Relaxed),
            decode_worker_lock_wait_last_us: self
                .decode_worker_lock_wait_last_us
                .load(Ordering::Relaxed),
            decode_worker_lock_wait_max_us: self
                .decode_worker_lock_wait_max_us
                .load(Ordering::Relaxed),
            worker_deadline_slip_last_us: self.worker_deadline_slip_last_us.load(Ordering::Relaxed),
            worker_deadline_slip_max_us: self.worker_deadline_slip_max_us.load(Ordering::Relaxed),
            stale_audio_discard_event_count: self
                .stale_audio_discard_event_count
                .load(Ordering::Relaxed),
            stale_audio_discard_frame_count: self
                .stale_audio_discard_frame_count
                .load(Ordering::Relaxed),
            stale_audio_discard_last_frame_count: self
                .stale_audio_discard_last_frame_count
                .load(Ordering::Relaxed),
            stale_audio_discard_last_lag_us: self
                .stale_audio_discard_last_lag_us
                .load(Ordering::Relaxed),
            stale_audio_discard_max_lag_us: self
                .stale_audio_discard_max_lag_us
                .load(Ordering::Relaxed),
            seek_event_count: seek_snapshot.seek_event_count,
            seek_active: seek_snapshot.seek_active,
            last_seek_target_us: seek_snapshot.last_seek_target_us,
            seek_api_duration_us: seek_snapshot.seek_api_duration_us,
            seek_lock_wait_us: seek_snapshot.seek_lock_wait_us,
            seek_ffmpeg_seek_us: seek_snapshot.seek_ffmpeg_seek_us,
            seek_reset_us: seek_snapshot.seek_reset_us,
            seek_first_video_decoded_us: seek_snapshot.seek_first_video_decoded_us,
            seek_first_audio_decoded_us: seek_snapshot.seek_first_audio_decoded_us,
            seek_target_video_ready_us: seek_snapshot.seek_target_video_ready_us,
            seek_target_audio_ready_us: seek_snapshot.seek_target_audio_ready_us,
            seek_stable_us: seek_snapshot.seek_stable_us,
        }
    }
}

impl SeekObservation {
    fn new(target_us: MediaTimeUs) -> Self {
        Self {
            requested_at: Instant::now(),
            target_us,
            seek_api_duration_us: None,
            seek_lock_wait_us: None,
            seek_ffmpeg_seek_us: None,
            seek_reset_us: None,
            seek_first_video_decoded_us: None,
            seek_first_audio_decoded_us: None,
            seek_target_video_ready_us: None,
            seek_target_audio_ready_us: None,
            seek_stable_us: None,
        }
    }

    fn elapsed_us(&self) -> MediaTimeUs {
        i64::try_from(self.requested_at.elapsed().as_micros()).unwrap_or(i64::MAX)
    }

    fn snapshot(&self, seek_event_count: u64, seek_active: bool) -> SeekDiagnosticsSnapshot {
        SeekDiagnosticsSnapshot {
            seek_event_count,
            seek_active,
            last_seek_target_us: self.target_us,
            seek_api_duration_us: self.seek_api_duration_us.unwrap_or(-1),
            seek_lock_wait_us: self.seek_lock_wait_us.unwrap_or(-1),
            seek_ffmpeg_seek_us: self.seek_ffmpeg_seek_us.unwrap_or(-1),
            seek_reset_us: self.seek_reset_us.unwrap_or(-1),
            seek_first_video_decoded_us: self.seek_first_video_decoded_us.unwrap_or(-1),
            seek_first_audio_decoded_us: self.seek_first_audio_decoded_us.unwrap_or(-1),
            seek_target_video_ready_us: self.seek_target_video_ready_us.unwrap_or(-1),
            seek_target_audio_ready_us: self.seek_target_audio_ready_us.unwrap_or(-1),
            seek_stable_us: self.seek_stable_us.unwrap_or(-1),
        }
    }
}

impl SeekDiagnosticsState {
    fn snapshot(&self) -> SeekDiagnosticsSnapshot {
        if let Some(active) = self.active.as_ref() {
            return active.snapshot(self.seek_event_count, true);
        }

        self.last_completed.unwrap_or(SeekDiagnosticsSnapshot {
            seek_event_count: self.seek_event_count,
            ..SeekDiagnosticsSnapshot::default()
        })
    }
}

fn update_atomic_max(target: &AtomicI64, value: MediaTimeUs) {
    let mut current = target.load(Ordering::Relaxed);
    while value > current {
        match target.compare_exchange(current, value, Ordering::Relaxed, Ordering::Relaxed) {
            Ok(_) => break,
            Err(observed) => current = observed,
        }
    }
}
