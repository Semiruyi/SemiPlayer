use crate::audio::backends::{AudioOutputBackend, CpalAudioOutputBackend};
use crate::audio::core::output::{AudioOutputChunk, AudioStreamFormat};
use crate::api::types::PlayerState;

const TARGET_DEVICE_BUFFER_FRAMES: usize = 4_096;
const CHUNK_FRAME_COUNT: usize = 1_024;

pub struct AudioOutputController {
    backend: CpalAudioOutputBackend,
}

impl AudioOutputController {
    pub fn new() -> Self {
        Self {
            backend: CpalAudioOutputBackend::new(TARGET_DEVICE_BUFFER_FRAMES),
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
        if self.backend.submit(chunk).is_err() {
            self.clear_buffer();
        }
    }

    pub fn clear_buffer(&mut self) {
        let _ = self.backend.clear_buffer();
    }

    pub fn stop(&mut self) {
        let _ = self.backend.stop();
    }
}

impl Default for AudioOutputController {
    fn default() -> Self {
        Self::new()
    }
}
