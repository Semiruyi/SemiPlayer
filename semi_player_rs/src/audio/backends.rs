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

    fn submit(&mut self, chunk: &AudioOutputChunk) -> Result<(), AudioBackendError>;
}
