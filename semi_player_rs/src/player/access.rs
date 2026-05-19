use std::sync::Arc;

use crate::audio::core::output::{AudioOutputChunk, AudioStreamFormat};
use crate::audio::core::output_controller::{AudioOutputSnapshot, SharedAudioOutputController};
use crate::decode::VideoDecodeDiagnosticsSnapshot;
use crate::demux::MediaInfo;
use crate::demux::SeekDemuxDiagnosticsSnapshot;
use crate::player::diagnostics::PlayerDiagnosticsSnapshot;
use crate::player::handle::SemiPlayerHandle;
use crate::player::runtime::{AudioDiscardSummary, PlayerRuntime, RuntimeSnapshot};
use crate::render::core::frame::PresentationFrame;
use crate::render::service::RenderService;
use crate::sync::clock::AudioClock;
use crate::sync::schedule::{DecodeScheduleInputs, PumpScheduleHint, ScheduleInputs};
use crate::sync::video_scheduler::VideoScheduler;
use crate::sync::video_sync::{VideoSyncInputs, VideoSyncSnapshot, VideoSyncState, VideoSyncStats};
use crate::util::time::MediaTimeUs;

/// Control-domain snapshot used by new locking helpers.
///
/// This is intentionally read-only for now. The first skeleton step is to
/// establish the access boundary before migrating existing call sites.
#[derive(Clone, Copy, Debug, Default)]
pub struct ControlSnapshot {
    pub speed: f64,
    pub subtitles_visible: bool,
    pub host_presentation_offset_us: MediaTimeUs,
    pub media_generation: u64,
    pub gate_audio_until_video_ready: bool,
    pub seek_target_us: Option<MediaTimeUs>,
    pub state_raw: u32,
}

/// Runtime-domain guard skeleton.
///
/// For now this is just a typed wrapper around the existing runtime-related
/// fields so later migrations can switch call sites from `&mut SemiPlayerHandle`
/// to explicit runtime access without changing behavior all at once.
pub struct RuntimeAccess<'a> {
    pub runtime: &'a mut PlayerRuntime,
    pub video_scheduler: &'a mut VideoScheduler,
    pub video_sync: &'a mut VideoSyncState,
}

/// Audio coordination view skeleton.
pub struct AudioCoordAccess<'a> {
    pub audio_clock: &'a AudioClock,
    pub audio_output: &'a SharedAudioOutputController,
}

/// Control-domain access skeleton.
///
/// This wraps existing getter/setter methods so control-oriented call sites can
/// stop reaching for the whole player handle by default.
pub struct ControlAccess<'a> {
    player: &'a SemiPlayerHandle,
}

/// Render-domain guard skeleton.
pub struct RenderAccess<'a> {
    pub render: &'a mut RenderService,
}

#[derive(Clone, Copy, Debug, Default)]
pub struct PlaybackSnapshotInputs<'a> {
    pub control: ControlSnapshot,
    pub runtime: RuntimeSnapshot<'a>,
    pub playback_position_us: MediaTimeUs,
    pub video_sync_snapshot: VideoSyncSnapshot,
    pub video_sync_stats: VideoSyncStats,
    pub schedule_hint: PumpScheduleHint,
    pub diagnostics: PlayerDiagnosticsSnapshot,
    pub seek_demux: SeekDemuxDiagnosticsSnapshot,
    pub video_decode: VideoDecodeDiagnosticsSnapshot,
    pub audio_output: AudioOutputSnapshot,
}

#[derive(Clone, Copy, Debug)]
pub struct SeekPrepareContext {
    pub media_loaded: bool,
    pub current_state: crate::api::types::PlayerState,
    pub current_video_pts_us: Option<MediaTimeUs>,
}

#[derive(Clone, Copy, Debug, Default)]
pub struct SeekCommitContext {
    pub was_playing: bool,
}

#[derive(Clone)]
pub struct DecodePlanContext {
    pub media_loaded: bool,
    pub state: crate::api::types::PlayerState,
    pub opened_media: Option<crate::decode::session::SharedMediaSession>,
    pub generation: u64,
    pub decode_policy: crate::decode::DecodePolicy,
}

#[derive(Clone, Copy, Debug)]
pub struct DecodeAudioCommitContext {
    pub state: crate::api::types::PlayerState,
    pub decode_policy: crate::decode::DecodePolicy,
    pub audio_output: AudioOutputSnapshot,
}

