use std::sync::Arc;

use ffmpeg_next::ffi;
use ffmpeg_next::format;
use ffmpeg_next::frame;
use ffmpeg_next::software::scaling::{context::Context as ScalingContext, Flags as ScalingFlags};
use ffmpeg_next::{Rational, Rescale};

use crate::audio::core::frame::AudioFrame;
use crate::decode::decoder::shared::{OpenedAudioDecoder, OpenedVideoDecoder};
use crate::decode::error::MediaOpenError;
use crate::render::core::frame::{
    DecodedVideoFrame, PixelFormatCategory, VideoColorInfo, VideoColorPrimaries, VideoColorRange,
    VideoFrame, VideoMatrixCoefficients, VideoSurface, VideoTransferCharacteristic,
};
use crate::util::time::MediaTimeUs;

pub(crate) fn map_video_frame(
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

pub(crate) fn map_audio_frame(
    decoder: &mut OpenedAudioDecoder,
    frame: &frame::Audio,
    pts_us: MediaTimeUs,
    duration_us: Option<MediaTimeUs>,
) -> Result<AudioFrame, MediaOpenError> {
    decoder
        .resampler
        .convert(&decoder.decoder, frame, pts_us, duration_us)
}

pub(crate) fn frame_timestamp_us(timestamp: Option<i64>, time_base: Rational) -> MediaTimeUs {
    timestamp.map_or(0, |value| value.rescale(time_base, (1, 1_000_000)))
}

pub(crate) fn frame_duration_us(duration: i64, time_base: Rational) -> Option<MediaTimeUs> {
    if duration <= 0 {
        return None;
    }

    Some(duration.rescale(time_base, (1, 1_000_000)))
}

pub(crate) fn audio_duration_us(frame: &frame::Audio) -> Option<MediaTimeUs> {
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

fn map_d3d11_video_frame(
    decoder: &OpenedVideoDecoder,
    frame: &frame::Video,
    pts_us: MediaTimeUs,
    duration_us: Option<MediaTimeUs>,
) -> Option<DecodedVideoFrame> {
    if decoder.backend != crate::decode::video_decode::VideoDecodeBackend::D3d11va {
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
                    lease: None,
                },
            )
            .with_color_info(color_info),
        ),
    })
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
