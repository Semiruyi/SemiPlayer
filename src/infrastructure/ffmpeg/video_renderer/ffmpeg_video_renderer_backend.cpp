#include "infrastructure/ffmpeg/video_renderer/ffmpeg_video_renderer_backend.hpp"

extern "C" {
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
}

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>

namespace semi::infra::ffmpeg::video_renderer {
namespace {

using contracts::media::DecodedVideo;
using contracts::media::RenderedVideo;
using contracts::media::VideoPixelFormat;
using contracts::video_renderer::VideoRendererBackendError;
using contracts::video_renderer::VideoRendererBackendOperation;
using contracts::video_renderer::VideoRendererOptions;

struct SwsContextDeleter {
    void operator()(SwsContext* context) const noexcept {
        sws_freeContext(context);
    }
};

using SwsContextPtr = std::unique_ptr<SwsContext, SwsContextDeleter>;

std::string ffmpeg_message(int error_code) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    if (av_strerror(error_code, buffer.data(), buffer.size()) < 0) {
        return "FFmpeg error " + std::to_string(error_code);
    }
    return buffer.data();
}

VideoRendererBackendError make_error(VideoRendererBackendOperation operation,
                                     int error_code,
                                     std::string message = {}) {
    if (message.empty()) {
        message = ffmpeg_message(error_code);
    }
    return VideoRendererBackendError{
        .operation = operation,
        .native_code = error_code,
        .message = std::move(message),
    };
}

VideoRendererBackendError make_state_error(VideoRendererBackendOperation operation,
                                           const char* message) {
    return make_error(operation,
                      AVERROR(EINVAL),
                      message);
}

std::optional<AVPixelFormat> native_pixel_format(VideoPixelFormat format) noexcept {
    switch (format) {
    case VideoPixelFormat::Yuv420p:
        return AV_PIX_FMT_YUV420P;
    case VideoPixelFormat::Yuv422p:
        return AV_PIX_FMT_YUV422P;
    case VideoPixelFormat::Yuv444p:
        return AV_PIX_FMT_YUV444P;
    case VideoPixelFormat::Nv12:
        return AV_PIX_FMT_NV12;
    case VideoPixelFormat::P010:
        return AV_PIX_FMT_P010LE;
    case VideoPixelFormat::Rgba8:
        return AV_PIX_FMT_RGBA;
    case VideoPixelFormat::Unknown:
        return std::nullopt;
    }
    return std::nullopt;
}

std::size_t plane_height(VideoPixelFormat format,
                         std::uint32_t height,
                         std::size_t plane) noexcept {
    switch (format) {
    case VideoPixelFormat::Yuv420p:
        return plane == 0 ? height : (static_cast<std::size_t>(height) + 1U) / 2U;
    case VideoPixelFormat::Yuv422p:
        return plane == 0 ? height : (static_cast<std::size_t>(height) + 1U) / 2U;
    case VideoPixelFormat::Yuv444p:
    case VideoPixelFormat::Rgba8:
        return height;
    case VideoPixelFormat::Nv12:
    case VideoPixelFormat::P010:
        return plane == 0 ? height : (static_cast<std::size_t>(height) + 1U) / 2U;
    case VideoPixelFormat::Unknown:
        return 0;
    }
    return 0;
}

std::size_t plane_width(VideoPixelFormat format,
                        std::uint32_t width,
                        std::size_t plane) noexcept {
    switch (format) {
    case VideoPixelFormat::Yuv420p:
    case VideoPixelFormat::Yuv422p:
        return plane == 0 ? width : (static_cast<std::size_t>(width) + 1U) / 2U;
    case VideoPixelFormat::Yuv444p:
        return width;
    case VideoPixelFormat::Nv12:
        return width;
    case VideoPixelFormat::P010:
        return static_cast<std::size_t>(width) * 2U;
    case VideoPixelFormat::Rgba8:
        return static_cast<std::size_t>(width) * 4U;
    case VideoPixelFormat::Unknown:
        return 0;
    }
    return 0;
}

std::size_t expected_plane_count(VideoPixelFormat format) noexcept {
    switch (format) {
    case VideoPixelFormat::Yuv420p:
    case VideoPixelFormat::Yuv422p:
    case VideoPixelFormat::Yuv444p:
        return 3;
    case VideoPixelFormat::Nv12:
    case VideoPixelFormat::P010:
        return 2;
    case VideoPixelFormat::Rgba8:
        return 1;
    case VideoPixelFormat::Unknown:
        return 0;
    }
    return 0;
}

bool valid_dimension(std::uint32_t value) noexcept {
    return value > 0 && value <= static_cast<std::uint32_t>(std::numeric_limits<int>::max());
}

bool valid_source(const DecodedVideo& input,
                  VideoPixelFormat source_format,
                  std::uint32_t width,
                  std::uint32_t height) noexcept {
    if (!input.buffer || !valid_dimension(width) || !valid_dimension(height)) {
        return false;
    }

    const auto expected_planes = expected_plane_count(source_format);
    if (expected_planes == 0 || input.buffer->plane_count() != expected_planes) {
        return false;
    }

    for (std::size_t index = 0; index < expected_planes; ++index) {
        const auto plane = input.buffer->plane(index);
        const auto row_bytes = plane_width(source_format, width, index);
        const auto rows = plane_height(source_format, height, index);
        if (row_bytes == 0 || rows == 0 || plane.data == nullptr ||
            plane.stride_bytes < row_bytes ||
            plane.stride_bytes > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
            return false;
        }
        if (rows > std::numeric_limits<std::size_t>::max() / plane.stride_bytes ||
            plane.size_bytes < rows * plane.stride_bytes) {
            return false;
        }
    }
    return true;
}

} // namespace

