use std::collections::VecDeque;
use std::sync::Arc;
use std::ptr;

use ffmpeg_next as ffmpeg;
use ffmpeg_next::ffi;
use ffmpeg_next::software::scaling::{context::Context as ScalingContext, Flags as ScalingFlags};
use ffmpeg_next::{format, frame, Packet, Rational, Rescale};

use crate::audio::core::frame::AudioFrame;
use crate::audio::core::resampler::NormalizedAudioResampler;
use crate::core::media::error::MediaOpenError;
use crate::core::media::output::{
    DecodePolicy, DecodedOutput, SkippedAudioFrame, SkippedVideoFrame,
};
use crate::core::media::video_decode::{
    VideoDecodeBackend, VideoDecodeDiagnosticsSnapshot, VideoDecodeFallbackReason,
};
use crate::render::core::frame::{
    DecodedVideoFrame, PixelFormatCategory, VideoColorInfo, VideoColorPrimaries, VideoColorRange,
    VideoFrame, VideoMatrixCoefficients, VideoSurface, VideoTransferCharacteristic,
};
use crate::util::time::MediaTimeUs;

use super::demux_impl::SeekDemuxDiagnostics;

pub(crate) struct OpenedVideoDecoder {
    pub(super) index: usize,
    pub(super) decoder: ffmpeg::decoder::Video,
    pub(super) scaler: Option<ScalingContext>,
    pub(super) estimated_frame_duration_us: Option<MediaTimeUs>,
    pub(super) backend: VideoDecodeBackend,
    pub(super) hardware_requested: bool,
    pub(super) fallback_reason: VideoDecodeFallbackReason,
    #[allow(dead_code)]
    pub(crate) hardware_context: Option<Box<VideoHardwareContext>>,
}

pub(crate) struct OpenedAudioDecoder {
    pub(super) index: usize,
    pub(super) decoder: ffmpeg::decoder::Audio,
    pub(super) resampler: NormalizedAudioResampler,
}

#[derive(Default)]
#[allow(clippy::struct_excessive_bools)]
pub(crate) struct DecoderDrainingState {
    pub(super) input_exhausted: bool,
    pub(super) video_eof_sent: bool,
    pub(super) audio_eof_sent: bool,
    pub(super) video_drained: bool,
    pub(super) audio_drained: bool,
    pub(super) end_of_stream_emitted: bool,
}

pub(crate) struct MediaPacket {
    pub(super) stream_index: usize,
    pub(super) packet: ffmpeg::Packet,
}

pub(crate) struct VideoHardwareContext {
    pub(super) hw_device_ctx: *mut ffi::AVBufferRef,
    pub(super) hw_pix_fmt: ffi::AVPixelFormat,
}

impl OpenedVideoDecoder {
    pub(super) fn diagnostics_snapshot(&self) -> VideoDecodeDiagnosticsSnapshot {
        VideoDecodeDiagnosticsSnapshot {
            backend: self.backend,
            hardware_requested: self.hardware_requested,
            hardware_active: self.backend == VideoDecodeBackend::D3d11va,
            fallback_reason: self.fallback_reason,
        }
    }
}

pub(crate) fn open_video_decoder(
    input: &ffmpeg::format::context::Input,
    stream_index: Option<usize>,
    hw_device_ctx: Option<*mut ffi::AVBufferRef>,
) -> Result<Option<OpenedVideoDecoder>, MediaOpenError> {
    let Some(stream_index) = stream_index else {
        return Ok(None);
    };

    let stream = input
        .stream(stream_index)
        .ok_or(ffmpeg::Error::StreamNotFound)
        .map_err(MediaOpenError::VideoDecoder)?;
    let codec_id = stream.parameters().id();
    let codec = ffmpeg::decoder::find(codec_id)
        .ok_or(ffmpeg::Error::DecoderNotFound)
        .map_err(MediaOpenError::VideoDecoder)?;

    match try_open_d3d11va_video_decoder(stream_index, &stream, codec, hw_device_ctx)? {
        Ok(decoder) => Ok(Some(decoder)),
        Err(fallback_reason) => {
            let mut decoder = open_software_video_decoder(stream_index, &stream, codec)?;
            decoder.hardware_requested = true;
            decoder.fallback_reason = fallback_reason;
            Ok(Some(decoder))
        }
    }
}

