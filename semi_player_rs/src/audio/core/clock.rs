use std::sync::Mutex;
use std::time::Instant;

use crate::util::time::{add_media_time_us, MediaTimeUs};

struct ClockState {
    anchor_media_time_us: MediaTimeUs,
    anchor_instant: Option<Instant>,
    speed: f64,
}

pub struct AudioClock {
    state: Mutex<ClockState>,
}

impl AudioClock {
    pub fn new() -> Self {
        Self {
            state: Mutex::new(ClockState {
                anchor_media_time_us: 0,
                anchor_instant: None,
                speed: 1.0,
            }),
        }
    }

    pub fn play(&self) {
        let mut state = self.state.lock().unwrap();
        if state.anchor_instant.is_none() {
            state.anchor_instant = Some(Instant::now());
        }
    }

    pub fn pause(&self) {
        let mut state = self.state.lock().unwrap();
        self.reanchor_to_now(&mut state);
        state.anchor_instant = None;
    }

    pub fn seek(&self, position_us: MediaTimeUs) {
        let mut state = self.state.lock().unwrap();
        state.anchor_media_time_us = position_us;
        if state.anchor_instant.is_some() {
            state.anchor_instant = Some(Instant::now());
        }
    }

    pub fn set_speed(&self, speed: f64) {
        let mut state = self.state.lock().unwrap();
        self.reanchor_to_now(&mut state);
        state.speed = speed;
        if state.anchor_instant.is_some() {
            state.anchor_instant = Some(Instant::now());
        }
    }

    pub fn reset(&self) {
        let mut state = self.state.lock().unwrap();
        state.anchor_media_time_us = 0;
        state.anchor_instant = None;
        state.speed = 1.0;
    }

    pub fn presentation_time_us(&self) -> MediaTimeUs {
        let state = self.state.lock().unwrap();
        self.projected_media_time_us(&state, Instant::now())
    }

    pub fn speed(&self) -> f64 {
        self.state.lock().unwrap().speed
    }

    pub fn is_running(&self) -> bool {
        self.state.lock().unwrap().anchor_instant.is_some()
    }

    fn reanchor_to_now(&self, state: &mut ClockState) {
        state.anchor_media_time_us = self.projected_media_time_us(state, Instant::now());
    }

    fn projected_media_time_us(&self, state: &ClockState, now: Instant) -> MediaTimeUs {
        let Some(anchor_instant) = state.anchor_instant else {
            return state.anchor_media_time_us;
        };

        let elapsed_us = now.duration_since(anchor_instant).as_micros() as f64;
        let advanced_us = (elapsed_us * state.speed) as MediaTimeUs;
        add_media_time_us(state.anchor_media_time_us, advanced_us)
    }
}

impl Default for AudioClock {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::AudioClock;

    #[test]
    fn paused_clock_stays_frozen() {
        let clock = AudioClock::new();
        clock.seek(123_000);
        assert_eq!(clock.presentation_time_us(), 123_000);
    }

    #[test]
    fn changing_speed_preserves_continuity() {
        let clock = AudioClock::new();
        clock.play();
        clock.set_speed(1.5);

        let first = clock.presentation_time_us();
        let second = clock.presentation_time_us();

        assert!(second >= first);
    }
}
