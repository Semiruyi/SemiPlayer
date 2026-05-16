use crate::core::player::handle::SemiPlayerHandle;
use crate::core::player::runtime::VideoSelectionStats;
use crate::render::core::frame::VideoFrame;
use crate::util::time::add_media_time_us;

pub struct VideoSyncService;

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct VideoSyncStats {
    pub tick_count: u64,
    pub sync_count: u64,
    pub keep_count: u64,
    pub present_count: u64,
    pub drop_count: u64,
    pub underflow_count: u64,
    pub late_count: u64,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
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

        self.dirty = false;
    }
}

impl VideoSyncService {
    pub fn mark_dirty(player: &mut SemiPlayerHandle) {
        player.video_sync.mark_dirty();
    }

    pub fn evaluate(player: &SemiPlayerHandle, playback_time_us: i64) -> VideoSyncSnapshot {
        let target_video_time_us =
            add_media_time_us(playback_time_us, player.host_presentation_offset_us);

        let current_video_frame = player.runtime.current_video_frame();
        let next_video_frame = player.runtime.next_video_frame();
        let current_video_pts_us = current_video_frame.map(|frame| frame.pts_us).unwrap_or(0);
        let next_video_pts_us = next_video_frame.map(|frame| frame.pts_us);
        let current_video_effective_end_us = current_video_frame
            .and_then(|frame| effective_frame_end_us(frame, next_video_pts_us));
        let core_av_delta_us = playback_time_us.saturating_sub(current_video_pts_us);
        let core_sync_error_us = compute_core_sync_error_us(
            playback_time_us,
            current_video_frame,
            next_video_pts_us,
        );
        let next_wake_deadline_us = compute_next_wake_deadline_us(
            target_video_time_us,
            current_video_frame,
            current_video_effective_end_us,
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
            .video_sync
            .should_sync(playback_time_us, player.audio_clock.is_running())
        {
            return Self::sync(player, playback_time_us);
        }

        let snapshot = Self::evaluate(player, playback_time_us);
        player.video_sync.observe_tick(snapshot);
        snapshot
    }

    pub fn sync(player: &mut SemiPlayerHandle, playback_time_us: i64) -> VideoSyncSnapshot {
        let target_video_time_us =
            add_media_time_us(playback_time_us, player.host_presentation_offset_us);
        let selection = player
            .runtime
            .select_video_frame(&player.video_scheduler, target_video_time_us);
        let snapshot = Self::evaluate(player, playback_time_us);
        player.video_sync.observe_sync(snapshot, selection);
        snapshot
    }
}

fn compute_core_sync_error_us(
    target_time_us: i64,
    current_frame: Option<&VideoFrame>,
    next_video_pts_us: Option<i64>,
) -> i64 {
    let Some(frame) = current_frame else {
        return 0;
    };

    if target_time_us < frame.pts_us {
        return target_time_us - frame.pts_us;
    }

    let Some(frame_end_us) = effective_frame_end_us(frame, next_video_pts_us) else {
        return 0;
    };

    if target_time_us >= frame_end_us {
        return target_time_us - frame_end_us;
    }

    0
}

fn effective_frame_end_us(frame: &VideoFrame, next_video_pts_us: Option<i64>) -> Option<i64> {
    let next_pts_us = next_video_pts_us.filter(|next_pts_us| *next_pts_us > frame.pts_us);

    match (frame.end_time_us(), next_pts_us) {
        (Some(current_end_us), Some(next_pts_us)) => Some(current_end_us.max(next_pts_us)),
        (Some(current_end_us), None) => Some(current_end_us),
        (None, Some(next_pts_us)) => Some(next_pts_us),
        (None, None) => None,
    }
}

fn compute_next_wake_deadline_us(
    target_video_time_us: i64,
    current_frame: Option<&VideoFrame>,
    current_frame_effective_end_us: Option<i64>,
    next_video_pts_us: Option<i64>,
) -> Option<i64> {
    let wake_from_current = current_frame
        .map(|frame| frame.pts_us)
        .zip(current_frame_effective_end_us)
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
        (Some(current_deadline_us), Some(next_deadline_us)) => Some(current_deadline_us.min(next_deadline_us)),
        (Some(current_deadline_us), None) => Some(current_deadline_us),
        (None, Some(next_deadline_us)) if target_video_time_us < next_deadline_us => Some(next_deadline_us),
        (None, _) => None,
    }
}

#[cfg(test)]
mod tests {
    use super::{VideoSyncService, VideoSyncSnapshot, VideoSyncState};
    use crate::core::player::handle::SemiPlayerHandle;
    use crate::core::player::runtime::VideoSelectionStats;
    use crate::render::core::frame::{PixelFormatCategory, VideoFrame};

    fn frame(pts_us: i64, duration_us: Option<i64>) -> VideoFrame {
        VideoFrame {
            pts_us,
            duration_us,
            width: 1920,
            height: 1080,
            pixel_format: PixelFormatCategory::Bgra8,
            stride: 1920 * 4,
            data: vec![0; 16],
            is_key_frame: false,
        }
    }

    #[test]
    fn evaluate_uses_next_frame_pts_as_effective_end() {
        let mut player = SemiPlayerHandle::new();
        player.runtime.push_video_frame(frame(0, Some(33_000)));
        player.runtime.push_video_frame(frame(41_000, Some(41_000)));
        let _ = player.runtime.select_video_frame(&player.video_scheduler, 0);

        let snapshot = VideoSyncService::evaluate(&player, 48_000);

        assert_eq!(snapshot.core_sync_error_us, 7_000);
    }

    #[test]
    fn evaluate_reports_next_deadline_from_current_frame_end() {
        let mut player = SemiPlayerHandle::new();
        player.runtime.push_video_frame(frame(0, Some(33_000)));
        player.runtime.push_video_frame(frame(41_000, Some(41_000)));
        let _ = player.runtime.select_video_frame(&player.video_scheduler, 0);

        let snapshot = VideoSyncService::evaluate(&player, 10_000);

        assert_eq!(snapshot.current_video_effective_end_us, Some(41_000));
        assert_eq!(snapshot.next_wake_deadline_us, Some(41_000));
    }

    #[test]
    fn tick_skips_resync_before_deadline_when_state_is_clean() {
        let mut player = SemiPlayerHandle::new();
        player.audio_clock.play();
        player.runtime.push_video_frame(frame(0, Some(33_000)));
        player.runtime.push_video_frame(frame(41_000, Some(41_000)));

        let first = VideoSyncService::tick(&mut player, 0);
        let stats_after_first = player.video_sync.stats();

        let second = VideoSyncService::tick(&mut player, 10_000);
        let stats_after_second = player.video_sync.stats();

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
