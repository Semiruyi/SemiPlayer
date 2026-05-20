use std::sync::Mutex;
use std::time::Instant;

use crate::util::debug_trace::append_trace_line;
use crate::util::time::{add_media_time_us, MediaTimeUs};

const DEVICE_TIMING_MIN_PLAYED_FRAMES: u64 = 1;
const DEVICE_TIMING_MAX_BACKWARD_CORRECTION_US: MediaTimeUs = 50_000;

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
        let was_running = state.anchor_instant.is_some();
        if state.anchor_instant.is_none() {
            state.anchor_instant = Some(Instant::now());
        }
        append_trace_line(&format!(
            "audio_clock:play was_running={} anchor_media_time_us={}",
            was_running,
            state.anchor_media_time_us
        ));
    }

    pub fn pause(&self) {
        let mut state = self.state.lock().unwrap();
        Self::reanchor_to_now(&mut state);
        state.anchor_instant = None;
        state.device_timing = None;
        append_trace_line(&format!(
            "audio_clock:pause anchor_media_time_us={}",
            state.anchor_media_time_us
        ));
    }

    pub fn seek(&self, position_us: MediaTimeUs) {
        let mut state = self.state.lock().unwrap();
        state.anchor_media_time_us = position_us;
        state.device_timing = None;
        if state.anchor_instant.is_some() {
            state.anchor_instant = Some(Instant::now());
        }
        append_trace_line(&format!(
            "audio_clock:seek position_us={} running={}",
            position_us,
            state.anchor_instant.is_some()
        ));
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
        append_trace_line("audio_clock:reset");
    }

    pub fn update_from_device(&self, timing: Option<DevicePlaybackTiming>) {
        let mut state = self.state.lock().unwrap();
        let before_presentation_us = Self::projected_media_time_us(&state, Instant::now());
        let had_device_timing = state.device_timing.is_some();
        if state.anchor_instant.is_none() {
            if let Some(timing) = timing.filter(|timing| timing.played_frames >= DEVICE_TIMING_MIN_PLAYED_FRAMES)
            {
                let device_media_time_us = Self::device_media_time_us(&timing);
                state.anchor_media_time_us = device_media_time_us;
                state.anchor_instant = Some(Instant::now());
                state.device_timing = Some(timing);
                let after_presentation_us = Self::projected_media_time_us(&state, Instant::now());
                append_trace_line(&format!(
                    "audio_clock:update_from_device bootstrap before={before_presentation_us} after={after_presentation_us} base_pts={} played_frames={} sample_rate={}",
                    timing.base_pts_us,
                    timing.played_frames,
                    timing.sample_rate
                ));
                return;
            }

            append_trace_line(&format!(
                "audio_clock:update_from_device skipped not_running timing={:?} before={before_presentation_us}",
                timing
            ));
            state.device_timing = None;
            return;
        }

        if let Some(timing) = timing {
            let device_media_time_us = Self::device_media_time_us(&timing);
            let backward_correction_us =
                before_presentation_us.saturating_sub(device_media_time_us);

            if timing.played_frames < DEVICE_TIMING_MIN_PLAYED_FRAMES {
                append_trace_line(&format!(
                    "audio_clock:update_from_device skipped priming before={before_presentation_us} device={device_media_time_us} base_pts={} played_frames={} sample_rate={}",
                    timing.base_pts_us,
                    timing.played_frames,
                    timing.sample_rate
                ));
                return;
            }

            if had_device_timing
                && backward_correction_us > DEVICE_TIMING_MAX_BACKWARD_CORRECTION_US
            {
                append_trace_line(&format!(
                    "audio_clock:update_from_device skipped backward_jump before={before_presentation_us} device={device_media_time_us} backward_correction={backward_correction_us} base_pts={} played_frames={} sample_rate={}",
                    timing.base_pts_us,
                    timing.played_frames,
                    timing.sample_rate
                ));
                return;
            }

            state.anchor_media_time_us = device_media_time_us;
            state.anchor_instant = Some(Instant::now());
            state.device_timing = Some(timing);
            let after_presentation_us = Self::projected_media_time_us(&state, Instant::now());
            append_trace_line(&format!(
                "audio_clock:update_from_device apply before={before_presentation_us} after={after_presentation_us} correction={} initial_sync={} base_pts={} played_frames={} sample_rate={}",
                after_presentation_us.saturating_sub(before_presentation_us),
                !had_device_timing,
                timing.base_pts_us,
                timing.played_frames,
                timing.sample_rate
            ));
        } else {
            Self::reanchor_to_now(&mut state);
            state.device_timing = None;
            if state.anchor_instant.is_some() {
                state.anchor_instant = Some(Instant::now());
            }
            let after_presentation_us = Self::projected_media_time_us(&state, Instant::now());
            append_trace_line(&format!(
                "audio_clock:update_from_device clear before={before_presentation_us} after={after_presentation_us}"
            ));
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

    #[test]
    fn device_timing_with_zero_played_frames_does_not_override_clock() {
        let clock = AudioClock::new();
        clock.seek(300_000);
        clock.play();

        let before = clock.presentation_time_us();
        clock.update_from_device(Some(DevicePlaybackTiming {
            base_pts_us: 21_333,
            played_frames: 0,
            sample_rate: 48_000,
        }));
        let after = clock.presentation_time_us();

        assert!(after >= before);
        assert!(after < 360_000);
    }

    #[test]
    fn initial_device_sync_can_reanchor_backward() {
        let clock = AudioClock::new();
        clock.seek(400_000);

        clock.update_from_device(Some(DevicePlaybackTiming {
            base_pts_us: 0,
            played_frames: 4_800,
            sample_rate: 48_000,
        }));
        let after = clock.presentation_time_us();

        assert!(after >= 100_000);
        assert!(after < 160_000);
    }

    #[test]
    fn large_backward_device_jump_is_ignored_after_initial_sync() {
        let clock = AudioClock::new();
        clock.seek(400_000);
        clock.update_from_device(Some(DevicePlaybackTiming {
            base_pts_us: 380_000,
            played_frames: 960,
            sample_rate: 48_000,
        }));

        let before = clock.presentation_time_us();
        clock.update_from_device(Some(DevicePlaybackTiming {
            base_pts_us: 21_333,
            played_frames: 550,
            sample_rate: 48_000,
        }));
        let after = clock.presentation_time_us();

        assert!(after >= before);
        assert!(after < before.saturating_add(60_000));
    }
}
