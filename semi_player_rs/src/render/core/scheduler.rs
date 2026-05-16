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

    pub fn decide(
        &self,
        target_time_us: MediaTimeUs,
        current_frame: Option<&VideoFrame>,
        candidate_frame: Option<&VideoFrame>,
    ) -> VideoScheduleDecision {
        match (current_frame, candidate_frame) {
            (None, None) => VideoScheduleDecision::NeedMoreFrames,
            (Some(current), None) => {
                if frame_covers_time(current, target_time_us) {
                    VideoScheduleDecision::KeepCurrent
                } else {
                    VideoScheduleDecision::NeedMoreFrames
                }
            }
            (None, Some(candidate)) => {
                if frame_is_stale(candidate, target_time_us) {
                    VideoScheduleDecision::DropFrame
                } else {
                    VideoScheduleDecision::PresentFrame
                }
            }
            (Some(current), Some(candidate)) => {
                if current_frame_covers_time(current, Some(candidate), target_time_us) {
                    return VideoScheduleDecision::KeepCurrent;
                }

                if frame_is_stale(candidate, target_time_us) {
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

fn frame_covers_time(frame: &VideoFrame, target_time_us: MediaTimeUs) -> bool {
    if target_time_us < frame.pts_us {
        return false;
    }

    match frame.end_time_us() {
        Some(end_time_us) => target_time_us < end_time_us,
        None => true,
    }
}

fn current_frame_covers_time(
    current_frame: &VideoFrame,
    next_frame: Option<&VideoFrame>,
    target_time_us: MediaTimeUs,
) -> bool {
    if target_time_us < current_frame.pts_us {
        return false;
    }

    match effective_frame_end_time_us(current_frame, next_frame) {
        Some(end_time_us) => target_time_us < end_time_us,
        None => true,
    }
}

fn effective_frame_end_time_us(
    current_frame: &VideoFrame,
    next_frame: Option<&VideoFrame>,
) -> Option<MediaTimeUs> {
    let next_pts_us = next_frame
        .map(|frame| frame.pts_us)
        .filter(|next_pts_us| *next_pts_us > current_frame.pts_us);

    match (current_frame.end_time_us(), next_pts_us) {
        (Some(current_end_us), Some(next_pts_us)) => Some(current_end_us.max(next_pts_us)),
        (Some(current_end_us), None) => Some(current_end_us),
        (None, Some(next_pts_us)) => Some(next_pts_us),
        (None, None) => None,
    }
}

fn frame_is_stale(frame: &VideoFrame, target_time_us: MediaTimeUs) -> bool {
    match frame.end_time_us() {
        Some(end_time_us) => target_time_us >= end_time_us,
        None => false,
    }
}

#[cfg(test)]
mod tests {
    use super::{VideoScheduleDecision, VideoScheduler};
    use crate::render::core::frame::{PixelFormatCategory, VideoFrame};

    fn frame(pts_us: i64, duration_us: Option<i64>) -> VideoFrame {
        VideoFrame {
            pts_us,
            duration_us,
            width: 1920,
            height: 1080,
            pixel_format: PixelFormatCategory::Bgra8,
            stride: 1920 * 4,
            data: vec![0; 1920 * 1080 * 4],
            is_key_frame: false,
        }
    }

    #[test]
    fn unknown_duration_candidate_is_not_dropped_immediately() {
        let scheduler = VideoScheduler::new();
        let decision = scheduler.decide(10_000, None, Some(&frame(10_000, None)));
        assert_eq!(decision, VideoScheduleDecision::PresentFrame);
    }

    #[test]
    fn stale_candidate_is_dropped() {
        let scheduler = VideoScheduler::new();
        let decision = scheduler.decide(40_000, None, Some(&frame(10_000, Some(10_000))));
        assert_eq!(decision, VideoScheduleDecision::DropFrame);
    }

    #[test]
    fn current_frame_stays_valid_until_next_frame_pts() {
        let scheduler = VideoScheduler::new();
        let current = frame(0, Some(33_000));
        let next = frame(41_000, Some(41_000));

        let decision = scheduler.decide(38_000, Some(&current), Some(&next));

        assert_eq!(decision, VideoScheduleDecision::KeepCurrent);
    }
}