#[derive(Clone, Debug)]
pub struct SyncWorkerPlanContext {
    pub media_loaded: bool,
    pub state: crate::api::types::PlayerState,
    pub schedule_hint: PumpScheduleHint,
    pub phase_lock: PlaybackPhaseHandle,
}

#[derive(Clone)]
pub struct PlaybackAdvancePlanContext {
    pub initial_playback_time_us: MediaTimeUs,
    pub initial_discard: AudioDiscardSummary,
    pub audio_format: Option<AudioStreamFormat>,
    pub state: crate::api::types::PlayerState,
    pub audio_output: SharedAudioOutputController,
    pub request_frame_count: usize,
    pub max_chunks: usize,
    pub audio_chunks: Vec<AudioOutputChunk>,
}

#[derive(Clone, Copy, Debug, Default)]
pub struct PlaybackAdvanceObserveContext<'a> {
    pub control: ControlSnapshot,
    pub runtime: RuntimeSnapshot<'a>,
    pub playback_position_us: MediaTimeUs,
    pub audio_output: AudioOutputSnapshot,
}

/// Operation-level playback-phase handle skeleton.
pub type PlaybackPhaseHandle = Arc<std::sync::Mutex<()>>;

impl SemiPlayerHandle {
    pub fn control_access(&self) -> ControlAccess<'_> {
        ControlAccess { player: self }
    }

    pub fn control_snapshot(&self) -> ControlSnapshot {
        ControlSnapshot {
            speed: self.speed(),
            subtitles_visible: self.subtitles_visible(),
            host_presentation_offset_us: self.host_presentation_offset_us(),
            media_generation: self.media_generation(),
            gate_audio_until_video_ready: self.is_gating_audio_for_seek_recovery(),
            seek_target_us: self
                .decode_policy()
                .seek_recovery
                .map(|v| v.target_video_us),
            state_raw: self.state().as_raw(),
        }
    }

    pub fn with_runtime_access<T>(&mut self, f: impl FnOnce(RuntimeAccess<'_>) -> T) -> T {
        f(RuntimeAccess {
            runtime: &mut self.runtime,
            video_scheduler: &mut self.video_scheduler,
            video_sync: &mut self.video_sync,
        })
    }

    pub fn audio_coord_access(&self) -> AudioCoordAccess<'_> {
        AudioCoordAccess {
            audio_clock: &self.audio_clock,
            audio_output: &self.audio_output,
        }
    }

    pub fn playback_position_us_snapshot(&self) -> MediaTimeUs {
        let control = self.control_snapshot();
        if control.gate_audio_until_video_ready {
            return control.seek_target_us.unwrap_or(0);
        }

        self.audio_coord_access().audio_clock.presentation_time_us()
    }

    pub fn media_duration_us_snapshot(&self) -> Option<MediaTimeUs> {
        self.media_session()
            .and_then(|media_session| media_session.with_ref(|session| session.duration_us()))
    }

    pub fn media_info_snapshot(&self) -> Option<MediaInfo> {
        self.media_session()
            .map(|media_session| media_session.with_ref(|session| session.info().clone()))
    }

    pub fn audio_output_snapshot(&self) -> AudioOutputSnapshot {
        self.audio_coord_access()
            .audio_output
            .with_ref(crate::audio::core::output_controller::AudioOutputController::snapshot)
    }

    pub fn presentation_frame_snapshot(&self) -> Option<PresentationFrame> {
        self.current_video_frame_snapshot()
    }

    pub fn runtime_snapshot(&self) -> RuntimeSnapshot<'_> {
        self.runtime.snapshot()
    }

    pub fn seek_prepare_context(&self) -> SeekPrepareContext {
        SeekPrepareContext {
            media_loaded: self.control_access().is_media_loaded(),
            current_state: self.control_access().state(),
            current_video_pts_us: self.current_video_pts_us_snapshot(),
        }
    }

    pub fn seek_commit_context(&self) -> SeekCommitContext {
        SeekCommitContext {
            was_playing: self.control_access().current_state_is_playing(),
        }
    }

    pub fn decode_plan_context(&self) -> DecodePlanContext {
        DecodePlanContext {
            media_loaded: self.control_access().is_media_loaded(),
            state: self.control_access().state(),
            opened_media: self.cloned_media_session(),
            generation: self.media_generation(),
            decode_policy: self.decode_policy(),
        }
    }

    pub fn decode_schedule_inputs(&self) -> DecodeScheduleInputs {
        DecodeScheduleInputs {
            media_loaded: self.control_access().is_media_loaded(),
            state: self.control_access().state(),
            decode_supply: self.runtime_snapshot().decode_supply,
        }
    }

    pub fn decode_audio_commit_context(&self) -> DecodeAudioCommitContext {
        DecodeAudioCommitContext {
            state: self.control_access().state(),
            decode_policy: self.decode_policy(),
            audio_output: self.audio_output_snapshot(),
        }
    }

    pub fn schedule_inputs(&self) -> ScheduleInputs<'_> {
        let playback_time_us = self.playback_position_us_snapshot();
        let control = self.control_snapshot();
        let runtime = self.runtime_snapshot();
        let runtime_video = runtime.video;
        let video_snapshot = crate::sync::video_sync::VideoSyncService::evaluate_from_inputs(
            VideoSyncInputs {
                host_presentation_offset_us: control.host_presentation_offset_us,
                runtime_video,
            },
            playback_time_us,
        );
        let audio_output = self.audio_output_snapshot();

        ScheduleInputs {
            state: crate::api::types::PlayerState::from_raw(control.state_raw)
                .unwrap_or(crate::api::types::PlayerState::Idle),
            playback_time_us,
            gating_audio_for_seek_recovery: control.gate_audio_until_video_ready,
            decode_supply: runtime.decode_supply,
            video_sync_dirty: self.video_sync_dirty_snapshot(),
            runtime_video,
            video_snapshot,
            audio_output,
        }
    }

    pub fn sync_worker_plan_context(&self) -> SyncWorkerPlanContext {
        SyncWorkerPlanContext {
            media_loaded: self.control_access().is_media_loaded(),
            state: self.control_access().state(),
            schedule_hint: crate::sync::schedule::PlayerScheduleService::evaluate_from_inputs(
                self.schedule_inputs(),
            ),
            phase_lock: self.playback_phase_handle(),
        }
    }

    pub fn playback_advance_plan_context(&mut self) -> PlaybackAdvancePlanContext {
        let initial_playback_time_us = self.playback_position_us_snapshot();
        let state = self.control_access().state();
        let audio_output = self.audio_output.clone();

        let (initial_discard, audio_format, request_frame_count, max_chunks, audio_chunks) =
            self.with_runtime_access(|runtime_access| {
                let initial_discard = runtime_access
                    .runtime
                    .discard_consumed_audio_frames(initial_playback_time_us);
                let audio_format = runtime_access.runtime.current_audio_format();
                let (request_frame_count, max_chunks) =
                    audio_output.with_ref(|audio_output| {
                        let snapshot = audio_output.snapshot();
                        if snapshot.buffered_frames >= snapshot.target_buffer_frames {
                            return (0, 0);
                        }

                        let request_frame_count =
                            crate::audio::core::output_controller::AudioOutputController::next_request_frame_count();
                        if request_frame_count == 0 {
                            return (0, 0);
                        }

                        let deficit_frames = snapshot
                            .target_buffer_frames
                            .saturating_sub(snapshot.buffered_frames);
                        let max_chunks = deficit_frames
                            .saturating_add(request_frame_count.saturating_sub(1))
                            .saturating_div(request_frame_count)
                            .clamp(1, 4);
                        (request_frame_count, max_chunks)
                    });
                let audio_chunks = runtime_access
                    .runtime
                    .pull_audio_chunks(request_frame_count, max_chunks);

                (
                    initial_discard,
                    audio_format,
                    request_frame_count,
                    max_chunks,
                    audio_chunks,
                )
            });

        PlaybackAdvancePlanContext {
            initial_playback_time_us,
            initial_discard,
            audio_format,
            state,
            audio_output,
            request_frame_count,
            max_chunks,
            audio_chunks,
        }
    }

    pub fn playback_snapshot_inputs(&self) -> PlaybackSnapshotInputs<'_> {
        let control = self.control_snapshot();
        let runtime = self.runtime_snapshot();
        let schedule_inputs = self.schedule_inputs();
        let playback_position_us = schedule_inputs.playback_time_us;
        let video_sync_snapshot = schedule_inputs.video_snapshot;
        let audio_output = self.audio_output_snapshot();

        PlaybackSnapshotInputs {
            control,
            runtime,
            playback_position_us,
            video_sync_snapshot,
            video_sync_stats: self.video_sync_stats_snapshot(),
            schedule_hint: crate::sync::schedule::PlayerScheduleService::evaluate_from_inputs(
                schedule_inputs,
            ),
            diagnostics: self.diagnostics_snapshot(),
            seek_demux: self.seek_demux_diagnostics_snapshot(),
            video_decode: self.video_decode_diagnostics_snapshot(),
            audio_output,
        }
    }

    pub fn playback_advance_observe_context(&self) -> PlaybackAdvanceObserveContext<'_> {
        PlaybackAdvanceObserveContext {
            control: self.control_snapshot(),
            runtime: self.runtime_snapshot(),
            playback_position_us: self.playback_position_us_snapshot(),
            audio_output: self.audio_output_snapshot(),
        }
    }

    pub fn video_sync_dirty_snapshot(&self) -> bool {
        self.video_sync.is_dirty()
    }

    pub fn video_sync_stats_snapshot(&self) -> VideoSyncStats {
        self.video_sync.stats()
    }

    pub fn with_render_access<T>(&self, f: impl FnOnce(RenderAccess<'_>) -> T) -> T {
        let _ = f;
        unreachable!("with_render_access requires mutable player access in the current layout")
    }

    pub fn playback_phase_handle(&self) -> PlaybackPhaseHandle {
        self.playback_phase_lock()
    }

    pub fn with_render_access_mut<T>(&mut self, f: impl FnOnce(RenderAccess<'_>) -> T) -> T {
        f(RenderAccess {
            render: &mut self.render,
        })
    }
}

impl ControlAccess<'_> {
    pub fn is_media_loaded(&self) -> bool {
        self.player.is_media_loaded()
    }

    pub fn state(&self) -> crate::api::types::PlayerState {
        self.player.state()
    }

    pub fn set_state(&self, state: crate::api::types::PlayerState) {
        self.player.set_state(state);
    }

    pub fn set_speed_value(&self, speed: f64) {
        self.player.set_speed_value(speed);
    }

    pub fn set_host_presentation_offset_us(&self, offset_us: MediaTimeUs) {
        self.player.set_host_presentation_offset_us(offset_us);
    }

    pub fn set_subtitles_visible(&self, visible: bool) {
        self.player.set_subtitles_visible(visible);
    }

    pub fn set_video_presentation_profile(
        &self,
        profile: crate::render::core::pipeline::PresentationTargetProfile,
    ) {
        self.player.set_video_presentation_profile(profile);
    }

    pub fn begin_seek_recovery(&self, target_us: MediaTimeUs) {
        self.player.begin_seek_recovery(target_us);
    }

    pub fn current_state_is_playing(&self) -> bool {
        self.state() == crate::api::types::PlayerState::Playing
    }
}

