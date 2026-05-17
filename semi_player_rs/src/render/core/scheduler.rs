use crate::render::core::frame::VideoFrame;
use crate::util::time::MediaTimeUs;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum VideoScheduleDecision {
    KeepCurrent,
    PresentFrame,
    DropFrame,
    NeedMoreFrames,
}

pub struct VideoScheduler;

impl VideoScheduler {
    pub fn new() -> Self {
        Self
    }

    #[allow(clippy::unused_self)]
    pub fn decide(
        &self,
        target_time_us: MediaTimeUs,
        current_frame: Option<&VideoFrame>,
        candidate_frame: Option<&VideoFrame>,
        candidate_next_frame: Option<&VideoFrame>,
    ) -> VideoScheduleDecision {
        match (current_frame, candidate_frame) {
            (None, None) => VideoScheduleDecision::NeedMoreFrames,
            (Some(current), None) => {
                if current.covers_time_us(target_time_us) {
                    VideoScheduleDecision::KeepCurrent
                } else {
                    VideoScheduleDecision::NeedMoreFrames
                }
            }
            (None, Some(candidate)) => {
                if candidate.is_stale_for_time_us(candidate_next_frame, target_time_us) {
                    VideoScheduleDecision::DropFrame
                } else {
                    VideoScheduleDecision::PresentFrame
                }
            }
            (Some(current), Some(candidate)) => {
                if current.covers_time_with_next_us(Some(candidate), target_time_us) {
                    return VideoScheduleDecision::KeepCurrent;
                }

                if candidate.is_stale_for_time_us(candidate_next_frame, target_time_us) {
                    return VideoScheduleDecision::DropFrame;
                }

                if target_time_us < candidate.pts_us {
                    return VideoScheduleDecision::KeepCurrent;
                }

                VideoScheduleDecision::PresentFrame
            }
        }
    }
}

impl Default for VideoScheduler {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use std::sync::Arc;

    use super::{VideoScheduleDecision, VideoScheduler};
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
                vec![0; 1920 * 1080 * 4],
            )),
        }
    }

    #[test]
    fn unknown_duration_candidate_is_not_dropped_immediately() {
        let scheduler = VideoScheduler::new();
        let decision = scheduler.decide(10_000, None, Some(&frame(10_000, None)), None);
        assert_eq!(decision, VideoScheduleDecision::PresentFrame);
    }

    #[test]
    fn stale_candidate_is_dropped() {
        let scheduler = VideoScheduler::new();
        let decision = scheduler.decide(40_000, None, Some(&frame(10_000, Some(10_000))), None);
        assert_eq!(decision, VideoScheduleDecision::DropFrame);
    }

    #[test]
    fn current_frame_stays_valid_until_next_frame_pts() {
        let scheduler = VideoScheduler::new();
        let current = frame(0, Some(33_000));
        let next = frame(41_000, Some(41_000));

        let decision = scheduler.decide(38_000, Some(&current), Some(&next), None);

        assert_eq!(decision, VideoScheduleDecision::KeepCurrent);
    }

    #[test]
    fn next_frame_pts_overrides_inflated_current_duration() {
        let scheduler = VideoScheduler::new();
        let current = frame(0, Some(83_000));
        let next = frame(41_000, Some(41_000));

        let keep = scheduler.decide(40_000, Some(&current), Some(&next), None);
        let present = scheduler.decide(41_000, Some(&current), Some(&next), None);

        assert_eq!(keep, VideoScheduleDecision::KeepCurrent);
        assert_eq!(present, VideoScheduleDecision::PresentFrame);
    }

    #[test]
    fn candidate_stale_uses_next_pts_over_short_duration() {
        let scheduler = VideoScheduler::new();
        let candidate = frame(41_000, Some(10_000));
        let candidate_next = frame(83_000, Some(41_000));

        let decision = scheduler.decide(60_000, None, Some(&candidate), Some(&candidate_next));

        assert_eq!(decision, VideoScheduleDecision::PresentFrame);
    }
}
