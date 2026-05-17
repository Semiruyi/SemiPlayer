use std::collections::VecDeque;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};
use cpal::{
    BufferSize, OutputCallbackInfo, SampleFormat, SampleRate, Stream, StreamConfig,
    SupportedStreamConfigRange,
};

use crate::audio::core::output::{AudioOutputChunk, AudioStreamFormat};

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct AudioBackendTiming {
    pub rendered_frames_total: u64,
    pub audible_frames_total: u64,
    pub buffered_frames: usize,
    pub pending_device_frames: usize,
    pub started: bool,
}

#[derive(Debug)]
pub enum AudioBackendError {
    NotConfigured,
    InvalidFormat,
    DeviceUnavailable,
    BackendFailure,
}

pub trait AudioOutputBackend {
    fn configure(&mut self, format: AudioStreamFormat) -> Result<(), AudioBackendError>;

    fn start(&mut self) -> Result<(), AudioBackendError>;

    fn pause(&mut self) -> Result<(), AudioBackendError>;

    fn stop(&mut self) -> Result<(), AudioBackendError>;

    fn clear_buffer(&mut self) -> Result<(), AudioBackendError>;

    fn submit(&mut self, chunk: &AudioOutputChunk) -> Result<(), AudioBackendError>;

    fn timing(&self) -> AudioBackendTiming;
}

pub struct CpalAudioOutputBackend {
    stream: Option<Stream>,
    configured_format: Option<AudioStreamFormat>,
    started: bool,
    target_buffer_frames: usize,
    shared: Arc<Mutex<SharedAudioBuffer>>,
}

impl CpalAudioOutputBackend {
    pub fn new(target_buffer_frames: usize) -> Self {
        Self {
            stream: None,
            configured_format: None,
            started: false,
            target_buffer_frames,
            shared: Arc::new(Mutex::new(SharedAudioBuffer::default())),
        }
    }

    pub fn configured_format(&self) -> Option<AudioStreamFormat> {
        self.configured_format
    }

    pub fn is_started(&self) -> bool {
        self.started
    }

    pub fn target_buffer_frames(&self) -> usize {
        self.target_buffer_frames
    }
}

impl AudioOutputBackend for CpalAudioOutputBackend {
    fn configure(&mut self, format: AudioStreamFormat) -> Result<(), AudioBackendError> {
        let host = cpal::default_host();
        let device = host
            .default_output_device()
            .ok_or(AudioBackendError::DeviceUnavailable)?;

        let config = find_matching_output_config(&device, format)?;
        let stream_config = StreamConfig {
            channels: format.channels,
            sample_rate: SampleRate(format.sample_rate),
            buffer_size: select_buffer_size(&config, self.target_buffer_frames),
        };

        let shared = Arc::clone(&self.shared);
        let stream = device
            .build_output_stream(
                &stream_config,
                move |output: &mut [f32], info| fill_output_buffer(output, info, &shared),
                |_error| {},
                None,
            )
            .map_err(|_| AudioBackendError::BackendFailure)?;

        self.stream = Some(stream);
        self.configured_format = Some(format);
        self.started = false;

        let mut shared = self.shared.lock().unwrap();
        shared.format = Some(format);
        shared.samples.clear();
        shared.buffered_frames = 0;
        shared.rendered_frames_total = 0;
        shared.completed_audible_frames_total = 0;
        shared.scheduled_blocks.clear();
        shared.paused_at = None;
        Ok(())
    }

    fn start(&mut self) -> Result<(), AudioBackendError> {
        let stream = self
            .stream
            .as_ref()
            .ok_or(AudioBackendError::NotConfigured)?;
        {
            let mut shared = self.shared.lock().unwrap();
            shared.resume_playback_clock(Instant::now());
        }
        stream
            .play()
            .map_err(|_| AudioBackendError::BackendFailure)?;
        self.started = true;
        Ok(())
    }

    fn pause(&mut self) -> Result<(), AudioBackendError> {
        let stream = self
            .stream
            .as_ref()
            .ok_or(AudioBackendError::NotConfigured)?;
        stream
            .pause()
            .map_err(|_| AudioBackendError::BackendFailure)?;
        {
            let mut shared = self.shared.lock().unwrap();
            shared.pause_playback_clock(Instant::now());
        }
        self.started = false;
        Ok(())
    }

    fn stop(&mut self) -> Result<(), AudioBackendError> {
        if let Some(stream) = self.stream.as_ref() {
            stream
                .pause()
                .map_err(|_| AudioBackendError::BackendFailure)?;
        }
        self.started = false;
        self.clear_buffer()
    }

    fn clear_buffer(&mut self) -> Result<(), AudioBackendError> {
        let mut shared = self.shared.lock().unwrap();
        shared.samples.clear();
        shared.buffered_frames = 0;
        shared.rendered_frames_total = 0;
        shared.completed_audible_frames_total = 0;
        shared.scheduled_blocks.clear();
        shared.paused_at = None;
        Ok(())
    }