impl AudioCoordAccess<'_> {
    pub fn play_clock(&self) {
        self.audio_clock.play();
    }

    pub fn pause_clock(&self) {
        self.audio_clock.pause();
    }

    pub fn seek_clock(&self, position_us: MediaTimeUs) {
        self.audio_clock.seek(position_us);
    }

    pub fn set_clock_speed(&self, speed: f64) {
        self.audio_clock.set_speed(speed);
    }

    pub fn reset_clock(&self) {
        self.audio_clock.reset();
    }

    pub fn sync_output_started_state(&self, state: crate::api::types::PlayerState) {
        self.audio_output
            .with_mut(|audio_output| audio_output.sync_started_state(state));
    }

    pub fn clear_output_buffer(&self) {
        self.audio_output
            .with_mut(crate::audio::core::output_controller::AudioOutputController::clear_buffer);
    }

    pub fn stop_output(&self) {
        self.audio_output
            .with_mut(crate::audio::core::output_controller::AudioOutputController::stop);
    }

    pub fn started_snapshot(&self) -> bool {
        self.audio_output
            .with_ref(|audio_output| audio_output.snapshot().started)
    }

    pub fn playback_timing_snapshot(&self) -> Option<crate::sync::clock::DevicePlaybackTiming> {
        self.audio_output
            .with_ref(crate::audio::core::output_controller::AudioOutputController::playback_timing)
    }

    pub fn update_clock_from_device(
        &self,
        timing: Option<crate::sync::clock::DevicePlaybackTiming>,
    ) {
        self.audio_clock.update_from_device(timing);
    }
}

