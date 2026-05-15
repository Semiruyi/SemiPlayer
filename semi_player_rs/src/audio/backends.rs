use std::collections::VecDeque;
use std::sync::{Arc, Mutex};

use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};
use cpal::{BufferSize, SampleFormat, SampleRate, Stream, StreamConfig, SupportedStreamConfigRange};

use crate::audio::core::output::{AudioOutputChunk, AudioStreamFormat};

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

    fn buffered_frames(&self) -> usize;
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
                move |output: &mut [f32], _| fill_output_buffer(output, &shared),
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
        Ok(())
    }

    fn start(&mut self) -> Result<(), AudioBackendError> {
        let stream = self.stream.as_ref().ok_or(AudioBackendError::NotConfigured)?;
        stream.play().map_err(|_| AudioBackendError::BackendFailure)?;
        self.started = true;
        Ok(())
    }

    fn pause(&mut self) -> Result<(), AudioBackendError> {
        let stream = self.stream.as_ref().ok_or(AudioBackendError::NotConfigured)?;
        stream.pause().map_err(|_| AudioBackendError::BackendFailure)?;
        self.started = false;
        Ok(())
    }

    fn stop(&mut self) -> Result<(), AudioBackendError> {
        if let Some(stream) = self.stream.as_ref() {
            stream.pause().map_err(|_| AudioBackendError::BackendFailure)?;
        }
        self.started = false;
        self.clear_buffer()
    }

    fn clear_buffer(&mut self) -> Result<(), AudioBackendError> {
        let mut shared = self.shared.lock().unwrap();
        shared.samples.clear();
        shared.buffered_frames = 0;
        Ok(())
    }

    fn submit(&mut self, chunk: &AudioOutputChunk) -> Result<(), AudioBackendError> {
        let configured_format = self.configured_format.ok_or(AudioBackendError::NotConfigured)?;
        let chunk_format = chunk.format().ok_or(AudioBackendError::InvalidFormat)?;
        if configured_format != chunk_format {
            return Err(AudioBackendError::InvalidFormat);
        }

        let mut shared = self.shared.lock().unwrap();
        shared.samples.extend(chunk.samples.iter().copied());
        shared.buffered_frames = shared.buffered_frames.saturating_add(chunk.frame_count);
        Ok(())
    }

    fn buffered_frames(&self) -> usize {
        self.shared.lock().unwrap().buffered_frames
    }
}

#[derive(Default)]
struct SharedAudioBuffer {
    format: Option<AudioStreamFormat>,
    samples: VecDeque<f32>,
    buffered_frames: usize,
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

fn fill_output_buffer(output: &mut [f32], shared: &Arc<Mutex<SharedAudioBuffer>>) {
    let mut shared = shared.lock().unwrap();
    let channel_count = shared
        .format
        .map(|format| format.sample_stride())
        .unwrap_or(1)
        .max(1);

    for sample in output.iter_mut() {
        *sample = shared.samples.pop_front().unwrap_or(0.0);
    }

    let consumed_frames = output.len() / channel_count;
    shared.buffered_frames = shared.buffered_frames.saturating_sub(consumed_frames);
}

fn select_buffer_size(
    config: &SupportedStreamConfigRange,
    preferred_frames: usize,
) -> BufferSize {
    match config.buffer_size() {
        cpal::SupportedBufferSize::Range { min, max } => {
            let preferred = preferred_frames as u32;
            BufferSize::Fixed(preferred.clamp(*min, *max))
        }
        cpal::SupportedBufferSize::Unknown => BufferSize::Default,
    }
}
