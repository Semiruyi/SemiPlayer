use std::ptr;

use ffmpeg_next as ffmpeg;
use ffmpeg_next::ffi;
use ffmpeg_next::Rational;

use crate::decode::decoder::planner::plan_video_decode;
use crate::decode::decoder::shared::{
    OpenedAudioDecoder, OpenedVideoDecoder, VideoHardwareContext,
};
use crate::decode::error::MediaOpenError;
use crate::decode::policy::{VideoDecodeOpenOptions, VideoDecodeRequirements};
use crate::decode::video_decode::{VideoDecodeBackend, VideoDecodeFallbackReason};
use crate::util::time::MediaTimeUs;

use super::shared;

#[allow(dead_code)]
pub(crate) fn open_video_decoder(
    input: &ffmpeg::format::context::Input,
    stream_index: Option<usize>,
    hw_device_ctx: Option<*mut ffi::AVBufferRef>,
) -> Result<Option<OpenedVideoDecoder>, MediaOpenError> {
    open_video_decoder_with_options(
        input,
        stream_index,
        VideoDecodeOpenOptions {
            requirements: VideoDecodeRequirements::performance(),
            hw_device_ctx,
        },
    )
}

pub(crate) fn open_video_decoder_with_options(
    input: &ffmpeg::format::context::Input,
    stream_index: Option<usize>,
    options: VideoDecodeOpenOptions,
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
    let plan = plan_video_decode(options);

    match plan.preferred_backend {
        VideoDecodeBackend::D3d11va => {
            match try_open_d3d11va_video_decoder(stream_index, &stream, codec, options.hw_device_ctx)?
            {
                Ok(mut decoder) => {
                    decoder.hardware_requested = plan.hardware_requested;
                    Ok(Some(decoder))
                }
                Err(fallback_reason) => {
                    if !plan.allow_fallback {
                        return Ok(None);
                    }

                    let mut decoder = open_software_video_decoder(stream_index, &stream, codec)?;
                    decoder.hardware_requested = plan.hardware_requested;
                    decoder.fallback_reason = fallback_reason;
                    Ok(Some(decoder))
                }
            }
        }
        VideoDecodeBackend::SoftwareBgra | VideoDecodeBackend::Unknown => {
            let mut decoder = open_software_video_decoder(stream_index, &stream, codec)?;
            decoder.hardware_requested = plan.hardware_requested;
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
        resampler: crate::audio::core::resampler::NormalizedAudioResampler::new(),
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

    let hardware_context =
        unsafe { ((*avctx).opaque as *const shared::VideoHardwareContext).as_ref() };
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
