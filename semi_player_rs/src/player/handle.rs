use std::ffi::c_double;
use std::sync::atomic::{AtomicU32, AtomicU64, Ordering};
use std::sync::{Arc, Mutex, RwLock};

use crate::api::types::PlayerState;
use crate::audio::core::output_controller::SharedAudioOutputController;
use crate::decode::session::{MediaSession, SharedMediaSession};
use crate::decode::MediaOpenError;
use crate::decode::{
    DecodePolicy, DecodePreference, DecodedOutput, SeekRecoveryPolicy,
    VideoDecodeDiagnosticsSnapshot, VideoDecodeRequirements,
};
use crate::demux::{
    probe_expected_left_keyframe_pts, probe_expected_right_keyframe_pts, MediaInfo,
    SeekDemuxDiagnosticsSnapshot,
};
use crate::player::diagnostics::{PlayerDiagnostics, PlayerDiagnosticsSnapshot};
use crate::player::runtime::AudioDiscardSummary;
use crate::player::worker::{DecodeWorkerHandle, RenderWorkerHandle, SyncWorkerHandle};
use crate::render::core::pipeline::PresentationTargetProfile;
use crate::render::gpu::GpuDevice;
use crate::render::service::RenderService;
use crate::scheduler::snapshot::StageMap;
use crate::scheduler::decision::evaluate_scheduler_decision;
use crate::scheduler::state::SchedulerState;
use crate::scheduler::types::{SchedulerEvent, StageId};
use crate::sync::clock::AudioClock;
use crate::sync::schedule::PlayerScheduleService;
use crate::util::debug_trace::append_trace_line;
use crate::util::time::MediaTimeUs;

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
struct SeekRecoveryState {
    target_us: Option<MediaTimeUs>,
    gate_audio_until_video_ready: bool,
}

#[derive(Clone, Copy, Debug)]
struct ControlState {
    speed: c_double,
    video_decode_preference: DecodePreference,
    video_presentation_profile: PresentationTargetProfile,
    subtitles_visible: bool,
    host_presentation_offset_us: MediaTimeUs,
    seek_recovery: SeekRecoveryState,
}

impl Default for ControlState {
    fn default() -> Self {
        Self {
            speed: 1.0,
            video_decode_preference: DecodePreference::PreferPerformance,
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
    playback_phase_lock: Arc<Mutex<()>>,
    sync_worker: Option<SyncWorkerHandle>,
    render_worker: Option<RenderWorkerHandle>,
    decode_worker: Option<DecodeWorkerHandle>,
    diagnostics: PlayerDiagnostics,
    control: Mutex<ControlState>,
    scheduler: Mutex<SchedulerState>,
    media_generation: AtomicU64,
    media_session: RwLock<Option<SharedMediaSession>>,
    pub(crate) audio_clock: AudioClock,
    pub(crate) audio_output: SharedAudioOutputController,
    pub(crate) runtime: Mutex<crate::player::runtime::RuntimeDomain>,
    pub(crate) render: Mutex<RenderService>,
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
            playback_phase_lock: Arc::new(Mutex::new(())),
            sync_worker: None,
            render_worker: None,
            decode_worker: None,
            diagnostics: PlayerDiagnostics::default(),
            control: Mutex::new(ControlState::default()),
            scheduler: Mutex::new(SchedulerState::default()),
            media_generation: AtomicU64::new(0),
            media_session: RwLock::new(None),
            audio_clock: AudioClock::new(),
            audio_output: SharedAudioOutputController::default(),
            runtime: Mutex::new(crate::player::runtime::RuntimeDomain::new()),
            render: Mutex::new(render),
            gpu_device,
        }
    }

    fn with_media_session<R>(&self, f: impl FnOnce(Option<&SharedMediaSession>) -> R) -> R {
        f(self.media_session.read().unwrap().as_ref())
    }

    fn with_media_session_mut<R>(&self, f: impl FnOnce(&mut Option<SharedMediaSession>) -> R) -> R {
        f(&mut *self.media_session.write().unwrap())
    }

    pub fn is_media_loaded(&self) -> bool {
        self.with_media_session(|ms| ms.is_some())
    }

    pub fn cloned_media_session(&self) -> Option<SharedMediaSession> {
        self.with_media_session(|ms| ms.cloned())
    }

    pub fn install_media_session(&self, media_session: MediaSession) {
        self.with_media_session_mut(|ms| *ms = Some(SharedMediaSession::new(media_session)));
    }

    pub fn media_duration_us(&self) -> Option<MediaTimeUs> {
        self.with_media_session(|ms| {
            ms.and_then(|media_session| media_session.with_ref(MediaSession::duration_us))
        })
    }

    pub fn media_info(&self) -> Option<MediaInfo> {
        self.with_media_session(|ms| {
            ms.map(|media_session| media_session.with_ref(|session| session.info().clone()))
        })
    }