pub(crate) fn open_audio_decoder(
    input: &ffmpeg::format::context::Input,
    stream_index: Option<usize>,
) -> Result<Option<OpenedAudioDecoder>, MediaOpenError> {
    let Some(stream_index) = stream_index else {
        return Ok(None);
    };

    let stream = input
        .stream(stream_index)
        .ok_or(ffmpeg::Error::StreamNotFound)
        .map_err(MediaOpenError::AudioDecoder)?;
    let context = ffmpeg::codec::context::Context::from_parameters(stream.parameters())
        .map_err(MediaOpenError::AudioDecoder)?;
    let mut decoder = context.decoder();
    decoder.set_packet_time_base(stream.time_base());
    let decoder = decoder.audio().map_err(MediaOpenError::AudioDecoder)?;

    Ok(Some(OpenedAudioDecoder {
        index: stream_index,
        decoder,
        resampler: NormalizedAudioResampler::new(),
    }))
}

fn open_software_video_decoder(
    stream_index: usize,
    stream: &ffmpeg::Stream<'_>,
    codec: ffmpeg::Codec,
) -> Result<OpenedVideoDecoder, MediaOpenError> {
    let context = ffmpeg::codec::context::Context::from_parameters(stream.parameters())
        .map_err(MediaOpenError::VideoDecoder)?;
    let mut decoder = context.decoder();
    decoder.set_packet_time_base(stream.time_base());
    let decoder = decoder
        .open_as(codec)
        .map_err(MediaOpenError::VideoDecoder)?
        .video()
        .map_err(MediaOpenError::VideoDecoder)?;

    Ok(OpenedVideoDecoder {
        index: stream_index,
        decoder,
        scaler: None,
        estimated_frame_duration_us: estimate_stream_frame_duration_us(stream.avg_frame_rate()),
        backend: VideoDecodeBackend::SoftwareBgra,
        hardware_requested: false,
        fallback_reason: VideoDecodeFallbackReason::None,
        hardware_context: None,
    })
}

fn try_open_d3d11va_video_decoder(
    stream_index: usize,
    stream: &ffmpeg::Stream<'_>,
    codec: ffmpeg::Codec,
    hw_device_ctx: Option<*mut ffi::AVBufferRef>,
) -> Result<Result<OpenedVideoDecoder, VideoDecodeFallbackReason>, MediaOpenError> {
    let Some(hw_pix_fmt) = find_d3d11va_hw_pixel_format(&codec) else {
        return Ok(Err(VideoDecodeFallbackReason::NoHardwareConfig));
    };

    let context = ffmpeg::codec::context::Context::from_parameters(stream.parameters())
        .map_err(MediaOpenError::VideoDecoder)?;
    let mut decoder = context.decoder();
    decoder.set_packet_time_base(stream.time_base());

    let hardware_context = match prepare_d3d11va_context(&mut decoder, hw_pix_fmt, hw_device_ctx) {
        Ok(hardware_context) => hardware_context,
        Err(fallback_reason) => return Ok(Err(fallback_reason)),
    };

    let decoder = match decoder.open_as(codec) {
        Ok(opened) => match opened.video() {
            Ok(video) => video,
            Err(_) => return Ok(Err(VideoDecodeFallbackReason::HwDecoderTypeMismatch)),
        },
        Err(_) => return Ok(Err(VideoDecodeFallbackReason::HwDecoderOpenFailed)),
    };

    Ok(Ok(OpenedVideoDecoder {
        index: stream_index,
        decoder,
        scaler: None,
        estimated_frame_duration_us: estimate_stream_frame_duration_us(stream.avg_frame_rate()),
        backend: VideoDecodeBackend::D3d11va,
        hardware_requested: true,
        fallback_reason: VideoDecodeFallbackReason::None,
        hardware_context: Some(hardware_context),
    }))
}

fn estimate_stream_frame_duration_us(frame_rate: Rational) -> Option<MediaTimeUs> {
    let numerator = i64::from(frame_rate.numerator());
    let denominator = i64::from(frame_rate.denominator());
    if numerator <= 0 || denominator <= 0 {
        return None;
    }

    Some(
        denominator
            .saturating_mul(1_000_000)
            .saturating_div(numerator),
    )
}

