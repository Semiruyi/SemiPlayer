use std::sync::{Arc, Mutex};

use crate::api::types::PlayerState;
use crate::audio::backends::{AudioBackendTiming, AudioOutputBackend, CpalAudioOutputBackend};
use crate::audio::core::clock::DevicePlaybackTiming;
use crate::audio::core::output::{AudioOutputChunk, AudioStreamFormat};
use crate::util::time::MediaTimeUs;

const TARGET_DEVICE_BUFFER_FRAMES: usize = 4_096;
const CHUNK_FRAME_COUNT: usize = 1_024;

pub struct AudioOutputController {
    backend: CpalAudioOutputBackend,
    timing_state: AudioOutputTimingState,
}

#[derive(Clone)]
pub struct SharedAudioOutputController {
    inner: Arc<Mutex<AudioOutputController>>,
}

#[derive(Clone, Copy, Debug, Default)]
pub struct AudioOutputSnapshot {
    pub configured_format: Option<AudioStreamFormat>,
    pub target_buffer_frames: usize,
    pub buffered_frames: usize,
    pub pending_device_frames: usize,
    pub rendered_frames_total: u64,
    pub audible_frames_total: u64,
    pub submitted_frames_total: u64,
    pub started: bool,
    pub device_timing: Option<DevicePlaybackTiming>,
}

impl AudioOutputController {
    pub fn new() -> Self {
        Self {
            backend: CpalAudioOutputBackend::new(TARGET_DEVICE_BUFFER_FRAMES),
            timing_state: AudioOutputTimingState::default(),
        }
    }

    pub fn ensure_backend_format(&mut self, audio_format: Option<AudioStreamFormat>) {
        let Some(audio_format) = audio_format else {
            return;
        };

        if self.backend.configured_format() == Some(audio_format) {
            return;
        }

        if self.backend.configure(audio_format).is_err() {
            return;
        }

        self.timing_state = AudioOutputTimingState::default();
    }

    pub fn sync_started_state(&mut self, state: PlayerState) {
        match state {
            PlayerState::Playing => {
                if self.backend.configured_format().is_some() && !self.backend.is_started() {
                    let _ = self.backend.start();
                }
            }
            PlayerState::Paused | PlayerState::Ready | PlayerState::Idle => {
                if self.backend.is_started() {
                    let _ = self.backend.pause();
                }
            }
        }
    }

    pub fn configured_format(&self) -> Option<AudioStreamFormat> {
        self.backend.configured_format()
    }

    pub fn needs_more_frames(&self) -> bool {
        self.backend.buffered_frames() < self.backend.target_buffer_frames()
    }

    pub fn next_request_frame_count(&self) -> usize {
        CHUNK_FRAME_COUNT
    }

    pub fn submit_chunk(&mut self, chunk: &AudioOutputChunk) {
        self.timing_state.observe_submit(chunk);
        if self.backend.submit(chunk).is_err() {
            self.clear_buffer();
        }
    }

    pub fn playback_timing(&self) -> Option<DevicePlaybackTiming> {
        let backend_timing = self.backend.timing();
        self.timing_state.to_device_timing(backend_timing)
    }

    pub fn snapshot(&self) -> AudioOutputSnapshot {
        let backend_timing = self.backend.timing();

        AudioOutputSnapshot {
            configured_format: self.backend.configured_format(),
            target_buffer_frames: self.backend.target_buffer_frames(),
            buffered_frames: backend_timing.buffered_frames,
            pending_device_frames: backend_timing.pending_device_frames,
            rendered_frames_total: backend_timing.rendered_frames_total,
            audible_frames_total: backend_timing.audible_frames_total,
            submitted_frames_total: self.timing_state.submitted_frames_total,
            started: backend_timing.started,
            device_timing: self.timing_state.to_device_timing(backend_timing),
        }
    }

    pub fn clear_buffer(&mut self) {
        let _ = self.backend.clear_buffer();
        self.timing_state = AudioOutputTimingState::default();
    }

    pub fn stop(&mut self) {
        let _ = self.backend.stop();
        self.timing_state = AudioOutputTimingState::default();
    }
}

impl Default for AudioOutputController {
    fn default() -> Self {
        Self::new()
    }
}

impl SharedAudioOutputController {
    pub fn new(controller: AudioOutputController) -> Self {
        Self {
            inner: Arc::new(Mutex::new(controller)),
        }
    }

    pub fn with_ref<T>(&self, f: impl FnOnce(&AudioOutputController) -> T) -> T {
        let guard = self.inner.lock().unwrap();
        f(&guard)
    }

    pub fn with_mut<T>(&self, f: impl FnOnce(&mut AudioOutputController) -> T) -> T {
        let mut guard = self.inner.lock().unwrap();
        f(&mut guard)
    }
}

impl Default for SharedAudioOutputController {
    fn default() -> Self {
        Self::new(AudioOutputController::new())
    }
}

#[derive(Clone, Copy, Debug, Default)]
struct AudioOutputTimingState {
    base_pts_us: Option<MediaTimeUs>,
    submitted_frames_total: u64,
    format: Option<AudioStreamFormat>,
}

impl AudioOutputTimingState {
    fn observe_submit(&mut self, chunk: &AudioOutputChunk) {
        let Some(format) = chunk.format() else {
            return;
        };

        if self.format != Some(format) {
            self.base_pts_us = chunk.pts_us;
            self.submitted_frames_total = 0;
            self.format = Some(format);
        }

        if self.base_pts_us.is_none() {
            self.base_pts_us = chunk.pts_us;
        }

        self.submitted_frames_total = self
            .submitted_frames_total
            .saturating_add(chunk.frame_count as u64);
    }

    fn to_device_timing(&self, backend_timing: AudioBackendTiming) -> Option<DevicePlaybackTiming> {
        let base_pts_us = self.base_pts_us?;
        let format = self.format?;
        let played_frames = backend_timing
            .audible_frames_total
            .min(self.submitted_frames_total);

        Some(DevicePlaybackTiming {
            base_pts_us,
            played_frames,
            sample_rate: format.sample_rate,
        })
    }
}