    fn submit(&mut self, chunk: &AudioOutputChunk) -> Result<(), AudioBackendError> {
        let configured_format = self
            .configured_format
            .ok_or(AudioBackendError::NotConfigured)?;
        let chunk_format = chunk.format().ok_or(AudioBackendError::InvalidFormat)?;
        if configured_format != chunk_format {
            return Err(AudioBackendError::InvalidFormat);
        }

        let mut shared = self.shared.lock().unwrap();
        shared.samples.extend(chunk.samples.iter().copied());
        shared.buffered_frames = shared.buffered_frames.saturating_add(chunk.frame_count);
        Ok(())
    }

    fn timing(&self) -> AudioBackendTiming {
        let mut shared = self.shared.lock().unwrap();
        let progress = shared.playback_progress(Instant::now());
        AudioBackendTiming {
            rendered_frames_total: shared.rendered_frames_total,
            audible_frames_total: progress.audible_frames_total,
            buffered_frames: shared.buffered_frames,
            pending_device_frames: progress.pending_device_frames,
            started: self.started,
        }
    }
}

#[derive(Default)]
struct SharedAudioBuffer {
    format: Option<AudioStreamFormat>,
    samples: VecDeque<f32>,
    buffered_frames: usize,
    rendered_frames_total: u64,
    completed_audible_frames_total: u64,
    scheduled_blocks: VecDeque<ScheduledPlaybackBlock>,
    paused_at: Option<Instant>,
}

#[derive(Clone, Copy, Debug)]
struct ScheduledPlaybackBlock {
    playback_start: Instant,
    frame_count: usize,
    sample_rate: u32,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
struct PlaybackProgress {
    audible_frames_total: u64,
    pending_device_frames: usize,
}

impl SharedAudioBuffer {
    fn playback_progress(&mut self, now: Instant) -> PlaybackProgress {
        let effective_now = self.paused_at.unwrap_or(now);

        while let Some(front) = self.scheduled_blocks.front() {
            if !front.is_finished(effective_now) {
                break;
            }

            self.completed_audible_frames_total = self
                .completed_audible_frames_total
                .saturating_add(front.frame_count as u64);
            self.scheduled_blocks.pop_front();
        }

        let mut partially_audible_frames_total = 0u64;
        let mut pending_device_frames = 0usize;

        for block in &self.scheduled_blocks {
            let audible_frames = block.audible_frames_at(effective_now);
            partially_audible_frames_total =
                partially_audible_frames_total.saturating_add(audible_frames as u64);
            pending_device_frames = pending_device_frames
                .saturating_add(block.frame_count.saturating_sub(audible_frames));
        }

        PlaybackProgress {
            audible_frames_total: self
                .completed_audible_frames_total
                .saturating_add(partially_audible_frames_total),
            pending_device_frames,
        }
    }

    fn pause_playback_clock(&mut self, now: Instant) {
        if self.paused_at.is_some() {
            return;
        }

        let _ = self.playback_progress(now);
        self.paused_at = Some(now);
    }

    fn resume_playback_clock(&mut self, now: Instant) {
        let Some(paused_at) = self.paused_at.take() else {
            return;
        };

        let paused_duration = now.saturating_duration_since(paused_at);
        if paused_duration.is_zero() {
            return;
        }

        for block in &mut self.scheduled_blocks {
            block.shift_forward(paused_duration);
        }
    }
}

impl ScheduledPlaybackBlock {
    fn new(playback_start: Instant, frame_count: usize, sample_rate: u32) -> Self {
        Self {
            playback_start,
            frame_count,
            sample_rate,
        }
    }

    fn is_finished(&self, now: Instant) -> bool {
        now >= self.playback_end()
    }

    fn audible_frames_at(&self, now: Instant) -> usize {
        if now <= self.playback_start {
            return 0;
        }

        let elapsed = now.duration_since(self.playback_start);
        let audible_frames = duration_to_frames(elapsed, self.sample_rate);
        audible_frames.min(self.frame_count)
    }

    fn playback_end(&self) -> Instant {
        self.playback_start + frames_to_duration(self.frame_count, self.sample_rate)
    }