fn find_d3d11va_hw_pixel_format(codec: &ffmpeg::Codec) -> Option<ffi::AVPixelFormat> {
    let codec_ptr = unsafe { codec.as_ptr() };
    if codec_ptr.is_null() {
        return None;
    }

    let mut index = 0;
    loop {
        let config = unsafe { ffi::avcodec_get_hw_config(codec_ptr, index) };
        if config.is_null() {
            return None;
        }

        let supports_d3d11va = unsafe {
            (*config).device_type == ffi::AVHWDeviceType::AV_HWDEVICE_TYPE_D3D11VA
                && ((*config).methods & ffi::AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX as i32) != 0
                && matches!(
                    (*config).pix_fmt,
                    ffi::AVPixelFormat::AV_PIX_FMT_D3D11VA_VLD
                        | ffi::AVPixelFormat::AV_PIX_FMT_D3D11
                )
        };
        if supports_d3d11va {
            return Some(unsafe { (*config).pix_fmt });
        }

        index += 1;
    }
}

fn prepare_d3d11va_context(
    decoder: &mut ffmpeg::codec::decoder::Decoder,
    hw_pix_fmt: ffi::AVPixelFormat,
    external_hw_device_ctx: Option<*mut ffi::AVBufferRef>,
) -> Result<Box<VideoHardwareContext>, VideoDecodeFallbackReason> {
    let hw_device_ctx = match external_hw_device_ctx {
        Some(ctx) => {
            let ref_ctx = unsafe { ffi::av_buffer_ref(ctx) };
            if ref_ctx.is_null() {
                return Err(VideoDecodeFallbackReason::HwDeviceCreateFailed);
            }
            ref_ctx
        }
        None => {
            let mut created_ctx = ptr::null_mut();
            let create_result = unsafe {
                ffi::av_hwdevice_ctx_create(
                    &mut created_ctx,
                    ffi::AVHWDeviceType::AV_HWDEVICE_TYPE_D3D11VA,
                    ptr::null(),
                    ptr::null_mut(),
                    0,
                )
            };
            if create_result < 0 || created_ctx.is_null() {
                return Err(VideoDecodeFallbackReason::HwDeviceCreateFailed);
            }
            created_ctx
        }
    };

    let avctx = unsafe { decoder.as_mut_ptr() };
    let mut hardware_context = Box::new(VideoHardwareContext {
        hw_device_ctx,
        hw_pix_fmt,
    });
    let hardware_context_ptr =
        (hardware_context.as_mut() as *mut VideoHardwareContext).cast::<std::ffi::c_void>();

    let avctx_hw_device_ref = unsafe { ffi::av_buffer_ref(hw_device_ctx) };
    if avctx_hw_device_ref.is_null() {
        let mut owned_ref = hw_device_ctx;
        unsafe {
            ffi::av_buffer_unref(&mut owned_ref);
        }
        return Err(VideoDecodeFallbackReason::HwDeviceContextBindFailed);
    }

    unsafe {
        (*avctx).opaque = hardware_context_ptr;
        (*avctx).get_format = Some(select_d3d11va_pixel_format);
        (*avctx).hw_device_ctx = avctx_hw_device_ref;
    }

    Ok(hardware_context)
}

unsafe extern "C" fn select_d3d11va_pixel_format(
    avctx: *mut ffi::AVCodecContext,
    fmt: *const ffi::AVPixelFormat,
) -> ffi::AVPixelFormat {
    if avctx.is_null() || fmt.is_null() {
        return ffi::AVPixelFormat::AV_PIX_FMT_NONE;
    }

    let hardware_context = unsafe { ((*avctx).opaque as *const VideoHardwareContext).as_ref() };
    let Some(hardware_context) = hardware_context else {
        return unsafe { ffi::avcodec_default_get_format(avctx, fmt) };
    };

    let mut current = fmt;
    loop {
        let candidate = unsafe { *current };
        if candidate == ffi::AVPixelFormat::AV_PIX_FMT_NONE {
            break;
        }
        if candidate == hardware_context.hw_pix_fmt {
            return candidate;
        }
        current = unsafe { current.add(1) };
    }

    unsafe { ffi::avcodec_default_get_format(avctx, fmt) }
}

