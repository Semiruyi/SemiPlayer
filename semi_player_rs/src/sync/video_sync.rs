use crate::player::handle::SemiPlayerHandle;
use crate::player::runtime::{RuntimeVideoSnapshot, VideoSelectionStats};
use crate::sync::video_scheduler::VideoScheduler;
use crate::util::time::add_media_time_us;

pub struct VideoSyncService;

#[derive(Clone, Copy, Debug, Default)]
pub struct VideoSyncInputs {
    pub host_presentation_offset_us: i64,
    pub runtime_video: RuntimeVideoSnapshot,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct VideoSyncStats {
    pub tick_count: u64,
    pub sync_count: u64,
    pub keep_count: u64,
    pub present_count: u64,
    pub drop_count: u64,
    pub underflow_count: u64,
    pub late_count: u64,
    pub last_presented_frames: u64,
    pub last_dropped_frames: u64,
    pub max_presented_frames_in_run: u64,
    pub max_dropped_frames_in_run: u64,
    pub run_present_only_count: u64,
    pub run_drop_only_count: u64,
    pub run_present_drop_count: u64,
    pub run_other_count: u64,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
#[allow(clippy::struct_field_names)]
pub struct VideoSyncSnapshot {
    pub target_video_time_us: i64,
    pub current_video_pts_us: i64,
    pub next_video_pts_us: Option<i64>,
    pub current_video_effective_end_us: Option<i64>,
    pub next_wake_deadline_us: Option<i64>,
    pub core_av_delta_us: i64,
    pub core_sync_error_us: i64,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct VideoSyncState {
    snapshot: VideoSyncSnapshot,
    stats: VideoSyncStats,
    dirty: bool,
}

impl VideoSyncState {
    pub fn reset(&mut self) {
        *self = Self {
            dirty: true,
            ..Self::default()
        };
    }

    pub fn mark_dirty(&mut self) {
        self.dirty = true;
    }

    pub fn stats(&self) -> VideoSyncStats {
        self.stats
    }

    pub fn is_dirty(&self) -> bool {
        self.dirty
    }

    fn should_sync(&self, playback_time_us: i64, is_running: bool) -> bool {
        if self.dirty || self.stats.tick_count == 0 {
            return true;
        }

        if !is_running {
            return false;
        }

        if self.snapshot.core_sync_error_us > 0 {
            return true;
        }

        self.snapshot
            .next_wake_deadline_us
            .is_some_and(|deadline_us| playback_time_us >= deadline_us)
    }

    fn observe_tick(&mut self, snapshot: VideoSyncSnapshot) {
        self.snapshot = snapshot;
        self.stats.tick_count = self.stats.tick_count.saturating_add(1);
    }

    fn observe_sync(&mut self, snapshot: VideoSyncSnapshot, selection: VideoSelectionStats) {
        self.observe_tick(snapshot);
        self.stats.sync_count = self.stats.sync_count.saturating_add(1);
        self.stats.last_presented_frames = u64::from(selection.presented_frames);
        self.stats.last_dropped_frames = u64::from(selection.dropped_frames);
        self.stats.max_presented_frames_in_run = self
            .stats
            .max_presented_frames_in_run
            .max(u64::from(selection.presented_frames));
        self.stats.max_dropped_frames_in_run = self
            .stats
            .max_dropped_frames_in_run
            .max(u64::from(selection.dropped_frames));
        self.stats.present_count = self
            .stats
            .present_count
            .saturating_add(u64::from(selection.presented_frames));
        self.stats.drop_count = self
            .stats
            .drop_count
            .saturating_add(u64::from(selection.dropped_frames));

        if selection.kept_current {
            self.stats.keep_count = self.stats.keep_count.saturating_add(1);
        }

        if selection.needs_more_frames {
            self.stats.underflow_count = self.stats.underflow_count.saturating_add(1);
        }

        if snapshot.core_sync_error_us > 0 {
            self.stats.late_count = self.stats.late_count.saturating_add(1);
        }

        match (selection.presented_frames, selection.dropped_frames) {
            (presented, 0) if presented > 0 => {
                self.stats.run_present_only_count =
                    self.stats.run_present_only_count.saturating_add(1);
            }
            (0, dropped) if dropped > 0 => {
                self.stats.run_drop_only_count = self.stats.run_drop_only_count.saturating_add(1);
            }
            (presented, dropped) if presented > 0 && dropped > 0 => {
                self.stats.run_present_drop_count =
                    self.stats.run_present_drop_count.saturating_add(1);
            }
            _ => {
                self.stats.run_other_count = self.stats.run_other_count.saturating_add(1);
            }
        }

        self.dirty = false;
    }
}

impl VideoSyncService {
    pub fn evaluate(player: &SemiPlayerHandle, playback_time_us: i64) -> VideoSyncSnapshot {
        Self::evaluate_from_inputs(
            VideoSyncInputs {
                host_presentation_offset_us: player.host_presentation_offset_us(),
                runtime_video: player.runtime.lock().unwrap().runtime.video_snapshot(),
            },
            playback_time_us,
        )
    }

    pub fn evaluate_from_inputs(
        inputs: VideoSyncInputs,
        playback_time_us: i64,
    ) -> VideoSyncSnapshot {
        let target_video_time_us =
            add_media_time_us(playback_time_us, inputs.host_presentation_offset_us);

        let runtime_video = inputs.runtime_video;
        let current_video_pts_us = runtime_video.current_pts_us.unwrap_or(0);
        let next_video_pts_us = runtime_video.next_pts_us;
        let current_video_effective_end_us = runtime_video.current_effective_end_us;
        let core_av_delta_us = playback_time_us.saturating_sub(current_video_pts_us);
        let core_sync_error_us = compute_core_sync_error_us(
            playback_time_us,
            &runtime_video,
        );
        let next_wake_deadline_us = compute_next_wake_deadline_us(
            target_video_time_us,
            &runtime_video,
            next_video_pts_us,
        );

        VideoSyncSnapshot {
            target_video_time_us,
            current_video_pts_us,
            next_video_pts_us,
            current_video_effective_end_us,
            next_wake_deadline_us,
            core_av_delta_us,
            core_sync_error_us,
        }
    }

    pub fn tick(player: &mut SemiPlayerHandle, playback_time_us: i64) -> VideoSyncSnapshot {
        if player
            .runtime
            .lock()
            .unwrap()
            .video_sync
            .should_sync(playback_time_us, player.audio_clock.is_running())
        {
            return Self::sync(player, playback_time_us);
        }

        let snapshot = Self::evaluate(player, playback_time_us);
        player.runtime.lock().unwrap().video_sync.observe_tick(snapshot);
        snapshot
    }

    pub fn sync(player: &mut SemiPlayerHandle, playback_time_us: i64) -> VideoSyncSnapshot {
        let target_video_time_us =
            add_media_time_us(playback_time_us, player.host_presentation_offset_us());
        let mut dropped_pts = Vec::new();
        let (_selection, snapshot) = {
            let mut domain = player.runtime.lock().unwrap();
            let selection = domain.runtime.select_video_frame(
                &VideoScheduler,
                target_video_time_us,
                |frame| dropped_pts.push(frame.pts_us),
            );
            let snapshot = Self::evaluate_from_inputs(
                VideoSyncInputs {
                    host_presentation_offset_us: player.host_presentation_offset_us(),
                    runtime_video: domain.runtime.video_snapshot(),
                },
                playback_time_us,
            );
            domain.video_sync.observe_sync(snapshot, selection);
            (selection, snapshot)
        };
        for pts_us in dropped_pts {
            player.observe_seek_video_dropped(pts_us);
        }
        snapshot
    }
}

fn compute_core_sync_error_us(
    target_time_us: i64,
    runtime_video: &RuntimeVideoSnapshot,
) -> i64 {
    let Some(current_pts_us) = runtime_video.current_pts_us else {
        return 0;
    };

    if target_time_us < current_pts_us {
        return target_time_us - current_pts_us;
    }

    let Some(frame_end_us) = runtime_video.current_effective_end_us else {
        return 0;
    };

    if target_time_us >= frame_end_us {
        return target_time_us - frame_end_us;
    }

    0
}
fn compute_next_wake_deadline_us(
    target_video_time_us: i64,
    runtime_video: &RuntimeVideoSnapshot,
    next_video_pts_us: Option<i64>,
) -> Option<i64> {
    let wake_from_current = runtime_video
        .current_pts_us
        .zip(runtime_video.current_effective_end_us)
        .and_then(|(frame_start_us, frame_end_us)| {
            if target_video_time_us < frame_start_us {
                Some(frame_start_us)
            } else if target_video_time_us < frame_end_us {
                Some(frame_end_us)
            } else {
                None
            }
        });

    match (wake_from_current, next_video_pts_us) {
        (Some(current_deadline_us), Some(next_deadline_us)) => {
            Some(current_deadline_us.min(next_deadline_us))
        }
        (Some(current_deadline_us), None) => Some(current_deadline_us),
        (None, Some(next_deadline_us)) if target_video_time_us < next_deadline_us => {
            Some(next_deadline_us)
        }
        (None, _) => None,
    }
}

#[cfg(test)]
mod tests {
    use std::sync::Arc;

    use super::{VideoSyncService, VideoSyncSnapshot, VideoSyncState};
    use crate::player::handle::SemiPlayerHandle;
    use crate::player::runtime::VideoSelectionStats;
    use crate::render::core::frame::{PixelFormatCategory, VideoFrame, VideoSurface};

    fn frame(pts_us: i64, duration_us: Option<i64>) -> VideoFrame {
        VideoFrame {
            pts_us,
            duration_us,
            width: 1920,
            height: 1080,
            is_key_frame: false,
            surface: Arc::new(VideoSurface::new_cpu_packed(
                PixelFormatCategory::Bgra8,
                1920 * 4,
                vec![0; 16],
            )),
        }
    }

    #[test]
    fn evaluate_uses_next_frame_pts_as_effective_end() {
        let mut player = SemiPlayerHandle::new();
        let rt = player.runtime.get_mut().unwrap();
        rt.runtime.push_video_frame(frame(0, Some(33_000)));
        rt.runtime.push_video_frame(frame(41_000, Some(41_000)));
        let _ = rt.runtime.select_video_frame(&rt.video_scheduler, 0, |_| {});

        let snapshot = VideoSyncService::evaluate(&player, 48_000);

        assert_eq!(snapshot.core_sync_error_us, 7_000);
    }

    #[test]
    fn evaluate_reports_next_deadline_from_current_frame_end() {
        let mut player = SemiPlayerHandle::new();
        let rt = player.runtime.get_mut().unwrap();
        rt.runtime.push_video_frame(frame(0, Some(33_000)));
        rt.runtime.push_video_frame(frame(41_000, Some(41_000)));
        let _ = rt.runtime.select_video_frame(&rt.video_scheduler, 0, |_| {});

        let snapshot = VideoSyncService::evaluate(&player, 10_000);

        assert_eq!(snapshot.current_video_effective_end_us, Some(41_000));
        assert_eq!(snapshot.next_wake_deadline_us, Some(41_000));
    }

    #[test]
    fn evaluate_prefers_next_pts_over_inflated_duration() {
        let mut player = SemiPlayerHandle::new();
        let rt = player.runtime.get_mut().unwrap();
        rt.runtime.push_video_frame(frame(0, Some(83_000)));
        rt.runtime.push_video_frame(frame(41_000, Some(41_000)));
        let _ = rt.runtime.select_video_frame(&rt.video_scheduler, 0, |_| {});

        let snapshot_before_boundary = VideoSyncService::evaluate(&player, 40_000);
        let snapshot_after_boundary = VideoSyncService::evaluate(&player, 41_000);

        assert_eq!(
            snapshot_before_boundary.current_video_effective_end_us,
            Some(41_000)
        );
        assert_eq!(snapshot_before_boundary.core_sync_error_us, 0);
        assert_eq!(snapshot_after_boundary.core_sync_error_us, 0);
        assert_eq!(
            snapshot_after_boundary.current_video_effective_end_us,
            Some(41_000)
        );
        assert_eq!(snapshot_after_boundary.next_wake_deadline_us, None);
    }

    #[test]
    fn tick_skips_resync_before_deadline_when_state_is_clean() {
        let mut player = SemiPlayerHandle::new();
        player.audio_clock.play();
        let rt = player.runtime.get_mut().unwrap();
        rt.runtime.push_video_frame(frame(0, Some(33_000)));
        rt.runtime.push_video_frame(frame(41_000, Some(41_000)));

        let first = VideoSyncService::tick(&mut player, 0);
        let stats_after_first = player.runtime.get_mut().unwrap().video_sync.stats();

        let second = VideoSyncService::tick(&mut player, 10_000);
        let stats_after_second = player.runtime.get_mut().unwrap().video_sync.stats();

        assert_eq!(first.next_wake_deadline_us, Some(41_000));
        assert_eq!(second.current_video_pts_us, 0);
        assert_eq!(stats_after_first.sync_count, 1);
        assert_eq!(stats_after_second.sync_count, 1);
        assert_eq!(stats_after_second.tick_count, 2);
    }

    #[test]
    fn dirty_state_forces_resync_without_waiting_for_deadline() {
        let mut state = VideoSyncState::default();
        assert!(state.should_sync(0, false));

        state.observe_sync(VideoSyncSnapshot::default(), VideoSelectionStats::default());
        assert!(!state.should_sync(0, false));

        state.mark_dirty();
        assert!(state.should_sync(0, false));
    }
}