impl RuntimeAccess<'_> {
    pub fn clear_runtime(&mut self) {
        self.runtime.clear();
    }

    pub fn reset_video_scheduler(&mut self) {
        *self.video_scheduler = VideoScheduler;
    }

    pub fn reset_video_sync(&mut self) {
        self.video_sync.reset();
    }

    pub fn mark_video_sync_dirty(&mut self) {
        self.video_sync.mark_dirty();
    }

    pub fn restore_audio_chunks_front(&mut self, chunks: Vec<AudioOutputChunk>) {
        self.runtime.restore_audio_chunks_front(chunks);
    }

    pub fn discard_consumed_audio_frames(
        &mut self,
        playback_time_us: MediaTimeUs,
    ) -> AudioDiscardSummary {
        self.runtime.discard_consumed_audio_frames(playback_time_us)
    }

    pub fn current_video_pts_us(&self) -> Option<MediaTimeUs> {
        self.runtime.current_video_frame().map(|frame| frame.pts_us)
    }

    pub fn has_current_video_frame(&self) -> bool {
        self.runtime.current_video_frame().is_some()
    }

    pub fn decode_supply_status(&self) -> crate::player::runtime::DecodeSupplyStatus {
        self.runtime.decode_supply_status()
    }

    pub fn push_decoded_video_frame(
        &mut self,
        frame: crate::render::core::frame::DecodedVideoFrame,
    ) {
        self.runtime.push_decoded_video_frame(frame);
    }

    pub fn push_audio_frame(&mut self, frame: crate::audio::core::frame::AudioFrame) {
        self.runtime.push_audio_frame(frame);
    }

    pub fn mark_end_of_stream(&mut self) {
        self.runtime.mark_end_of_stream();
    }
}