pub(crate) fn decode_video_packet(
    decoder: &mut OpenedVideoDecoder,
    packet: &Packet,
    outputs: &mut VecDeque<DecodedOutput>,
    seek_diagnostics: &mut SeekDemuxDiagnostics,
    policy: DecodePolicy,
) -> Result<(), MediaOpenError> {
    loop {
        match decoder.decoder.send_packet(packet) {
            Ok(()) => {
                let _ = collect_video_frames(decoder, outputs, seek_diagnostics, false, policy)?;
                return Ok(());
            }
            Err(ffmpeg::Error::Other { errno }) if errno == ffmpeg::error::EAGAIN => {
                let output_count_before = outputs.len();
                let drained =
                    collect_video_frames(decoder, outputs, seek_diagnostics, false, policy)?;
                if drained || outputs.len() == output_count_before {
                    return Err(MediaOpenError::SendPacket(ffmpeg::Error::Other {
                        errno: ffmpeg::error::EAGAIN,
                    }));
                }
            }
            Err(error) => return Err(MediaOpenError::SendPacket(error)),
        }
    }
}

pub(crate) fn decode_audio_packet(
    decoder: &mut OpenedAudioDecoder,
    packet: &Packet,
    outputs: &mut VecDeque<DecodedOutput>,
    seek_diagnostics: &mut SeekDemuxDiagnostics,
    policy: DecodePolicy,
) -> Result<(), MediaOpenError> {
    loop {
        match decoder.decoder.send_packet(packet) {
            Ok(()) => {
                let _ = collect_audio_frames(decoder, outputs, seek_diagnostics, false, policy)?;
                return Ok(());
            }
            Err(ffmpeg::Error::Other { errno }) if errno == ffmpeg::error::EAGAIN => {
                let output_count_before = outputs.len();
                let drained =
                    collect_audio_frames(decoder, outputs, seek_diagnostics, false, policy)?;
                if drained || outputs.len() == output_count_before {
                    return Err(MediaOpenError::SendPacket(ffmpeg::Error::Other {
                        errno: ffmpeg::error::EAGAIN,
                    }));
                }
            }
            Err(error) => return Err(MediaOpenError::SendPacket(error)),
        }
    }
}

pub(crate) fn collect_video_frames(
    decoder: &mut OpenedVideoDecoder,
    outputs: &mut VecDeque<DecodedOutput>,
    seek_diagnostics: &mut SeekDemuxDiagnostics,
    draining: bool,
    policy: DecodePolicy,
) -> Result<bool, MediaOpenError> {
    let mut reached_decoder_eof = false;

    loop {
        let mut frame = frame::Video::empty();
        match decoder.decoder.receive_frame(&mut frame) {
            Ok(()) => {
                let time_base = decoder.decoder.packet_time_base();
                let pts_us =
                    frame_timestamp_us(frame.pts().or_else(|| frame.timestamp()), time_base);
                let duration_us = frame_duration_us(frame.packet().duration, time_base)
                    .or(decoder.estimated_frame_duration_us);

                if should_skip_video_frame_for_seek_recovery(policy, pts_us, duration_us) {
                    outputs.push_back(DecodedOutput::SkippedVideo(SkippedVideoFrame {
                        pts_us,
                        duration_us,
                    }));
                    seek_diagnostics.observe_video_frame(true);
                } else {
                    outputs.push_back(DecodedOutput::Video(map_video_frame(
                        decoder,
                        &frame,
                        pts_us,
                        duration_us,
                    )?));
                    seek_diagnostics.observe_video_frame(false);
                }
            }
            Err(ffmpeg::Error::Other { errno }) if errno == ffmpeg::error::EAGAIN => break,
            Err(ffmpeg::Error::Eof) => {
                reached_decoder_eof = true;
                break;
            }
            Err(error) => return Err(MediaOpenError::ReceiveFrame(error)),
        }
    }

    Ok(draining && reached_decoder_eof)
}

pub(crate) fn collect_audio_frames(
    decoder: &mut OpenedAudioDecoder,
    outputs: &mut VecDeque<DecodedOutput>,
    seek_diagnostics: &mut SeekDemuxDiagnostics,
    draining: bool,
    policy: DecodePolicy,
) -> Result<bool, MediaOpenError> {
    let mut reached_decoder_eof = false;

    loop {
        let mut frame = frame::Audio::empty();
        match decoder.decoder.receive_frame(&mut frame) {
            Ok(()) => {
                let time_base = decoder.decoder.packet_time_base();
                let pts_us =
                    frame_timestamp_us(frame.pts().or_else(|| frame.timestamp()), time_base);
                let duration_us = audio_duration_us(&frame);

                if should_skip_audio_frame_for_seek_recovery(policy, pts_us, duration_us) {
                    outputs.push_back(DecodedOutput::SkippedAudio(SkippedAudioFrame {
                        pts_us,
                        duration_us,
                    }));
                    seek_diagnostics.observe_audio_frame(true);
                    continue;
                }

                outputs.push_back(DecodedOutput::Audio(map_audio_frame(
                    decoder,
                    &frame,
                    pts_us,
                    duration_us,
                )?));
                seek_diagnostics.observe_audio_frame(false);
            }
            Err(ffmpeg::Error::Other { errno }) if errno == ffmpeg::error::EAGAIN => break,
            Err(ffmpeg::Error::Eof) => {
                reached_decoder_eof = true;
                break;
            }
            Err(error) => return Err(MediaOpenError::ReceiveFrame(error)),
        }
    }

    Ok(draining && reached_decoder_eof)
}

