use super::output::AudioStreamFormat;
use crate::util::time::MediaTimeUs;

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
        self.duration_us
            .map(|duration_us| self.pts_us.saturating_add(duration_us))
    }

    pub fn trim_to_start_us(&self, target_us: MediaTimeUs) -> Option<Self> {
        if target_us <= self.pts_us {
            return Some(self.clone());
        }

        let end_time_us = self
            .end_time_us()
            .or_else(|| normalized_duration_us(self.sample_rate, self.sample_count)
                .map(|duration_us| self.pts_us.saturating_add(duration_us)))?;
        if end_time_us <= target_us {
            return None;
        }

        let channel_count = usize::from(self.channels);
        if self.sample_rate == 0 || channel_count == 0 {
            return Some(self.clone());
        }

        let delta_us = target_us.saturating_sub(self.pts_us);
        let frames_to_drop = ceil_frames_for_duration_us(delta_us, self.sample_rate)
            .min(self.sample_count);
        if frames_to_drop == 0 {
            return Some(self.clone());
        }

        let remaining_frames = self.sample_count.saturating_sub(frames_to_drop);
        if remaining_frames == 0 {
            return None;
        }

        let start_sample = frames_to_drop.saturating_mul(channel_count).min(self.data.len());
        let dropped_us = duration_for_frames(frames_to_drop, self.sample_rate).unwrap_or(0);

        Some(Self {
            pts_us: self.pts_us.saturating_add(dropped_us),
            duration_us: normalized_duration_us(self.sample_rate, remaining_frames),
            sample_rate: self.sample_rate,
            channels: self.channels,
            sample_count: remaining_frames,
            sample_format: self.sample_format,
            is_planar: self.is_planar,
            data: self.data[start_sample..].to_vec(),
        })
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

fn normalized_duration_us(sample_rate: u32, sample_count: usize) -> Option<MediaTimeUs> {
    if sample_rate == 0 || sample_count == 0 {
        return None;
    }

    Some(
        (sample_count as i64)
            .saturating_mul(1_000_000)
            .saturating_div(i64::from(sample_rate)),
    )
}

fn duration_for_frames(frame_count: usize, sample_rate: u32) -> Option<MediaTimeUs> {
    normalized_duration_us(sample_rate, frame_count)
}

fn ceil_frames_for_duration_us(duration_us: MediaTimeUs, sample_rate: u32) -> usize {
    if duration_us <= 0 || sample_rate == 0 {
        return 0;
    }

    let numerator = (duration_us as u128)
        .saturating_mul(u128::from(sample_rate))
        .saturating_add(999_999);
    usize::try_from(numerator / 1_000_000).unwrap_or(usize::MAX)
}

#[cfg(test)]
mod tests {
    use super::{AudioFrame, AudioSampleFormatCategory};

    fn frame(pts_us: i64, samples: usize) -> AudioFrame {
        AudioFrame {
            pts_us,
            duration_us: Some((samples as i64) * 1_000),
            sample_rate: 1_000,
            channels: 2,
            sample_count: samples,
            sample_format: AudioSampleFormatCategory::F32,
            is_planar: false,
            data: (0..samples * 2).map(|index| index as f32).collect(),
        }
    }

    #[test]
    fn trim_to_start_us_drops_fully_consumed_frame() {
        assert!(frame(0, 4).trim_to_start_us(4_000).is_none());
    }

    #[test]
    fn trim_to_start_us_keeps_overlapping_tail() {
        let trimmed = frame(0, 4).trim_to_start_us(1_500).expect("trimmed");

        assert_eq!(trimmed.pts_us, 2_000);
        assert_eq!(trimmed.sample_count, 2);
        assert_eq!(trimmed.duration_us, Some(2_000));
        assert_eq!(trimmed.data, vec![4.0, 5.0, 6.0, 7.0]);
    }
}