struct FfmpegVideoRendererBackend::Impl {
    SwsContextPtr context;
    VideoRendererOptions options{};
    VideoPixelFormat source_format = VideoPixelFormat::Unknown;
    std::uint32_t source_width = 0;
    std::uint32_t source_height = 0;
    std::uint32_t output_width = 0;
    std::uint32_t output_height = 0;
    bool configured = false;
};

FfmpegVideoRendererBackend::FfmpegVideoRendererBackend() : impl_(std::make_unique<Impl>()) {}

FfmpegVideoRendererBackend::~FfmpegVideoRendererBackend() {
    unconfigure();
}

std::expected<void, VideoRendererBackendError>
FfmpegVideoRendererBackend::configure(const VideoRendererOptions& options) {
    if (impl_->configured) {
        return std::unexpected(make_state_error(
            VideoRendererBackendOperation::Configure,
            "video renderer backend is already configured"));
    }
    if (options.output_pixel_format != VideoPixelFormat::Rgba8 ||
        (options.output_width != 0 && !valid_dimension(options.output_width)) ||
        (options.output_height != 0 && !valid_dimension(options.output_height))) {
        return std::unexpected(make_state_error(
            VideoRendererBackendOperation::Configure,
            "video renderer output must be RGBA8 with valid optional dimensions"));
    }

    impl_->options = options;
    impl_->configured = true;
    return {};
}