pub(crate) fn send_video_decoder_eof(
    decoder: &mut ffmpeg::decoder::Video,
) -> Result<(), MediaOpenError> {
    match decoder.send_eof() {
        Ok(()) | Err(ffmpeg::Error::Eof) => Ok(()),
        Err(error) => Err(MediaOpenError::SendPacket(error)),
    }
}

pub(crate) fn send_audio_decoder_eof(
    decoder: &mut ffmpeg::decoder::Audio,
) -> Result<(), MediaOpenError> {
    match decoder.send_eof() {
        Ok(()) | Err(ffmpeg::Error::Eof) => Ok(()),
        Err(error) => Err(MediaOpenError::SendPacket(error)),
    }
}

fn map_video_frame(
    decoder: &mut OpenedVideoDecoder,
    frame: &frame::Video,
    pts_us: MediaTimeUs,
    duration_us: Option<MediaTimeUs>,
) -> Result<DecodedVideoFrame, MediaOpenError> {
    if let Some(mapped) = map_d3d11_video_frame(decoder, frame, pts_us, duration_us) {
        return Ok(mapped);
    }

    let converted = convert_video_frame_to_bgra(decoder, frame)?;
    let stride = converted.stride(0);
    let data = copy_packed_plane(&converted);

    Ok(VideoFrame {
        pts_us,
        duration_us,
        width: converted.width(),
        height: converted.height(),
        is_key_frame: frame.is_key(),
        surface: Arc::new(
            VideoSurface::new_cpu_packed(PixelFormatCategory::Bgra8, stride, data)
                .with_color_info(video_color_info_from_av_frame(unsafe { frame.as_ptr() })),
        ),
    })
}

fn map_d3d11_video_frame(
    decoder: &OpenedVideoDecoder,
    frame: &frame::Video,
    pts_us: MediaTimeUs,
    duration_us: Option<MediaTimeUs>,
) -> Option<DecodedVideoFrame> {
    if decoder.backend != VideoDecodeBackend::D3d11va {
        return None;
    }

    if !matches!(
        frame.format(),
        format::Pixel::D3D11VA_VLD | format::Pixel::D3D11
    ) {
        return None;
    }

    let av_frame = unsafe { frame.as_ptr() };
    if av_frame.is_null() {
        return None;
    }

    let texture_ptr = unsafe { (*av_frame).data[0] as usize as u64 };
    if texture_ptr == 0 {
        return None;
    }

    let array_slice = unsafe { (*av_frame).data[1] as usize as u32 };
    let pixel_format = d3d11_hw_sw_format(av_frame).unwrap_or(PixelFormatCategory::Nv12);
    let color_info = video_color_info_from_av_frame(av_frame);

    Some(VideoFrame {
        pts_us,
        duration_us,
        width: frame.width(),
        height: frame.height(),
        is_key_frame: frame.is_key(),
        surface: Arc::new(
            VideoSurface::new_gpu_texture(
                pixel_format,
                crate::render::gpu::GpuTextureData::D3d11 {
                    texture_ptr,
                    shared_handle: None,
                    array_slice,
                },
            )
            .with_color_info(color_info),
        ),
    })
}

fn map_audio_frame(
    decoder: &mut OpenedAudioDecoder,
    frame: &frame::Audio,
    pts_us: MediaTimeUs,
    duration_us: Option<MediaTimeUs>,
) -> Result<AudioFrame, MediaOpenError> {
    decoder
        .resampler
        .convert(&decoder.decoder, frame, pts_us, duration_us)
}

fn frame_timestamp_us(timestamp: Option<i64>, time_base: Rational) -> MediaTimeUs {
    timestamp.map_or(0, |value| value.rescale(time_base, (1, 1_000_000)))
}

