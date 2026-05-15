use crate::util::time::MediaTimeUs;

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
}