impl SemiPlayerHandle {
    pub fn current_video_pts_us_snapshot(&self) -> Option<MediaTimeUs> {
        self.runtime.current_video_frame().map(|frame| frame.pts_us)
    }

    pub fn probe_prev_keyframe_pts_snapshot(&self, target_us: MediaTimeUs) -> Option<MediaTimeUs> {
        self.probe_prev_keyframe_pts(target_us)
    }

    pub fn probe_next_keyframe_pts_snapshot(&self, target_us: MediaTimeUs) -> Option<MediaTimeUs> {
        self.probe_next_keyframe_pts(target_us)
    }

    pub fn observe_seek_requested_access(&self, target_us: MediaTimeUs) {
        self.observe_seek_requested(target_us);
    }

    pub fn observe_seek_lock_acquired_access(&self) {
        self.observe_seek_lock_acquired();
    }

    pub fn observe_seek_aborted_access(&self) {
        self.observe_seek_aborted();
    }

    pub fn observe_seek_reset_finished_access(&self) {
        self.observe_seek_reset_finished();
    }

    pub fn observe_seek_api_completed_access(&self) {
        self.observe_seek_api_completed();
    }

    pub fn observe_seek_current_video_access(
        &self,
        current_pts_us: MediaTimeUs,
        current_effective_end_us: Option<MediaTimeUs>,
        playback_time_us: MediaTimeUs,
    ) {
        self.observe_seek_current_video(current_pts_us, current_effective_end_us, playback_time_us);
    }

    pub fn observe_seek_video_decoded_access(
        &self,
        frame_pts_us: MediaTimeUs,
        playback_time_us: MediaTimeUs,
    ) {
        self.observe_seek_video_decoded(frame_pts_us, playback_time_us);
    }

    pub fn observe_seek_target_audio_ready_access(&self) {
        self.observe_seek_target_audio_ready();
    }

    pub fn observe_seek_audio_output_started_access(&self) {
        self.observe_seek_audio_output_started();
    }

    pub fn observe_seek_stable_access(&self) {
        self.observe_seek_stable();
    }

    pub fn release_audio_gate_for_seek_recovery_access(&self) -> bool {
        self.release_audio_gate_for_seek_recovery()
    }

    pub fn observe_seek_first_audio_decoded_access(&self) {
        self.observe_seek_first_audio_decoded();
    }

    pub fn observe_seek_first_audio_decoder_output_access(&self) {
        self.observe_seek_first_audio_decoder_output();
    }
}