fn frame_duration_us(duration: i64, time_base: Rational) -> Option<MediaTimeUs> {
    if duration <= 0 {
        return None;
    }

    Some(duration.rescale(time_base, (1, 1_000_000)))
}

fn audio_duration_us(frame: &frame::Audio) -> Option<MediaTimeUs> {
    if frame.rate() == 0 || frame.samples() == 0 {
        return None;
    }

    let samples = i64::try_from(frame.samples()).ok()?;
    Some(
        samples
            .saturating_mul(1_000_000)
            .saturating_div(i64::from(frame.rate())),
    )
}

pub(crate) fn should_skip_video_frame_for_seek_recovery(
    policy: DecodePolicy,
    pts_us: MediaTimeUs,
    duration_us: Option<MediaTimeUs>,
) -> bool {
    let Some(seek_recovery) = policy.seek_recovery else {
        return false;
    };

    let Some(end_us) = duration_us.and_then(|duration_us| pts_us.checked_add(duration_us)) else {
        return false;
    };

    end_us <= seek_recovery.target_video_us
}

pub(crate) fn should_skip_audio_frame_for_seek_recovery(
    policy: DecodePolicy,
    pts_us: MediaTimeUs,
    duration_us: Option<MediaTimeUs>,
) -> bool {
    let Some(seek_recovery) = policy.seek_recovery else {
        return false;
    };

    let Some(end_us) = duration_us.and_then(|duration_us| pts_us.checked_add(duration_us)) else {
        return false;
    };

    end_us <= seek_recovery.target_video_us
}

fn convert_video_frame_to_bgra(
    decoder: &mut OpenedVideoDecoder,
    input: &frame::Video,
) -> Result<frame::Video, MediaOpenError> {
    ensure_video_scaler(decoder, input)?;

    let mut output = frame::Video::empty();
    decoder
        .scaler
        .as_mut()
        .expect("video scaler initialized")
        .run(input, &mut output)
        .map_err(MediaOpenError::ScaleFrame)?;

    Ok(output)
}

fn ensure_video_scaler(
    decoder: &mut OpenedVideoDecoder,
    input: &frame::Video,
) -> Result<(), MediaOpenError> {
    let needs_rebuild = decoder.scaler.as_ref().is_none_or(|scaler| {
        scaler.input().format != input.format()
            || scaler.input().width != input.width()
            || scaler.input().height != input.height()
            || scaler.output().format != format::Pixel::BGRA
            || scaler.output().width != input.width()
            || scaler.output().height != input.height()
    });

    if needs_rebuild {
        decoder.scaler = Some(
            ScalingContext::get(
                input.format(),
                input.width(),
                input.height(),
                format::Pixel::BGRA,
                input.width(),
                input.height(),
                ScalingFlags::BILINEAR,
            )
            .map_err(MediaOpenError::ScaleFrame)?,
        );
    }

    Ok(())
}

fn copy_packed_plane(frame: &frame::Video) -> Vec<u8> {
    let stride = frame.stride(0);
    let height = usize::try_from(frame.height()).unwrap_or(0);
    let byte_len = stride.saturating_mul(height);
    let data = frame.data(0);

    data[..byte_len.min(data.len())].to_vec()
}

fn d3d11_hw_sw_format(av_frame: *const ffi::AVFrame) -> Option<PixelFormatCategory> {
    if av_frame.is_null() {
        return None;
    }

    let hw_frames_ctx = unsafe { (*av_frame).hw_frames_ctx };
    if hw_frames_ctx.is_null() {
        return None;
    }

    let frames_ctx = unsafe { (*hw_frames_ctx).data as *const ffi::AVHWFramesContext };
    if frames_ctx.is_null() {
        return None;
    }

    match unsafe { (*frames_ctx).sw_format } {
        ffi::AVPixelFormat::AV_PIX_FMT_NV12 => Some(PixelFormatCategory::Nv12),
        _ => None,
    }
}

fn video_color_info_from_av_frame(av_frame: *const ffi::AVFrame) -> VideoColorInfo {
    if av_frame.is_null() {
        return VideoColorInfo::default();
    }

    VideoColorInfo {
        range: map_av_color_range(unsafe { (*av_frame).color_range }),
        primaries: map_av_color_primaries(unsafe { (*av_frame).color_primaries }),
        transfer: map_av_color_transfer(unsafe { (*av_frame).color_trc }),
        matrix: map_av_color_space(unsafe { (*av_frame).colorspace }),
    }
}

