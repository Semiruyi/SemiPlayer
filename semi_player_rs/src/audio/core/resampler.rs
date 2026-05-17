use ffmpeg_next as ffmpeg;
use ffmpeg_next::software::resampling::Context as ResamplingContext;
use ffmpeg_next::{format, frame, ChannelLayout};

use crate::audio::core::frame::{AudioFrame, NORMALIZED_AUDIO_FORMAT};
use crate::util::time::MediaTimeUs;

use crate::core::media::MediaOpenError;

pub struct NormalizedAudioResampler {
    context: Option<ResamplingContext>,
}

impl NormalizedAudioResampler {
    pub fn new() -> Self {
        Self { context: None }
    }

    pub fn convert(
        &mut self,
        decoder: &ffmpeg::decoder::Audio,
        input: &frame::Audio,
        pts_us: MediaTimeUs,
        duration_us: Option<MediaTimeUs>,
    ) -> Result<AudioFrame, MediaOpenError> {
        let input_layout = resolve_channel_layout(decoder, input);
        self.ensure_context(input.format(), input_layout, input.rate(), input.channels())?;

        let mut output = frame::Audio::empty();
        self.context
            .as_mut()
            .expect("audio resampler initialized")
            .run(input, &mut output)
            .map_err(MediaOpenError::ResampleFrame)?;

        Ok(AudioFrame {
            pts_us,
            duration_us: duration_us
                .or_else(|| normalized_audio_duration_us(output.rate(), output.samples())),
            sample_rate: output.rate(),
            channels: output.channels(),
            sample_count: output.samples(),
            sample_format: NORMALIZED_AUDIO_FORMAT,
            is_planar: false,
            data: copy_packed_f32_samples(&output),
        })
    }

    fn ensure_context(
        &mut self,
        input_format: format::Sample,
        input_layout: ChannelLayout,
        input_rate: u32,
        input_channels: u16,
    ) -> Result<(), MediaOpenError> {
        let output_layout = ChannelLayout::default(i32::from(input_channels));
        let needs_rebuild = self
            .context
            .as_ref()
            .is_none_or(|context| {
                context.input().format != input_format
                    || context.input().channel_layout != input_layout
                    || context.input().rate != input_rate
                    || context.output().format != format::Sample::F32(format::sample::Type::Packed)
                    || context.output().channel_layout != output_layout
                    || context.output().rate != input_rate
            });

        if needs_rebuild {
            self.context = Some(
                ResamplingContext::get(
                    input_format,
                    input_layout,
                    input_rate,
                    format::Sample::F32(format::sample::Type::Packed),
                    output_layout,
                    input_rate,
                )
                .map_err(MediaOpenError::ResampleFrame)?,
            );
        }

        Ok(())
    }
}

impl Default for NormalizedAudioResampler {
    fn default() -> Self {
        Self::new()
    }
}

fn resolve_channel_layout(decoder: &ffmpeg::decoder::Audio, input: &frame::Audio) -> ChannelLayout {
    let frame_layout = input.channel_layout();
    if !frame_layout.is_empty() {
        return frame_layout;
    }

    let decoder_layout = decoder.channel_layout();
    if !decoder_layout.is_empty() {
        return decoder_layout;
    }

    ChannelLayout::default(i32::from(input.channels()))
}

fn copy_packed_f32_samples(frame: &frame::Audio) -> Vec<f32> {
    let sample_len = frame
        .samples()
        .saturating_mul(usize::from(frame.channels()));
    let expected_bytes = sample_len.saturating_mul(std::mem::size_of::<f32>());
    let data = frame.data(0);
    let byte_len = expected_bytes.min(data.len());

    data[..byte_len]
        .chunks_exact(std::mem::size_of::<f32>())
        .map(|chunk| f32::from_ne_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]))
        .collect()
}

fn normalized_audio_duration_us(rate: u32, samples: usize) -> Option<MediaTimeUs> {
    if rate == 0 || samples == 0 {
        return None;
    }

    let samples = i64::try_from(samples).ok()?;
    Some(
        samples
            .saturating_mul(1_000_000)
            .saturating_div(i64::from(rate)),
    )
}

#[cfg(test)]
mod tests {
    use super::normalized_audio_duration_us;

    #[test]
    fn normalized_duration_matches_sample_rate() {
        assert_eq!(normalized_audio_duration_us(48_000, 480), Some(10_000));
    }
}
