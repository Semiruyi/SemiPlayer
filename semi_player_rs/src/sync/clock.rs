use std::sync::Mutex;
use std::time::Instant;

use crate::util::time::{add_media_time_us, MediaTimeUs};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct DevicePlaybackTiming {
    pub base_pts_us: MediaTimeUs,
    pub played_frames: u64,
    pub sample_rate: u32,
}

struct ClockState {
    anchor_media_time_us: MediaTimeUs,
    anchor_instant: Option<Instant>,
    speed: f64,
    device_timing: Option<DevicePlaybackTiming>,
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
                device_timing: None,
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
        Self::reanchor_to_now(&mut state);
        state.anchor_instant = None;
        state.device_timing = None;
    }

    pub fn seek(&self, position_us: MediaTimeUs) {
        let mut state = self.state.lock().unwrap();
        state.anchor_media_time_us = position_us;
        state.device_timing = None;
        if state.anchor_instant.is_some() {
            state.anchor_instant = Some(Instant::now());
        }
    }

    pub fn set_speed(&self, speed: f64) {
        let mut state = self.state.lock().unwrap();
        Self::reanchor_to_now(&mut state);
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
        state.device_timing = None;
    }

    pub fn update_from_device(&self, timing: Option<DevicePlaybackTiming>) {
        let mut state = self.state.lock().unwrap();
        if state.anchor_instant.is_none() {
            state.device_timing = None;
            return;
        }

        if let Some(timing) = timing {
            state.anchor_media_time_us = Self::device_media_time_us(&timing);
            state.anchor_instant = Some(Instant::now());
            state.device_timing = Some(timing);
        } else {
            Self::reanchor_to_now(&mut state);
            state.device_timing = None;
            if state.anchor_instant.is_some() {
                state.anchor_instant = Some(Instant::now());
            }
        }
    }

    pub fn presentation_time_us(&self) -> MediaTimeUs {
        let state = self.state.lock().unwrap();
        Self::projected_media_time_us(&state, Instant::now())
    }

    pub fn is_running(&self) -> bool {
        self.state.lock().unwrap().anchor_instant.is_some()
    }

    fn reanchor_to_now(state: &mut ClockState) {
        state.anchor_media_time_us = Self::projected_media_time_us(state, Instant::now());
    }

    fn projected_media_time_us(state: &ClockState, now: Instant) -> MediaTimeUs {
        let Some(anchor_instant) = state.anchor_instant else {
            return state.anchor_media_time_us;
        };

        #[allow(clippy::cast_precision_loss, clippy::cast_possible_truncation)]
        let advanced_us =
            (now.duration_since(anchor_instant).as_micros() as f64 * state.speed) as MediaTimeUs;
        add_media_time_us(state.anchor_media_time_us, advanced_us)
    }

    fn device_media_time_us(timing: &DevicePlaybackTiming) -> MediaTimeUs {
        if timing.sample_rate == 0 {
            return timing.base_pts_us;
        }

        let advanced_us = i64::try_from(timing.played_frames)
            .unwrap_or(i64::MAX)
            .saturating_mul(1_000_000)
            .saturating_div(i64::from(timing.sample_rate));
        add_media_time_us(timing.base_pts_us, advanced_us)
    }
}

impl Default for AudioClock {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::{AudioClock, DevicePlaybackTiming};

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

    #[test]
    fn device_timing_overrides_logical_projection() {
        let clock = AudioClock::new();
        clock.play();
        clock.update_from_device(Some(DevicePlaybackTiming {
            base_pts_us: 100_000,
            played_frames: 4_800,
            sample_rate: 48_000,
        }));

        let value = clock.presentation_time_us();
        assert!(value >= 200_000);
        assert!(value <= 205_000);
    }

    #[test]
    fn pause_clears_device_timing_and_freezes_position() {
        let clock = AudioClock::new();
        clock.play();
        clock.update_from_device(Some(DevicePlaybackTiming {
            base_pts_us: 100_000,
            played_frames: 4_800,
            sample_rate: 48_000,
        }));

        let before_pause = clock.presentation_time_us();
        clock.pause();
        let after_pause = clock.presentation_time_us();
        let frozen_after_pause = clock.presentation_time_us();
        assert!(before_pause >= 200_000);
        assert!(after_pause >= before_pause);
        assert_eq!(frozen_after_pause, after_pause);
    }
}
