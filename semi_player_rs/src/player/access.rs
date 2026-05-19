use std::sync::Arc;

use crate::audio::core::output_controller::SharedAudioOutputController;
use crate::player::handle::SemiPlayerHandle;
use crate::player::runtime::PlayerRuntime;
use crate::render::service::RenderService;
use crate::sync::clock::AudioClock;
use crate::sync::video_scheduler::VideoScheduler;
use crate::sync::video_sync::VideoSyncState;
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
