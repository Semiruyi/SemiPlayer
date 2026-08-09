#include "infrastructure/ffmpeg/video_decoder/ffmpeg_video_frame_buffer.hpp"

extern "C" {
#include <libavutil/pixdesc.h>
}

#include <limits>
#include <utility>

namespace semi::infra::ffmpeg::video_decoder {
namespace {

using contracts::media::VideoPixelFormat;

std::size_t plane_height(const AVPixFmtDescriptor& descriptor, int frame_height,
                         std::size_t plane_index) noexcept {
    if (plane_index == 0 || !(descriptor.flags & AV_PIX_FMT_FLAG_PLANAR)) {
        return static_cast<std::size_t>(frame_height);
    }

    const int divisor = 1 << descriptor.log2_chroma_h;
    return static_cast<std::size_t>((frame_height + divisor - 1) / divisor);
}

} // namespace

FfmpegVideoFrameBuffer::FfmpegVideoFrameBuffer(AvFramePtr frame) noexcept
    : frame_(std::move(frame)) {
    if (frame_ == nullptr || frame_->width <= 0 || frame_->height <= 0 || frame_->format < 0) {
        return;
    }

    const auto native_format = static_cast<AVPixelFormat>(frame_->format);
    const auto media_format = media_pixel_format(native_format);
    const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(native_format);
    const int native_plane_count = av_pix_fmt_count_planes(native_format);
    if (!media_format || descriptor == nullptr || native_plane_count <= 0 ||
        native_plane_count > AV_NUM_DATA_POINTERS) {
        return;
    }

    const auto width = static_cast<std::uint32_t>(frame_->width);
    const auto height = static_cast<std::uint32_t>(frame_->height);
    std::array<contracts::media::VideoPlaneView, AV_NUM_DATA_POINTERS> planes{};
    for (int index = 0; index < native_plane_count; ++index) {
        const int native_stride = frame_->linesize[index];
        if (native_stride <= 0) {
            return;
        }

        const auto bytes_per_row = static_cast<std::size_t>(native_stride);
        const auto rows = plane_height(*descriptor, frame_->height, static_cast<std::size_t>(index));
        if (rows > std::numeric_limits<std::size_t>::max() / bytes_per_row) {
            return;
        }

        const auto size_bytes = rows * bytes_per_row;
        if (size_bytes != 0 && frame_->data[index] == nullptr) {
            return;
        }

        planes[static_cast<std::size_t>(index)] = contracts::media::VideoPlaneView{
            .data = reinterpret_cast<const std::byte*>(frame_->data[index]),
            .size_bytes = size_bytes,
            .stride_bytes = static_cast<std::uint32_t>(native_stride),
        };
    }

    pixel_format_ = *media_format;
    width_ = width;
    height_ = height;
    plane_count_ = static_cast<std::size_t>(native_plane_count);
    planes_ = planes;
}

std::optional<VideoPixelFormat>
FfmpegVideoFrameBuffer::media_pixel_format(AVPixelFormat format) noexcept {
    switch (format) {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
        return VideoPixelFormat::Yuv420p;
    case AV_PIX_FMT_YUV422P:
    case AV_PIX_FMT_YUVJ422P:
        return VideoPixelFormat::Yuv422p;
    case AV_PIX_FMT_YUV444P:
    case AV_PIX_FMT_YUVJ444P:
        return VideoPixelFormat::Yuv444p;
    case AV_PIX_FMT_NV12:
        return VideoPixelFormat::Nv12;
    case AV_PIX_FMT_P010LE:
        return VideoPixelFormat::P010;
    case AV_PIX_FMT_RGBA:
        return VideoPixelFormat::Rgba8;
    default:
        return std::nullopt;
    }
}

contracts::media::VideoPixelFormat FfmpegVideoFrameBuffer::pixel_format() const noexcept {
    return pixel_format_;
}

std::uint32_t FfmpegVideoFrameBuffer::width() const noexcept {
    return width_;
}

std::uint32_t FfmpegVideoFrameBuffer::height() const noexcept {
    return height_;
}

std::size_t FfmpegVideoFrameBuffer::plane_count() const noexcept {
    return plane_count_;
}

contracts::media::VideoPlaneView FfmpegVideoFrameBuffer::plane(std::size_t index) const noexcept {
    if (index >= plane_count_) {
        return {};
    }
    return planes_[index];
}

} // namespace semi::infra::ffmpeg::video_decoder
