use std::collections::VecDeque;

use crate::audio::core::frame::AudioFrame;
use crate::render::core::frame::VideoFrame;
use crate::render::core::scheduler::{VideoScheduleDecision, VideoScheduler};
use crate::util::time::MediaTimeUs;

pub struct PlayerRuntime {
    queued_audio_frames: VecDeque<AudioFrame>,
    queued_video_frames: VecDeque<VideoFrame>,
    current_video_frame: Option<VideoFrame>,
    last_audio_frame: Option<AudioFrame>,
    end_of_stream: bool,
}

impl PlayerRuntime {
    pub fn new() -> Self {
        Self {
            queued_audio_frames: VecDeque::new(),
            queued_video_frames: VecDeque::new(),
            current_video_frame: None,
            last_audio_frame: None,
            end_of_stream: false,
        }
    }

    pub fn clear(&mut self) {
        self.queued_audio_frames.clear();
        self.queued_video_frames.clear();
        self.current_video_frame = None;
        self.last_audio_frame = None;
        self.end_of_stream = false;
    }

    pub fn push_audio_frame(&mut self, frame: AudioFrame) {
        self.last_audio_frame = Some(frame.clone());
        self.queued_audio_frames.push_back(frame);
    }

    pub fn push_video_frame(&mut self, frame: VideoFrame) {
        self.queued_video_frames.push_back(frame);
    }

    pub fn mark_end_of_stream(&mut self) {
        self.end_of_stream = true;
    }

    pub fn audio_queue_len(&self) -> usize {
        self.queued_audio_frames.len()
    }

    pub fn video_queue_len(&self) -> usize {
        self.queued_video_frames.len()
    }

    pub fn last_audio_frame(&self) -> Option<&AudioFrame> {
        self.last_audio_frame.as_ref()
    }

    pub fn has_reached_end_of_stream(&self) -> bool {
        self.end_of_stream
    }

    pub fn current_video_frame(&self) -> Option<&VideoFrame> {
        self.current_video_frame.as_ref()
    }

    pub fn has_current_video_frame(&self) -> bool {
        self.current_video_frame.is_some()
    }

    pub fn select_video_frame(
        &mut self,
        scheduler: &VideoScheduler,
        target_time_us: MediaTimeUs,
    ) -> Option<&VideoFrame> {
        loop {
            let decision = scheduler.decide(
                target_time_us,
                self.current_video_frame.as_ref(),
                self.queued_video_frames.front(),
            );

            match decision {
                VideoScheduleDecision::KeepCurrent => {
                    return self.current_video_frame.as_ref();
                }
                VideoScheduleDecision::PresentFrame => {
                    self.current_video_frame = self.queued_video_frames.pop_front();
                    continue;
                }
                VideoScheduleDecision::DropFrame => {
                    let _ = self.queued_video_frames.pop_front();
                    continue;
                }
                VideoScheduleDecision::NeedMoreFrames => {
                    return self.current_video_frame.as_ref();
                }
            }
        }
    }
}

impl Default for PlayerRuntime {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::PlayerRuntime;
    use crate::render::core::frame::{PixelFormatCategory, VideoFrame};
    use crate::render::core::scheduler::VideoScheduler;

    #[test]
    fn scheduler_promotes_next_frame_into_current_slot() {
        let mut runtime = PlayerRuntime::new();
        let scheduler = VideoScheduler::new();

        runtime.push_video_frame(VideoFrame {
            pts_us: 10_000,
            duration_us: Some(10_000),
            width: 1920,
            height: 1080,
            pixel_format: PixelFormatCategory::Nv12,
            is_key_frame: true,
        });

        let current = runtime
            .select_video_frame(&scheduler, 10_000)
            .expect("current frame");

        assert_eq!(current.pts_us, 10_000);
        assert_eq!(runtime.video_queue_len(), 0);
    }

    #[test]
    fn clear_resets_current_frame_and_end_of_stream() {
        let mut runtime = PlayerRuntime::new();

        runtime.push_audio_frame(crate::audio::core::frame::AudioFrame {
            pts_us: 5_000,
            duration_us: Some(5_000),
            sample_rate: 48_000,
            channels: 2,
            sample_count: 240,
            sample_format: crate::audio::core::frame::AudioSampleFormatCategory::F32,
            is_planar: false,
        });
        runtime.push_video_frame(VideoFrame {
            pts_us: 10_000,
            duration_us: Some(10_000),
            width: 1920,
            height: 1080,
            pixel_format: PixelFormatCategory::Nv12,
            is_key_frame: true,
        });
        let _ = runtime.select_video_frame(&VideoScheduler::new(), 10_000);
        runtime.mark_end_of_stream();

        runtime.clear();

        assert_eq!(runtime.audio_queue_len(), 0);
        assert_eq!(runtime.video_queue_len(), 0);
        assert!(runtime.current_video_frame().is_none());
        assert!(runtime.last_audio_frame().is_none());
        assert!(!runtime.has_reached_end_of_stream());
    }
}