    pub fn seek_demux_diagnostics_snapshot(&self) -> SeekDemuxDiagnosticsSnapshot {
        self.with_media_session(|ms| {
            ms.map(SharedMediaSession::seek_diagnostics_snapshot)
                .unwrap_or_default()
        })
    }

    pub fn video_decode_diagnostics_snapshot(&self) -> VideoDecodeDiagnosticsSnapshot {
        self.with_media_session(|ms| {
            ms.map(SharedMediaSession::video_decode_diagnostics_snapshot)
                .unwrap_or_default()
        })
    }

    pub fn seek_media(&self, target_us: MediaTimeUs) -> Result<MediaTimeUs, MediaOpenError> {
        self.with_media_session(|ms| {
            let media_session = ms.ok_or(ffmpeg_next::Error::Bug).map_err(MediaOpenError::Seek)?;
            media_session.with_mut(|media_session| media_session.seek(target_us))
        })
    }

    pub fn finish_media_seek_diagnostics(&self) {
        self.with_media_session(|ms| {
            if let Some(media_session) = ms {
                #[allow(clippy::redundant_closure_for_method_calls)]
                media_session.with_mut(|media_session| media_session.finish_seek_diagnostics());
            }
        });
    }

    pub fn probe_prev_keyframe_pts(&self, target_us: MediaTimeUs) -> Option<MediaTimeUs> {
        self.with_media_session(|ms| {
            let media_session = ms?;
            let path = media_session.with_ref(|session| session.path().to_string());
            let stream_index = media_session.with_ref(|session| session.info().best_video_stream_index);
            probe_expected_left_keyframe_pts(&path, stream_index, target_us).map(|(pts, _)| pts)
        })
    }

    pub fn probe_next_keyframe_pts(&self, target_us: MediaTimeUs) -> Option<MediaTimeUs> {
        self.with_media_session(|ms| {
            let media_session = ms?;
            let path = media_session.with_ref(|session| session.path().to_string());
            let stream_index = media_session.with_ref(|session| session.info().best_video_stream_index);
            probe_expected_right_keyframe_pts(&path, stream_index, target_us)
        })
    }

    pub fn next_debug_decoded_output(&self) -> Result<Option<DecodedOutput>, MediaOpenError> {
        self.with_media_session(|ms| {
            let media_session = ms.ok_or(ffmpeg_next::Error::Bug).map_err(MediaOpenError::Seek)?;
            media_session.with_mut(MediaSession::next_decoded_output)
        })
    }

    pub fn start_workers(&mut self, player_ptr: *mut SemiPlayerHandle) {
        if self.sync_worker.is_some() {
            return;
        }

        self.sync_worker = Some(SyncWorkerHandle::start(player_ptr));
        self.render_worker = Some(RenderWorkerHandle::start(player_ptr));
        self.decode_worker = Some(DecodeWorkerHandle::start(player_ptr));
    }

    pub fn notify_workers(&self) {
        append_trace_line(&format!(
            "player:notify_workers state={:?} runtime={:?} audio_output={:?}",
            self.state(),
            self.runtime_snapshot(),
            self.audio_output_snapshot()
        ));
        self.wake_sync_worker_direct();
        self.dispatch_scheduler_event(SchedulerEvent::PlaybackDemandChanged);
    }

    pub fn stop_workers(&mut self) {
        append_trace_line("player:stop_workers begin");
        if let Some(mut sync_worker) = self.sync_worker.take() {
            sync_worker.stop();
        }

        if let Some(mut render_worker) = self.render_worker.take() {
            render_worker.stop();
        }

        if let Some(mut decode_worker) = self.decode_worker.take() {
            decode_worker.stop();
        }
        append_trace_line("player:stop_workers end");
    }

    pub fn notify_sync_worker(&self) {
        append_trace_line("player:notify_sync_worker");
        self.wake_sync_worker_direct();
    }

    pub fn dispatch_scheduler_event(&self, event: SchedulerEvent) {
        let should_process = {
            let mut scheduler = self.scheduler.lock().unwrap();
            scheduler.enqueue(event.clone());
            let trace = scheduler.trace_snapshot();
            append_trace_line(&format!(
                "scheduler:enqueue event={:?} state={:?}",
                event, trace
            ));
            scheduler.try_begin_dispatch()
        };

        if !should_process {
            return;
        }

        loop {
            let (next_event, trace_after_apply) = {
                let mut scheduler = self.scheduler.lock().unwrap();
                let next_event = scheduler.pop_next_event();
                let trace_after_apply = next_event.as_ref().map(|event| {
                    scheduler.apply_event(event);
                    scheduler.trace_snapshot()
                });
                (next_event, trace_after_apply)
            };

            let Some(next_event) = next_event else {
                let mut scheduler = self.scheduler.lock().unwrap();
                scheduler.end_dispatch();
                append_trace_line(&format!(
                    "scheduler:idle state={:?}",
                    scheduler.trace_snapshot()
                ));
                break;
            };

            if let Some(trace_after_apply) = trace_after_apply {
                append_trace_line(&format!(
                    "scheduler:apply event={:?} state={:?}",
                    next_event, trace_after_apply
                ));
            }

            let snapshot = self.scheduler_snapshot();
            let decision = evaluate_scheduler_decision(&snapshot, &next_event);
            append_trace_line(&format!(
                "scheduler:dispatch event={:?} snapshot={:?} decision={:?}",
                next_event, snapshot, decision
            ));
            self.apply_scheduler_decision(&decision);
            let mut scheduler = self.scheduler.lock().unwrap();
            scheduler.finish_event(next_event, decision);
        }
    }