std::expected<RenderedVideo, VideoRendererBackendError>
FfmpegVideoRendererBackend::render(const DecodedVideo& input) {
    if (!impl_->configured) {
        return std::unexpected(make_state_error(
            VideoRendererBackendOperation::Render,
            "video renderer backend is not configured"));
    }
    if (!input.buffer) {
        return std::unexpected(make_state_error(
            VideoRendererBackendOperation::Render,
            "decoded video frame has no buffer"));
    }

    const auto source_format = input.buffer->pixel_format();
    const auto source_native = native_pixel_format(source_format);
    if (!source_native) {
        return std::unexpected(make_state_error(
            VideoRendererBackendOperation::Render,
            "decoded video pixel format is unsupported"));
    }

    const auto source_width = input.buffer->width();
    const auto source_height = input.buffer->height();
    if (!valid_source(input, source_format, source_width, source_height)) {
        return std::unexpected(make_state_error(
            VideoRendererBackendOperation::Render,
            "decoded video planes are invalid"));
    }

    const auto output_width = impl_->options.output_width == 0
                                  ? source_width
                                  : impl_->options.output_width;
    const auto output_height = impl_->options.output_height == 0
                                   ? source_height
                                   : impl_->options.output_height;
    if (!valid_dimension(output_width) || !valid_dimension(output_height)) {
        return std::unexpected(make_state_error(
            VideoRendererBackendOperation::Render,
            "rendered video dimensions are invalid"));
    }

    if (impl_->context == nullptr || impl_->source_format != source_format ||
        impl_->source_width != source_width || impl_->source_height != source_height ||
        impl_->output_width != output_width || impl_->output_height != output_height) {
        SwsContext* context = sws_getCachedContext(
            impl_->context.release(),
            static_cast<int>(source_width),
            static_cast<int>(source_height),
            *source_native,
            static_cast<int>(output_width),
            static_cast<int>(output_height),
            AV_PIX_FMT_RGBA,
            SWS_BILINEAR,
            nullptr,
            nullptr,
            nullptr);
        impl_->context.reset(context);
        if (impl_->context == nullptr) {
            return std::unexpected(make_error(VideoRendererBackendOperation::Render,
                                               AVERROR(ENOMEM),
                                               "FFmpeg could not create the video scaler"));
        }
        impl_->source_format = source_format;
        impl_->source_width = source_width;
        impl_->source_height = source_height;
        impl_->output_width = output_width;
        impl_->output_height = output_height;
    }

    const std::size_t bytes_per_pixel = 4;
    const auto output_width_size = static_cast<std::size_t>(output_width);
    if (output_width_size > std::numeric_limits<std::size_t>::max() / bytes_per_pixel) {
        return std::unexpected(make_state_error(
            VideoRendererBackendOperation::Render,
            "rendered video stride overflows"));
    }
    const auto stride = output_width_size * bytes_per_pixel;
    const auto output_height_size = static_cast<std::size_t>(output_height);
    if (output_height_size > std::numeric_limits<std::size_t>::max() / stride ||
        stride > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        stride > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        return std::unexpected(make_state_error(
            VideoRendererBackendOperation::Render,
            "rendered video buffer size overflows"));
    }
    const auto output_size = stride * output_height_size;
    // libswscale may use aligned SIMD stores that touch a small tail beyond
    // the logical image. Keep that tail private to the conversion buffer and
    // expose only the tightly packed image through the media contract.
    constexpr std::size_t kSwscaleOutputPadding = 64;
    if (output_size > std::numeric_limits<std::size_t>::max() - kSwscaleOutputPadding) {
        return std::unexpected(make_state_error(
            VideoRendererBackendOperation::Render,
            "rendered video buffer size overflows"));
    }

    try {
        RenderedVideo output{
            .pixel_format = VideoPixelFormat::Rgba8,
            .width = output_width,
            .height = output_height,
            .stride_bytes = static_cast<std::uint32_t>(stride),
            .pixels = std::vector<std::byte>(output_size + kSwscaleOutputPadding),
            .pts_us = input.pts_us,
        };

        std::array<const std::uint8_t*, 4> source_data{};
        std::array<int, 4> source_linesize{};
        const auto plane_count = input.buffer->plane_count();
        for (std::size_t index = 0; index < plane_count; ++index) {
            const auto plane = input.buffer->plane(index);
            source_data[index] = reinterpret_cast<const std::uint8_t*>(plane.data);
            source_linesize[index] = static_cast<int>(plane.stride_bytes);
        }

        std::uint8_t* destination_data[4] = {
            reinterpret_cast<std::uint8_t*>(output.pixels.data()), nullptr, nullptr, nullptr};
        const int destination_linesize[4] = {static_cast<int>(stride), 0, 0, 0};
        const int scaled = sws_scale(impl_->context.get(),
                                     source_data.data(),
                                     source_linesize.data(),
                                     0,
                                     static_cast<int>(source_height),
                                     destination_data,
                                     destination_linesize);
        if (scaled != static_cast<int>(output_height)) {
            return std::unexpected(make_state_error(
                VideoRendererBackendOperation::Render,
                "FFmpeg video scaling returned an incomplete frame"));
        }
        output.pixels.resize(output_size);
        return output;
    } catch (const std::bad_alloc&) {
        return std::unexpected(make_error(VideoRendererBackendOperation::Render,
                                           AVERROR(ENOMEM),
                                           "video renderer allocation failed"));
    }
}

void FfmpegVideoRendererBackend::reset() noexcept {
    impl_->context.reset();
    impl_->source_format = VideoPixelFormat::Unknown;
    impl_->source_width = 0;
    impl_->source_height = 0;
    impl_->output_width = 0;
    impl_->output_height = 0;
}

void FfmpegVideoRendererBackend::unconfigure() noexcept {
    if (!impl_) {
        return;
    }
    reset();
    impl_->options = {};
    impl_->configured = false;
}

} // namespace semi::infra::ffmpeg::video_renderer