fn map_av_color_range(range: ffi::AVColorRange) -> VideoColorRange {
    match range {
        ffi::AVColorRange::AVCOL_RANGE_MPEG => VideoColorRange::Limited,
        ffi::AVColorRange::AVCOL_RANGE_JPEG => VideoColorRange::Full,
        _ => VideoColorRange::Unknown,
    }
}

fn map_av_color_primaries(primaries: ffi::AVColorPrimaries) -> VideoColorPrimaries {
    match primaries {
        ffi::AVColorPrimaries::AVCOL_PRI_BT709 => VideoColorPrimaries::Bt709,
        ffi::AVColorPrimaries::AVCOL_PRI_BT470BG | ffi::AVColorPrimaries::AVCOL_PRI_SMPTE170M => {
            VideoColorPrimaries::Bt601
        }
        ffi::AVColorPrimaries::AVCOL_PRI_BT2020 => VideoColorPrimaries::Bt2020,
        _ => VideoColorPrimaries::Unknown,
    }
}

fn map_av_color_transfer(
    transfer: ffi::AVColorTransferCharacteristic,
) -> VideoTransferCharacteristic {
    match transfer {
        ffi::AVColorTransferCharacteristic::AVCOL_TRC_BT709 => VideoTransferCharacteristic::Bt709,
        ffi::AVColorTransferCharacteristic::AVCOL_TRC_IEC61966_2_1 => {
            VideoTransferCharacteristic::Srgb
        }
        ffi::AVColorTransferCharacteristic::AVCOL_TRC_SMPTE2084 => VideoTransferCharacteristic::Pq,
        ffi::AVColorTransferCharacteristic::AVCOL_TRC_ARIB_STD_B67 => {
            VideoTransferCharacteristic::Hlg
        }
        _ => VideoTransferCharacteristic::Unknown,
    }
}

fn map_av_color_space(space: ffi::AVColorSpace) -> VideoMatrixCoefficients {
    match space {
        ffi::AVColorSpace::AVCOL_SPC_BT709 => VideoMatrixCoefficients::Bt709,
        ffi::AVColorSpace::AVCOL_SPC_BT470BG | ffi::AVColorSpace::AVCOL_SPC_SMPTE170M => {
            VideoMatrixCoefficients::Bt601
        }
        ffi::AVColorSpace::AVCOL_SPC_BT2020_NCL => VideoMatrixCoefficients::Bt2020Ncl,
        ffi::AVColorSpace::AVCOL_SPC_RGB => VideoMatrixCoefficients::Rgb,
        _ => VideoMatrixCoefficients::Unknown,
    }
}

#[cfg(test)]
mod tests {
    use super::{
        should_skip_audio_frame_for_seek_recovery, should_skip_video_frame_for_seek_recovery,
    };
    use crate::core::media::{DecodePolicy, SeekRecoveryPolicy};

    #[test]
    fn seek_recovery_skips_frame_that_ends_before_target() {
        let policy = DecodePolicy {
            seek_recovery: Some(SeekRecoveryPolicy {
                target_video_us: 10_000,
            }),
        };

        assert!(should_skip_video_frame_for_seek_recovery(
            policy,
            5_000,
            Some(4_000),
        ));
    }

    #[test]
    fn seek_recovery_keeps_frame_that_covers_target() {
        let policy = DecodePolicy {
            seek_recovery: Some(SeekRecoveryPolicy {
                target_video_us: 10_000,
            }),
        };

        assert!(!should_skip_video_frame_for_seek_recovery(
            policy,
            5_000,
            Some(6_000),
        ));
    }

    #[test]
    fn seek_recovery_skips_audio_frame_that_ends_before_target() {
        let policy = DecodePolicy {
            seek_recovery: Some(SeekRecoveryPolicy {
                target_video_us: 10_000,
            }),
        };

        assert!(should_skip_audio_frame_for_seek_recovery(
            policy,
            5_000,
            Some(4_000),
        ));
    }

    #[test]
    fn seek_recovery_keeps_audio_frame_that_covers_target() {
        let policy = DecodePolicy {
            seek_recovery: Some(SeekRecoveryPolicy {
                target_video_us: 10_000,
            }),
        };

        assert!(!should_skip_audio_frame_for_seek_recovery(
            policy,
            5_000,
            Some(6_000),
        ));
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