    fn apply_scheduler_decision(&self, decision: &crate::scheduler::types::SchedulerDecision) {
        if decision.wake_playback {
            self.wake_sync_worker_direct();
        }

        let mut wake_render = false;
        for stage in &decision.wake_stages {
            {
                let mut scheduler = self.scheduler.lock().unwrap();
                scheduler.note_stage_requested(*stage);
                append_trace_line(&format!(
                    "scheduler:request stage={:?} state={:?}",
                    stage,
                    scheduler.trace_snapshot()
                ));
            }
            match stage {
                StageId::AudioDecode | StageId::VideoDecode => {
                    self.request_decode_stage_if_needed(*stage);
                }
                StageId::AudioRender | StageId::VideoRender => wake_render = true,
            }
        }

        if wake_render {
            self.request_render_worker_direct();
        }
    }

    fn wake_sync_worker_direct(&self) {
        append_trace_line("player:wake_sync_worker_direct");
        if let Some(sync_worker) = self.sync_worker.as_ref() {
            sync_worker.notify();
        }
    }

    pub fn notify_decode_worker(&self) {
        append_trace_line(&format!(
            "player:notify_decode_worker demand={:?}",
            self.runtime_decode_demand_snapshot()
        ));
        self.dispatch_scheduler_event(SchedulerEvent::PlaybackDemandChanged);
    }

    pub fn notify_render_worker(&self) {
        append_trace_line(&format!(
            "player:notify_render_worker supply={:?}",
            self.runtime_render_supply_snapshot()
        ));
        self.dispatch_scheduler_event(SchedulerEvent::PlaybackDemandChanged);
    }

    fn request_render_worker_direct(&self) {
        append_trace_line("player:request_render_worker_direct");
        if let Some(render_worker) = self.render_worker.as_ref() {
            render_worker.request_render();
        }
    }

    fn request_decode_stage_if_needed(&self, stage: StageId) {
        let hint =
            PlayerScheduleService::evaluate_decode_from_inputs(self.decode_schedule_inputs());
        append_trace_line(&format!(
            "player:request_decode_stage_if_needed stage={:?} hint={:?} demand={:?}",
            stage,
            hint,
            self.runtime_decode_demand_snapshot()
        ));
        if !hint.should_decode_now {
            return;
        }

        if let Some(decode_worker) = self.decode_worker.as_ref() {
            append_trace_line(&format!(
                "player:request_decode_worker_direct stage={:?}",
                stage
            ));
            decode_worker.request_decode_stage(stage);
        }
    }

    pub fn reset_runtime_state(&mut self) {
        *self.control.lock().unwrap() = ControlState::default();
        *self.scheduler.lock().unwrap() = SchedulerState::default();
        self.audio_clock.reset();
        self.audio_output
            .with_mut(crate::audio::core::output_controller::AudioOutputController::stop);
        self.runtime.lock().unwrap().clear();
    }

    pub fn clear_media(&mut self) {
        self.clear_media_session();
        self.reset_runtime_state();
    }

    pub fn clear_media_session(&self) {
        *self.media_session.write().unwrap() = None;
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

    pub fn scheduler_stage_snapshot(&self) -> StageMap {
        self.scheduler.lock().unwrap().stage_snapshot()
    }

    pub fn current_video_frame_snapshot(
        &self,
    ) -> Option<crate::render::core::frame::PresentationFrame> {
        self.runtime.lock().unwrap().runtime.current_video_frame().cloned()
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

    pub fn video_decode_preference(&self) -> DecodePreference {
        self.control.lock().unwrap().video_decode_preference
    }

    pub fn video_decode_requirements(&self) -> VideoDecodeRequirements {
        VideoDecodeRequirements {
            preference: self.video_decode_preference(),
            allow_fallback: true,
            require_gpu_output: false,
        }
    }

    pub fn set_video_decode_preference(&self, preference: DecodePreference) {
        self.control.lock().unwrap().video_decode_preference = preference;
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
        if !self.diagnostics.observe_seek_stable() {
            return;
        }
        self.finish_media_seek_diagnostics();
        self.clear_seek_recovery();
    }
}
