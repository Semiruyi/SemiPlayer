use ffmpeg_next as ffmpeg;
use ffmpeg_next::ffi;
use ffmpeg_next::software::scaling::context::Context as ScalingContext;

use crate::audio::core::resampler::NormalizedAudioResampler;
use crate::decode::video_decode::{
    VideoDecodeBackend, VideoDecodeDiagnosticsSnapshot, VideoDecodeFallbackReason,
};
use crate::util::time::MediaTimeUs;

pub(crate) struct OpenedVideoDecoder {
    pub(crate) index: usize,
    pub(crate) decoder: ffmpeg::decoder::Video,
    pub(crate) scaler: Option<ScalingContext>,
    pub(crate) estimated_frame_duration_us: Option<MediaTimeUs>,
    pub(crate) backend: VideoDecodeBackend,
    pub(crate) hardware_requested: bool,
    pub(crate) fallback_reason: VideoDecodeFallbackReason,
    #[allow(dead_code)]
    pub(crate) hardware_context: Option<Box<VideoHardwareContext>>,
}

pub(crate) struct OpenedAudioDecoder {
    pub(crate) index: usize,
    pub(crate) decoder: ffmpeg::decoder::Audio,
    pub(crate) resampler: NormalizedAudioResampler,
}

#[derive(Default)]
#[allow(clippy::struct_excessive_bools)]
pub(crate) struct DecoderDrainingState {
    pub(crate) input_exhausted: bool,
    pub(crate) video_eof_sent: bool,
    pub(crate) audio_eof_sent: bool,
    pub(crate) video_drained: bool,
    pub(crate) audio_drained: bool,
    pub(crate) end_of_stream_emitted: bool,
}

pub(crate) struct MediaPacket {
    pub(crate) stream_index: usize,
    pub(crate) packet: ffmpeg::Packet,
}

pub(crate) struct VideoHardwareContext {
    pub(crate) hw_device_ctx: *mut ffi::AVBufferRef,
    pub(crate) hw_pix_fmt: ffi::AVPixelFormat,
}

impl OpenedVideoDecoder {
    pub(crate) fn diagnostics_snapshot(&self) -> VideoDecodeDiagnosticsSnapshot {
        VideoDecodeDiagnosticsSnapshot {
            backend: self.backend,
            hardware_requested: self.hardware_requested,
            hardware_active: self.backend.is_hardware_accelerated(),
            fallback_reason: self.fallback_reason,
        }
    }
}

impl Drop for VideoHardwareContext {
    fn drop(&mut self) {
        if self.hw_device_ctx.is_null() {
            return;
        }

        unsafe {
            ffi::av_buffer_unref(&mut self.hw_device_ctx);
        }
    }
}
