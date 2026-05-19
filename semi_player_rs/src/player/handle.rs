use std::ffi::c_double;
use std::sync::atomic::{AtomicU32, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Instant;

use crate::api::types::PlayerState;
use crate::audio::core::output_controller::SharedAudioOutputController;
use crate::decode::session::{MediaSession, SharedMediaSession};
use crate::decode::MediaOpenError;
use crate::decode::{
    DecodePolicy, DecodedOutput, SeekRecoveryPolicy, VideoDecodeDiagnosticsSnapshot,
};
use crate::demux::{
    probe_expected_left_keyframe_pts, probe_expected_right_keyframe_pts, MediaInfo,
    SeekDemuxDiagnosticsSnapshot,
};
use crate::player::diagnostics::{LockOwner, PlayerDiagnostics, PlayerDiagnosticsSnapshot};
use crate::player::runtime::{AudioDiscardSummary, PlayerRuntime};
use crate::player::worker::{DecodeWorkerHandle, SyncWorkerHandle};
use crate::render::core::pipeline::PresentationTargetProfile;
use crate::render::gpu::GpuDevice;
use crate::render::service::RenderService;
use crate::sync::clock::AudioClock;
use crate::sync::schedule::PlayerScheduleService;
use crate::sync::video_scheduler::VideoScheduler;
use crate::sync::video_sync::VideoSyncState;
use crate::util::time::MediaTimeUs;

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
struct SeekRecoveryState {
    target_us: Option<MediaTimeUs>,
    gate_audio_until_video_ready: bool,
}

#[derive(Clone, Copy, Debug)]
struct ControlState {
    speed: c_double,
    video_presentation_profile: PresentationTargetProfile,
    subtitles_visible: bool,
    host_presentation_offset_us: MediaTimeUs,
    seek_recovery: SeekRecoveryState,
}

impl Default for ControlState {
    fn default() -> Self {
        Self {
            speed: 1.0,
            video_presentation_profile: PresentationTargetProfile::CpuBgraCompatibility,
            subtitles_visible: true,
            host_presentation_offset_us: 0,
            seek_recovery: SeekRecoveryState::default(),
        }
    }
}

#[repr(C)]
pub struct SemiPlayerHandle {
    state: AtomicU32,
    op_lock: Mutex<()>,
    runtime_lock: Mutex<()>,
    playback_phase_lock: Arc<Mutex<()>>,
    sync_worker: Option<SyncWorkerHandle>,
    decode_worker: Option<DecodeWorkerHandle>,
    diagnostics: PlayerDiagnostics,
    control: Mutex<ControlState>,
    media_generation: AtomicU64,
    pub(crate) media_session: Option<SharedMediaSession>,
    pub(crate) audio_clock: AudioClock,
    pub(crate) audio_output: SharedAudioOutputController,
    pub(crate) video_scheduler: VideoScheduler,
    pub(crate) render: RenderService,
    pub(crate) runtime: PlayerRuntime,
    pub(crate) video_sync: VideoSyncState,
    pub(crate) gpu_device: Option<Arc<dyn GpuDevice>>,
}

impl SemiPlayerHandle {
    pub fn new() -> Self {
        let gpu_device = crate::render::gpu::create_default_device().ok();
        let render = match &gpu_device {
            Some(device) => RenderService::from_device(device.as_ref()),
            None => RenderService::new(),
        };

        Self {
            state: AtomicU32::new(PlayerState::Idle.as_raw()),
            op_lock: Mutex::new(()),
            runtime_lock: Mutex::new(()),
            playback_phase_lock: Arc::new(Mutex::new(())),
            sync_worker: None,
            decode_worker: None,
            diagnostics: PlayerDiagnostics::default(),
            control: Mutex::new(ControlState::default()),
            media_generation: AtomicU64::new(0),
            media_session: None,
            audio_clock: AudioClock::new(),
            audio_output: SharedAudioOutputController::default(),
            video_scheduler: VideoScheduler::new(),
            render,
            runtime: PlayerRuntime::new(),
            video_sync: VideoSyncState::default(),
            gpu_device,
        }
    }

    pub fn is_media_loaded(&self) -> bool {
        self.media_session.is_some()
    }

    pub fn media_session(&self) -> Option<&SharedMediaSession> {
        self.media_session.as_ref()
    }

    pub fn cloned_media_session(&self) -> Option<SharedMediaSession> {
        self.media_session.clone()
    }

    pub fn install_media_session(&mut self, media_session: MediaSession) {
        self.media_session = Some(SharedMediaSession::new(media_session));
    }

    pub fn media_duration_us(&self) -> Option<MediaTimeUs> {
        self.media_session()
            .and_then(|media_session| media_session.with_ref(MediaSession::duration_us))
    }

    pub fn media_info(&self) -> Option<MediaInfo> {
        self.media_session()
            .map(|media_session| media_session.with_ref(|session| session.info().clone()))
    }

    pub fn seek_demux_diagnostics_snapshot(&self) -> SeekDemuxDiagnosticsSnapshot {
        self.media_session()
            .map(SharedMediaSession::seek_diagnostics_snapshot)
            .unwrap_or_default()
    }

    pub fn video_decode_diagnostics_snapshot(&self) -> VideoDecodeDiagnosticsSnapshot {
        self.media_session()
            .map(SharedMediaSession::video_decode_diagnostics_snapshot)
            .unwrap_or_default()
    }

    pub fn seek_media(&self, target_us: MediaTimeUs) -> Result<MediaTimeUs, MediaOpenError> {
        let media_session = self
            .media_session()
            .ok_or(ffmpeg_next::Error::Bug)
            .map_err(MediaOpenError::Seek)?;
        media_session.with_mut(|media_session| media_session.seek(target_us))
    }

    pub fn finish_media_seek_diagnostics(&self) {
        if let Some(media_session) = self.media_session() {
            #[allow(clippy::redundant_closure_for_method_calls)]
            media_session.with_mut(|media_session| media_session.finish_seek_diagnostics());
        }
    }

    pub fn probe_prev_keyframe_pts(&self, target_us: MediaTimeUs) -> Option<MediaTimeUs> {
        let media_session = self.media_session()?;
        let path = media_session.with_ref(|session| session.path().to_string());
        let stream_index = media_session.with_ref(|session| session.info().best_video_stream_index);
        probe_expected_left_keyframe_pts(&path, stream_index, target_us).map(|(pts, _)| pts)
    }

    pub fn probe_next_keyframe_pts(&self, target_us: MediaTimeUs) -> Option<MediaTimeUs> {
        let media_session = self.media_session()?;
        let path = media_session.with_ref(|session| session.path().to_string());
        let stream_index = media_session.with_ref(|session| session.info().best_video_stream_index);
        probe_expected_right_keyframe_pts(&path, stream_index, target_us)
    }

    pub fn next_debug_decoded_output(&self) -> Result<Option<DecodedOutput>, MediaOpenError> {
        let media_session = self
            .media_session()
            .ok_or(ffmpeg_next::Error::Bug)
            .map_err(MediaOpenError::Seek)?;
        media_session.with_mut(MediaSession::next_decoded_output)
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
        let _runtime_guard = player_ref.runtime_lock.lock().unwrap();
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
        let hint =
            PlayerScheduleService::evaluate_decode_from_inputs(self.decode_schedule_inputs());
        if !hint.should_decode_now {
            return;
        }

        if let Some(decode_worker) = self.decode_worker.as_ref() {
            decode_worker.request_decode();
        }
    }

    pub fn reset_runtime_state(&mut self) {
        *self.control.lock().unwrap() = ControlState::default();
        self.audio_clock.reset();
        self.audio_output
            .with_mut(crate::audio::core::output_controller::AudioOutputController::stop);
        self.video_scheduler = VideoScheduler::new();
        self.runtime.clear();
        self.video_sync.reset();
    }

    pub fn clear_media(&mut self) {
        self.media_session = None;
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

    pub fn current_video_frame_snapshot(
        &self,
    ) -> Option<crate::render::core::frame::PresentationFrame> {
        let _guard = self.runtime_lock.lock().unwrap();
        self.runtime.current_video_frame().cloned()
    }

    pub fn decode_policy(&self) -> DecodePolicy {
        let target_us = self.control.lock().unwrap().seek_recovery.target_us;
        DecodePolicy {
            seek_recovery: target_us.map(|target_video_us| SeekRecoveryPolicy { target_video_us }),
        }
    }

    pub fn video_presentation_profile(&self) -> PresentationTargetProfile {
        self.control.lock().unwrap().video_presentation_profile
    }

    pub fn set_video_presentation_profile(&self, profile: PresentationTargetProfile) {
        self.control.lock().unwrap().video_presentation_profile = profile;
    }

    pub fn subtitles_visible(&self) -> bool {
        self.control.lock().unwrap().subtitles_visible
    }

    pub fn set_subtitles_visible(&self, visible: bool) {
        self.control.lock().unwrap().subtitles_visible = visible;
    }

    pub fn host_presentation_offset_us(&self) -> MediaTimeUs {
        self.control.lock().unwrap().host_presentation_offset_us
    }

    pub fn set_host_presentation_offset_us(&self, offset_us: MediaTimeUs) {
        self.control.lock().unwrap().host_presentation_offset_us = offset_us;
    }

    pub fn speed(&self) -> c_double {
        self.control.lock().unwrap().speed
    }

    pub fn set_speed_value(&self, speed: c_double) {
        self.control.lock().unwrap().speed = speed;
    }

    pub fn current_playback_time_us(&self) -> MediaTimeUs {
        let seek_recovery = self.control.lock().unwrap().seek_recovery;
        if seek_recovery.gate_audio_until_video_ready {
            return seek_recovery.target_us.unwrap_or(0);
        }

        self.audio_clock.presentation_time_us()
    }

    pub fn is_gating_audio_for_seek_recovery(&self) -> bool {
        self.control
            .lock()
            .unwrap()
            .seek_recovery
            .gate_audio_until_video_ready
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

    pub fn observe_render_stats(
        &self,
        rendered_frames: usize,
        passthrough_frames: usize,
        passthrough_with_subtitle_intent_frames: usize,
        requires_transform_frames: usize,
        fallback_passthrough_frames: usize,
    ) {
        self.diagnostics.observe_render_stats(
            rendered_frames,
            passthrough_frames,
            passthrough_with_subtitle_intent_frames,
            requires_transform_frames,
            fallback_passthrough_frames,
        );
    }

    pub fn observe_seek_requested(&self, target_us: MediaTimeUs) {
        self.diagnostics.observe_seek_requested(target_us);
    }

    pub fn begin_seek_recovery(&self, target_us: MediaTimeUs) {
        let mut control = self.control.lock().unwrap();
        let seek_recovery = &mut control.seek_recovery;
        seek_recovery.target_us = Some(target_us);
        seek_recovery.gate_audio_until_video_ready = self.state() == PlayerState::Playing;
    }

    pub fn clear_seek_recovery(&self) {
        let mut control = self.control.lock().unwrap();
        let seek_recovery = &mut control.seek_recovery;
        seek_recovery.target_us = None;
        seek_recovery.gate_audio_until_video_ready = false;
    }

    pub fn release_audio_gate_for_seek_recovery(&self) -> bool {
        let mut control = self.control.lock().unwrap();
        let seek_recovery = &mut control.seek_recovery;
        if !seek_recovery.gate_audio_until_video_ready {
            return false;
        }

        seek_recovery.gate_audio_until_video_ready = false;
        true
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
        self.finish_media_seek_diagnostics();
        self.clear_seek_recovery();
    }

    pub fn observe_seek_video_decoded(
        &self,
        frame_pts_us: MediaTimeUs,
        playback_time_us: MediaTimeUs,
    ) {
        self.diagnostics
            .observe_seek_video_decoded(frame_pts_us, playback_time_us);
    }

    pub fn observe_seek_first_audio_decoded(&self) {
        self.diagnostics.observe_seek_first_audio_decoded();
    }

    pub fn observe_seek_first_audio_decoder_output(&self) {
        self.diagnostics.observe_seek_first_audio_decoder_output();
    }

    pub fn observe_seek_current_video(
        &self,
        current_pts_us: MediaTimeUs,
        current_effective_end_us: Option<MediaTimeUs>,
        playback_time_us: MediaTimeUs,
    ) {
        self.diagnostics.observe_seek_current_video(
            current_pts_us,
            current_effective_end_us,
            playback_time_us,
        );
    }

    pub fn observe_seek_video_dropped(&self, frame_pts_us: MediaTimeUs) {
        self.diagnostics.observe_seek_video_dropped(frame_pts_us);
    }

    pub fn observe_seek_audio_output_started(&self) {
        self.diagnostics.observe_seek_audio_output_started();
    }

    pub fn observe_seek_target_audio_ready(&self) {
        self.diagnostics.observe_seek_target_audio_ready();
    }

    pub fn observe_seek_stable(&self) {
        self.diagnostics.observe_seek_stable();
        self.finish_media_seek_diagnostics();
        self.clear_seek_recovery();
    }
}
