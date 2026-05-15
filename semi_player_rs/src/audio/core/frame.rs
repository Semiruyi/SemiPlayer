use crate::util::time::MediaTimeUs;
use super::output::AudioStreamFormat;

pub const NORMALIZED_AUDIO_FORMAT: AudioSampleFormatCategory = AudioSampleFormatCategory::F32;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum AudioSampleFormatCategory {
    U8,
    I16,
    I32,
    I64,
    F32,
    F64,
    Unknown,
}

#[derive(Clone, Debug)]
pub struct AudioFrame {
    pub pts_us: MediaTimeUs,
    pub duration_us: Option<MediaTimeUs>,
    pub sample_rate: u32,
    pub channels: u16,
    pub sample_count: usize,
    pub sample_format: AudioSampleFormatCategory,
    pub is_planar: bool,
    pub data: Vec<f32>,
}

impl AudioFrame {
    pub fn end_time_us(&self) -> Option<MediaTimeUs> {
        self.duration_us.map(|duration_us| self.pts_us.saturating_add(duration_us))
    }

    pub fn sample_len(&self) -> usize {
        self.data.len()
    }

    pub fn format(&self) -> AudioStreamFormat {
        AudioStreamFormat {
            sample_rate: self.sample_rate,
            channels: self.channels,
        }
    }
}
