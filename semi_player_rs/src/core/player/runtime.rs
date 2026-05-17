use std::collections::VecDeque;

use crate::audio::core::frame::{AudioFrame, NORMALIZED_AUDIO_FORMAT};
use crate::audio::core::output::AudioOutputChunk;
use crate::render::core::frame::VideoFrame;
use crate::render::core::scheduler::{VideoScheduleDecision, VideoScheduler};
use crate::util::time::MediaTimeUs;

pub const TARGET_AUDIO_QUEUE_LEN: usize = 8;
pub const TARGET_FUTURE_VIDEO_QUEUE_LEN: usize = 2;

#[derive(Clone, Copy, Debug, Default)]
pub struct RuntimeVideoSnapshot<'a> {
    pub current_frame: Option<&'a VideoFrame>,
    pub next_frame: Option<&'a VideoFrame>,
    pub current_pts_us: Option<MediaTimeUs>,
    pub next_pts_us: Option<MediaTimeUs>,
    pub current_duration_us: Option<MediaTimeUs>,
    pub current_effective_end_us: Option<MediaTimeUs>,
    pub current_to_next_delta_us: Option<MediaTimeUs>,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct AudioDiscardSummary {
    pub removed_frames: usize,
    pub max_lag_us: MediaTimeUs,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct VideoSelectionStats {
    pub kept_current: bool,
    pub presented_frames: u32,
    pub dropped_frames: u32,
    pub needs_more_frames: bool,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct DecodeSupplyStatus {
    pub audio_queue_len: usize,
    pub buffered_video_frame_count: usize,
    pub target_audio_queue_len: usize,
    pub target_total_video_frames: usize,
    pub end_of_stream: bool,
    pub has_sufficient_buffer: bool,
    pub needs_decode_supply: bool,
}

pub struct PlayerRuntime {
    queued_audio_frames: VecDeque<AudioFrame>,
    queued_audio_sample_offset: usize,
    queued_video_frames: VecDeque<VideoFrame>,
    current_video_frame: Option<VideoFrame>,
    last_audio_frame: Option<AudioFrame>,
    end_of_stream: bool,
}

impl PlayerRuntime {
    pub fn new() -> Self {
        Self {
            queued_audio_frames: VecDeque::new(),
            queued_audio_sample_offset: 0,
            queued_video_frames: VecDeque::new(),
            current_video_frame: None,
            last_audio_frame: None,
            end_of_stream: false,
        }
    }

    pub fn clear(&mut self) {
        self.queued_audio_frames.clear();
        self.queued_audio_sample_offset = 0;
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

    pub fn current_audio_format(&self) -> Option<crate::audio::core::output::AudioStreamFormat> {
        self.queued_audio_frames
            .front()
            .map(|frame| frame.format())
            .or_else(|| self.last_audio_frame.as_ref().map(|frame| frame.format()))
    }

    pub fn has_reached_end_of_stream(&self) -> bool {
        self.end_of_stream
    }

    pub fn current_video_frame(&self) -> Option<&VideoFrame> {
        self.current_video_frame.as_ref()
    }

    pub fn next_video_frame(&self) -> Option<&VideoFrame> {
        self.queued_video_frames.front()
    }

    pub fn video_snapshot(&self) -> RuntimeVideoSnapshot<'_> {
        let current_frame = self.current_video_frame();
        let next_frame = self.next_video_frame();

        RuntimeVideoSnapshot {
            current_frame,
            next_frame,
            current_pts_us: current_frame.map(|frame| frame.pts_us),
            next_pts_us: next_frame.map(|frame| frame.pts_us),
            current_duration_us: current_frame.and_then(|frame| frame.duration_us),
            current_effective_end_us: current_frame
                .and_then(|frame| frame.effective_end_time_us(next_frame)),
            current_to_next_delta_us: current_frame
                .zip(next_frame)
                .map(|(current, next)| next.pts_us.saturating_sub(current.pts_us)),
        }
    }

    pub fn buffered_video_frame_count(&self) -> usize {
        self.queued_video_frames.len() + usize::from(self.current_video_frame.is_some())
    }

    pub fn decode_supply_status(&self) -> DecodeSupplyStatus {
        let target_total_video_frames = 1 + TARGET_FUTURE_VIDEO_QUEUE_LEN;
        let audio_queue_len = self.audio_queue_len();
        let buffered_video_frame_count = self.buffered_video_frame_count();
        let has_sufficient_buffer = audio_queue_len >= TARGET_AUDIO_QUEUE_LEN
            && buffered_video_frame_count >= target_total_video_frames;
        let end_of_stream = self.has_reached_end_of_stream();

        DecodeSupplyStatus {
            audio_queue_len,
            buffered_video_frame_count,
            target_audio_queue_len: TARGET_AUDIO_QUEUE_LEN,
            target_total_video_frames,
            end_of_stream,
            has_sufficient_buffer,
            needs_decode_supply: !end_of_stream && !has_sufficient_buffer,
        }
    }

    pub fn discard_consumed_audio_frames(
        &mut self,
        playback_time_us: MediaTimeUs,
    ) -> AudioDiscardSummary {
        let mut summary = AudioDiscardSummary::default();

        while let Some(front) = self.queued_audio_frames.front() {
            let consumed_boundary_us = match front.end_time_us() {
                Some(end_time_us) => end_time_us,
                None => front.pts_us,
            };
            let is_consumed = consumed_boundary_us <= playback_time_us;

            if !is_consumed {
                break;
            }

            let _ = self.queued_audio_frames.pop_front();
            self.queued_audio_sample_offset = 0;
            summary.removed_frames += 1;
            summary.max_lag_us = summary
                .max_lag_us
                .max(playback_time_us.saturating_sub(consumed_boundary_us));
        }

        summary
    }

    pub fn pull_audio_chunk(&mut self, requested_frame_count: usize) -> Option<AudioOutputChunk> {
        if requested_frame_count == 0 {
            return None;
        }

        let mut chunk = AudioOutputChunk::default();
        let mut remaining_frames = requested_frame_count;

        while remaining_frames > 0 {
            let Some(front_frame) = self.queued_audio_frames.front() else {
                break;
            };

            let front_format = front_frame.format();
            if chunk.sample_rate == 0 {
                chunk.sample_rate = front_format.sample_rate;
                chunk.channels = front_format.channels;
            } else if chunk.sample_rate != front_format.sample_rate
                || chunk.channels != front_format.channels
            {
                break;
            }

            let channel_count = usize::from(front_frame.channels);
            if channel_count == 0 || front_frame.sample_len() <= self.queued_audio_sample_offset {
                let _ = self.queued_audio_frames.pop_front();
                self.queued_audio_sample_offset = 0;
                continue;
            }

            let available_samples = front_frame.sample_len() - self.queued_audio_sample_offset;
            let available_frames = available_samples / channel_count;
            if available_frames == 0 {
                let _ = self.queued_audio_frames.pop_front();
                self.queued_audio_sample_offset = 0;
                continue;
            }

            let frames_to_take = remaining_frames.min(available_frames);
            let samples_to_take = frames_to_take * channel_count;
            let start_sample = self.queued_audio_sample_offset;
            let end_sample = start_sample + samples_to_take;

            if chunk.pts_us.is_none() {
                let consumed_frames = start_sample / channel_count;
                let consumed_us = (consumed_frames as i64)
                    .saturating_mul(1_000_000)
                    .saturating_div(i64::from(front_frame.sample_rate));
                chunk.pts_us = Some(front_frame.pts_us.saturating_add(consumed_us));
            }

            chunk
                .samples
                .extend_from_slice(&front_frame.data[start_sample..end_sample]);
            chunk.frame_count += frames_to_take;
            remaining_frames -= frames_to_take;

            if end_sample >= front_frame.sample_len() {
                let _ = self.queued_audio_frames.pop_front();
                self.queued_audio_sample_offset = 0;
            } else {
                self.queued_audio_sample_offset = end_sample;
            }
        }

        if chunk.is_empty() {
            None
        } else {
            Some(chunk)
        }
    }

    pub fn pull_audio_chunks(
        &mut self,
        requested_frame_count: usize,
        max_chunks: usize,
    ) -> Vec<AudioOutputChunk> {
        if requested_frame_count == 0 || max_chunks == 0 {
            return Vec::new();
        }

        let mut chunks = Vec::with_capacity(max_chunks);
        for _ in 0..max_chunks {
            let Some(chunk) = self.pull_audio_chunk(requested_frame_count) else {
                break;
            };
            chunks.push(chunk);
        }

        chunks
    }

    pub fn restore_audio_chunks_front(&mut self, chunks: Vec<AudioOutputChunk>) {
        for chunk in chunks.into_iter().rev() {
            if chunk.is_empty() {
                continue;
            }

            let Some(format) = chunk.format() else {
                continue;
            };

            let duration_us = if chunk.frame_count == 0 || format.sample_rate == 0 {
                None
            } else {
                Some(
                    (chunk.frame_count as i64)
                        .saturating_mul(1_000_000)
                        .saturating_div(i64::from(format.sample_rate)),
                )
            };

            self.queued_audio_frames.push_front(AudioFrame {
                pts_us: chunk.pts_us.unwrap_or(0),
                duration_us,
                sample_rate: format.sample_rate,
                channels: format.channels,
                sample_count: chunk.frame_count,
                sample_format: NORMALIZED_AUDIO_FORMAT,
                is_planar: false,
                data: chunk.samples,
            });
        }

        self.queued_audio_sample_offset = 0;
    }

    pub fn select_video_frame(
        &mut self,
        scheduler: &VideoScheduler,
        target_time_us: MediaTimeUs,
        mut on_drop: impl FnMut(&VideoFrame),
    ) -> VideoSelectionStats {
        let mut stats = VideoSelectionStats::default();

        loop {
            let decision = scheduler.decide(
                target_time_us,
                self.current_video_frame.as_ref(),
                self.queued_video_frames.front(),
                self.queued_video_frames.get(1),
            );

            match decision {
                VideoScheduleDecision::KeepCurrent => {
                    stats.kept_current = true;
                    return stats;
                }
                VideoScheduleDecision::PresentFrame => {
                    self.current_video_frame = self.queued_video_frames.pop_front();
                    stats.presented_frames = stats.presented_frames.saturating_add(1);
                    stats.kept_current = true;
                    return stats;
                }
                VideoScheduleDecision::DropFrame => {
                    if let Some(frame) = self.queued_video_frames.pop_front() {
                        on_drop(&frame);
                    }
                    stats.dropped_frames = stats.dropped_frames.saturating_add(1);
                    continue;
                }
                VideoScheduleDecision::NeedMoreFrames => {
                    stats.needs_more_frames = true;
                    return stats;
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
    use super::{
        DecodeSupplyStatus, PlayerRuntime, VideoSelectionStats, TARGET_AUDIO_QUEUE_LEN,
        TARGET_FUTURE_VIDEO_QUEUE_LEN,
    };
    use crate::audio::core::frame::{AudioFrame, AudioSampleFormatCategory};
    use crate::audio::core::output::AudioStreamFormat;
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
            pixel_format: PixelFormatCategory::Bgra8,
            stride: 1920 * 4,
            data: vec![0; 1920 * 1080 * 4],
            is_key_frame: true,
        });

        let stats = runtime.select_video_frame(&scheduler, 10_000, |_| {});
        let current = runtime.current_video_frame().expect("current frame");

        assert_eq!(
            stats,
            VideoSelectionStats {
                kept_current: true,
                presented_frames: 1,
                dropped_frames: 0,
                needs_more_frames: false,
            }
        );
        assert_eq!(current.pts_us, 10_000);
        assert_eq!(runtime.video_queue_len(), 0);
    }

    #[test]
    fn scheduler_stops_after_presenting_one_frame_in_single_run() {
        let mut runtime = PlayerRuntime::new();
        let scheduler = VideoScheduler::new();

        runtime.push_video_frame(VideoFrame {
            pts_us: 41_000,
            duration_us: Some(41_000),
            width: 1920,
            height: 1080,
            pixel_format: PixelFormatCategory::Bgra8,
            stride: 1920 * 4,
            data: vec![0; 16],
            is_key_frame: false,
        });
        runtime.push_video_frame(VideoFrame {
            pts_us: 83_000,
            duration_us: Some(41_000),
            width: 1920,
            height: 1080,
            pixel_format: PixelFormatCategory::Bgra8,
            stride: 1920 * 4,
            data: vec![0; 16],
            is_key_frame: false,
        });

        let stats = runtime.select_video_frame(&scheduler, 90_000, |_| {});

        assert_eq!(
            stats,
            VideoSelectionStats {
                kept_current: true,
                presented_frames: 1,
                dropped_frames: 1,
                needs_more_frames: false,
            }
        );
        assert_eq!(
            runtime.current_video_frame().map(|frame| frame.pts_us),
            Some(83_000)
        );
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
            sample_format: AudioSampleFormatCategory::F32,
            is_planar: false,
            data: vec![0.0; 240 * 2],
        });
        runtime.push_video_frame(VideoFrame {
            pts_us: 10_000,
            duration_us: Some(10_000),
            width: 1920,
            height: 1080,
            pixel_format: PixelFormatCategory::Bgra8,
            stride: 1920 * 4,
            data: vec![0; 1920 * 1080 * 4],
            is_key_frame: true,
        });
        let _ = runtime.select_video_frame(&VideoScheduler::new(), 10_000, |_| {});
        runtime.mark_end_of_stream();

        runtime.clear();

        assert_eq!(runtime.audio_queue_len(), 0);
        assert_eq!(runtime.video_queue_len(), 0);
        assert!(runtime.current_video_frame().is_none());
        assert!(runtime.last_audio_frame().is_none());
        assert!(!runtime.has_reached_end_of_stream());
    }

    #[test]
    fn discard_consumed_audio_frames_keeps_future_audio() {
        let mut runtime = PlayerRuntime::new();

        runtime.push_audio_frame(AudioFrame {
            pts_us: 0,
            duration_us: Some(10_000),
            sample_rate: 48_000,
            channels: 2,
            sample_count: 480,
            sample_format: AudioSampleFormatCategory::F32,
            is_planar: false,
            data: vec![0.0; 480 * 2],
        });
        runtime.push_audio_frame(AudioFrame {
            pts_us: 10_000,
            duration_us: Some(10_000),
            sample_rate: 48_000,
            channels: 2,
            sample_count: 480,
            sample_format: AudioSampleFormatCategory::F32,
            is_planar: false,
            data: vec![0.0; 480 * 2],
        });
        runtime.push_audio_frame(AudioFrame {
            pts_us: 20_000,
            duration_us: Some(10_000),
            sample_rate: 48_000,
            channels: 2,
            sample_count: 480,
            sample_format: AudioSampleFormatCategory::F32,
            is_planar: false,
            data: vec![0.0; 480 * 2],
        });

        let removed = runtime.discard_consumed_audio_frames(15_000);

        assert_eq!(removed.removed_frames, 1);
        assert_eq!(removed.max_lag_us, 5_000);
        assert_eq!(runtime.audio_queue_len(), 2);
        assert_eq!(
            runtime.last_audio_frame().map(|frame| frame.pts_us),
            Some(20_000)
        );
    }

    #[test]
    fn pull_audio_chunk_consumes_partial_frame() {
        let mut runtime = PlayerRuntime::new();

        runtime.push_audio_frame(AudioFrame {
            pts_us: 100_000,
            duration_us: Some(10_000),
            sample_rate: 48_000,
            channels: 2,
            sample_count: 4,
            sample_format: AudioSampleFormatCategory::F32,
            is_planar: false,
            data: vec![0.0, 0.1, 1.0, 1.1, 2.0, 2.1, 3.0, 3.1],
        });

        let chunk = runtime.pull_audio_chunk(2).expect("chunk");

        assert_eq!(
            chunk.format(),
            Some(AudioStreamFormat {
                sample_rate: 48_000,
                channels: 2,
            })
        );
        assert_eq!(chunk.pts_us, Some(100_000));
        assert_eq!(chunk.frame_count, 2);
        assert_eq!(chunk.samples, vec![0.0, 0.1, 1.0, 1.1]);
        assert_eq!(runtime.audio_queue_len(), 1);

        let remaining = runtime.pull_audio_chunk(8).expect("remaining");
        assert_eq!(remaining.frame_count, 2);
        assert_eq!(remaining.samples, vec![2.0, 2.1, 3.0, 3.1]);
        assert_eq!(runtime.audio_queue_len(), 0);
    }

    #[test]
    fn pull_audio_chunks_can_be_restored_without_losing_order() {
        let mut runtime = PlayerRuntime::new();

        runtime.push_audio_frame(AudioFrame {
            pts_us: 0,
            duration_us: Some(10_000),
            sample_rate: 48_000,
            channels: 2,
            sample_count: 2,
            sample_format: AudioSampleFormatCategory::F32,
            is_planar: false,
            data: vec![0.0, 0.1, 1.0, 1.1],
        });
        runtime.push_audio_frame(AudioFrame {
            pts_us: 10_000,
            duration_us: Some(10_000),
            sample_rate: 48_000,
            channels: 2,
            sample_count: 2,
            sample_format: AudioSampleFormatCategory::F32,
            is_planar: false,
            data: vec![2.0, 2.1, 3.0, 3.1],
        });

        let chunks = runtime.pull_audio_chunks(1, 4);

        assert_eq!(chunks.len(), 4);
        assert_eq!(runtime.audio_queue_len(), 0);

        runtime.restore_audio_chunks_front(chunks);

        let restored = runtime.pull_audio_chunk(4).expect("restored chunk");
        assert_eq!(restored.pts_us, Some(0));
        assert_eq!(restored.frame_count, 4);
        assert_eq!(
            restored.samples,
            vec![0.0, 0.1, 1.0, 1.1, 2.0, 2.1, 3.0, 3.1]
        );
    }

    #[test]
    fn pull_audio_chunk_stops_on_format_boundary() {
        let mut runtime = PlayerRuntime::new();

        runtime.push_audio_frame(AudioFrame {
            pts_us: 0,
            duration_us: Some(10_000),
            sample_rate: 48_000,
            channels: 2,
            sample_count: 2,
            sample_format: AudioSampleFormatCategory::F32,
            is_planar: false,
            data: vec![0.0, 0.1, 1.0, 1.1],
        });
        runtime.push_audio_frame(AudioFrame {
            pts_us: 10_000,
            duration_us: Some(10_000),
            sample_rate: 44_100,
            channels: 2,
            sample_count: 2,
            sample_format: AudioSampleFormatCategory::F32,
            is_planar: false,
            data: vec![2.0, 2.1, 3.0, 3.1],
        });

        let chunk = runtime.pull_audio_chunk(8).expect("chunk");

        assert_eq!(chunk.sample_rate, 48_000);
        assert_eq!(chunk.frame_count, 2);
        assert_eq!(runtime.audio_queue_len(), 1);
        assert_eq!(
            runtime
                .queued_audio_frames
                .front()
                .map(|frame| frame.sample_rate),
            Some(44_100)
        );
    }

    #[test]
    fn decode_supply_status_reports_buffer_targets_and_need() {
        let mut runtime = PlayerRuntime::new();

        let empty = runtime.decode_supply_status();
        assert_eq!(
            empty,
            DecodeSupplyStatus {
                audio_queue_len: 0,
                buffered_video_frame_count: 0,
                target_audio_queue_len: TARGET_AUDIO_QUEUE_LEN,
                target_total_video_frames: TARGET_FUTURE_VIDEO_QUEUE_LEN + 1,
                end_of_stream: false,
                has_sufficient_buffer: false,
                needs_decode_supply: true,
            }
        );

        for index in 0..TARGET_AUDIO_QUEUE_LEN {
            let pts_us = i64::try_from(index).unwrap_or(i64::MAX).saturating_mul(10_000);
            runtime.push_audio_frame(AudioFrame {
                pts_us,
                duration_us: Some(10_000),
                sample_rate: 48_000,
                channels: 2,
                sample_count: 480,
                sample_format: AudioSampleFormatCategory::F32,
                is_planar: false,
                data: vec![0.0; 480 * 2],
            });
        }

        for index in 0..=TARGET_FUTURE_VIDEO_QUEUE_LEN {
            let pts_us = i64::try_from(index).unwrap_or(i64::MAX).saturating_mul(33_000);
            runtime.push_video_frame(VideoFrame {
                pts_us,
                duration_us: Some(33_000),
                width: 1920,
                height: 1080,
                pixel_format: PixelFormatCategory::Bgra8,
                stride: 1920 * 4,
                data: vec![0; 16],
                is_key_frame: false,
            });
        }

        let filled = runtime.decode_supply_status();
        assert!(filled.has_sufficient_buffer);
        assert!(!filled.needs_decode_supply);
    }
}
