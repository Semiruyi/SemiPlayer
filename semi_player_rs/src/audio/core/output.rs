use crate::util::time::MediaTimeUs;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct AudioStreamFormat {
    pub sample_rate: u32,
    pub channels: u16,
}

impl AudioStreamFormat {
    pub const fn sample_stride(self) -> usize {
        self.channels as usize
    }
}

#[derive(Clone, Debug, Default)]
pub struct AudioOutputChunk {
    pub pts_us: Option<MediaTimeUs>,
    pub sample_rate: u32,
    pub channels: u16,
    pub frame_count: usize,
    pub samples: Vec<f32>,
}

impl AudioOutputChunk {
    pub fn format(&self) -> Option<AudioStreamFormat> {
        if self.sample_rate == 0 || self.channels == 0 {
            return None;
        }

        Some(AudioStreamFormat {
            sample_rate: self.sample_rate,
            channels: self.channels,
        })
    }

    pub fn is_empty(&self) -> bool {
        self.frame_count == 0 || self.samples.is_empty()
    }
}