    fn shift_forward(&mut self, duration: Duration) {
        self.playback_start += duration;
    }
}

fn find_matching_output_config(
    device: &cpal::Device,
    format: AudioStreamFormat,
) -> Result<SupportedStreamConfigRange, AudioBackendError> {
    let supported_configs = device
        .supported_output_configs()
        .map_err(|_| AudioBackendError::DeviceUnavailable)?;

    supported_configs
        .into_iter()
        .find(|config| {
            config.sample_format() == SampleFormat::F32
                && config.channels() == format.channels
                && config.min_sample_rate().0 <= format.sample_rate
                && config.max_sample_rate().0 >= format.sample_rate
        })
        .ok_or(AudioBackendError::InvalidFormat)
}

fn fill_output_buffer(
    output: &mut [f32],
    info: &OutputCallbackInfo,
    shared: &Arc<Mutex<SharedAudioBuffer>>,
) {
    let mut shared = shared.lock().unwrap();
    let channel_count = shared
        .format
        .map_or(1, AudioStreamFormat::sample_stride)
        .max(1);

    for sample in output.iter_mut() {
        *sample = shared.samples.pop_front().unwrap_or(0.0);
    }

    let consumed_frames = output.len() / channel_count;
    shared.buffered_frames = shared.buffered_frames.saturating_sub(consumed_frames);
    shared.rendered_frames_total = shared
        .rendered_frames_total
        .saturating_add(consumed_frames as u64);

    if consumed_frames == 0 {
        return;
    }

    let Some(format) = shared.format else {
        return;
    };

    let timestamp = info.timestamp();
    let playback_delay = timestamp
        .playback
        .duration_since(&timestamp.callback)
        .unwrap_or(Duration::ZERO);
    let playback_start = Instant::now() + playback_delay;
    shared
        .scheduled_blocks
        .push_back(ScheduledPlaybackBlock::new(
            playback_start,
            consumed_frames,
            format.sample_rate,
        ));
}

fn select_buffer_size(config: &SupportedStreamConfigRange, preferred_frames: usize) -> BufferSize {
    match config.buffer_size() {
        cpal::SupportedBufferSize::Range { min, max } => {
            let preferred = u32::try_from(preferred_frames).unwrap_or(u32::MAX);
            BufferSize::Fixed(preferred.clamp(*min, *max))
        }
        cpal::SupportedBufferSize::Unknown => BufferSize::Default,
    }
}

fn frames_to_duration(frame_count: usize, sample_rate: u32) -> Duration {
    if frame_count == 0 || sample_rate == 0 {
        return Duration::ZERO;
    }

    let nanos = (frame_count as u128)
        .saturating_mul(1_000_000_000)
        .saturating_div(u128::from(sample_rate));
    Duration::from_nanos(u64::try_from(nanos).unwrap_or(u64::MAX))
}

fn duration_to_frames(duration: Duration, sample_rate: u32) -> usize {
    if sample_rate == 0 {
        return 0;
    }

    let frames = duration
        .as_nanos()
        .saturating_mul(u128::from(sample_rate))
        .saturating_div(1_000_000_000);
    usize::try_from(frames).unwrap_or(usize::MAX)
}

#[cfg(test)]
mod tests {
    use std::collections::VecDeque;
    use std::time::{Duration, Instant};

    use super::{ScheduledPlaybackBlock, SharedAudioBuffer};

    #[test]
    fn scheduled_block_reports_partial_audibility() {
        let start = Instant::now()
            .checked_sub(Duration::from_millis(10))
            .unwrap();
        let block = ScheduledPlaybackBlock::new(start, 4_800, 48_000);

        assert_eq!(block.audible_frames_at(Instant::now()), 480);
    }

    #[test]
    fn playback_progress_keeps_future_frames_pending() {
        let now = Instant::now();
        let mut shared = SharedAudioBuffer {
            completed_audible_frames_total: 1_000,
            scheduled_blocks: VecDeque::from([
                ScheduledPlaybackBlock::new(
                    now.checked_sub(Duration::from_millis(5)).unwrap(),
                    480,
                    48_000,
                ),
                ScheduledPlaybackBlock::new(now + Duration::from_millis(5), 480, 48_000),
            ]),
            ..Default::default()
        };

        let progress = shared.playback_progress(now);

        assert!(progress.audible_frames_total >= 1_240);
        assert!(progress.audible_frames_total < 1_480);
        assert!(progress.pending_device_frames >= 480);
    }

    #[test]
    fn pause_playback_clock_freezes_pending_progress() {
        let now = Instant::now();
        let mut shared = SharedAudioBuffer {
            scheduled_blocks: VecDeque::from([ScheduledPlaybackBlock::new(
                now.checked_sub(Duration::from_millis(5)).unwrap(),
                960,
                48_000,
            )]),
            ..Default::default()
        };

        let before_pause = shared.playback_progress(now);
        shared.pause_playback_clock(now);
        let after_pause = shared.playback_progress(now + Duration::from_millis(20));

        assert_eq!(
            after_pause.audible_frames_total,
            before_pause.audible_frames_total
        );
        assert_eq!(
            after_pause.pending_device_frames,
            before_pause.pending_device_frames
        );
    }

    #[test]
    fn resume_playback_clock_shifts_scheduled_blocks_forward() {
        let now = Instant::now();
        let mut shared = SharedAudioBuffer {
            scheduled_blocks: VecDeque::from([ScheduledPlaybackBlock::new(
                now + Duration::from_millis(5),
                960,
                48_000,
            )]),
            ..Default::default()
        };

        shared.pause_playback_clock(now);
        shared.resume_playback_clock(now + Duration::from_millis(20));

        let progress_before_new_start = shared.playback_progress(now + Duration::from_millis(10));
        assert_eq!(progress_before_new_start.audible_frames_total, 0);
        assert_eq!(progress_before_new_start.pending_device_frames, 960);
    }
}
