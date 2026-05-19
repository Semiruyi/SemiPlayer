use std::sync::Arc;

use crate::audio::core::output_controller::{AudioOutputSnapshot, SharedAudioOutputController};
use crate::decode::VideoDecodeDiagnosticsSnapshot;
use crate::demux::MediaInfo;
use crate::demux::SeekDemuxDiagnosticsSnapshot;
use crate::player::diagnostics::PlayerDiagnosticsSnapshot;
use crate::player::handle::SemiPlayerHandle;
use crate::player::runtime::{PlayerRuntime, RuntimeSnapshot};
use crate::render::core::frame::PresentationFrame;
use crate::render::service::RenderService;
use crate::sync::clock::AudioClock;
use crate::sync::schedule::{PumpScheduleHint, ScheduleInputs};
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

/// Operation-level playback-phase handle skeleton.
pub type PlaybackPhaseHandle = Arc<std::sync::Mutex<()>>;

impl SemiPlayerHandle {
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

    pub fn playback_snapshot_inputs(&self) -> PlaybackSnapshotInputs<'_> {
        let playback_position_us = self.playback_position_us_snapshot();
        let control = self.control_snapshot();
        let runtime = self.runtime_snapshot();
        let runtime_video = runtime.video;
        let video_sync_snapshot = crate::sync::video_sync::VideoSyncService::evaluate_from_inputs(
            VideoSyncInputs {
                host_presentation_offset_us: control.host_presentation_offset_us,
                runtime_video,
            },
            playback_position_us,
        );
        let audio_output = self.audio_output_snapshot();

        PlaybackSnapshotInputs {
            control,
            runtime,
            playback_position_us,
            video_sync_snapshot,
            video_sync_stats: self.video_sync.stats(),
            schedule_hint: crate::sync::schedule::PlayerScheduleService::evaluate_from_inputs(
                ScheduleInputs {
                    state: self.state(),
                    playback_time_us: playback_position_us,
                    gating_audio_for_seek_recovery: control.gate_audio_until_video_ready,
                    decode_supply: runtime.decode_supply,
                    video_sync_dirty: self.video_sync.is_dirty(),
                    runtime_video,
                    video_snapshot: video_sync_snapshot,
                    audio_output,
                },
            ),
            diagnostics: self.diagnostics_snapshot(),
            seek_demux: self.seek_demux_diagnostics_snapshot(),
            video_decode: self.video_decode_diagnostics_snapshot(),
            audio_output,
        }
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
