#include "infrastructure/gpu/readback/detail/owned_cpu_video_frame_buffer.hpp"

#include <limits>
#include <stdexcept>

namespace semi::infra::gpu::detail {

OwnedCpuVideoFrameBuffer::OwnedCpuVideoFrameBuffer(
    contracts::media::VideoPixelFormat format, std::uint32_t width, std::uint32_t height)
    : format_(format), width_(width), height_(height) {
    using contracts::media::VideoPixelFormat;
    if ((format != VideoPixelFormat::Nv12 && format != VideoPixelFormat::P010) ||
        width == 0 || height == 0 || height > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("unsupported CPU image format or dimensions");
    }
    const std::size_t sample_bytes = format == VideoPixelFormat::P010 ? 2 : 1;
    if (width > static_cast<std::size_t>(std::numeric_limits<int>::max()) / sample_bytes) {
        throw std::invalid_argument("CPU image width exceeds supported range");
    }
    const auto y_stride = static_cast<std::size_t>(width) * sample_bytes;
    const auto uv_stride = ((static_cast<std::size_t>(width) + 1) / 2) * 2 * sample_bytes;
    const auto uv_rows = (static_cast<std::size_t>(height) + 1) / 2;
    if (uv_stride > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("CPU image stride exceeds supported range");
    }
    const auto maximum = std::numeric_limits<std::size_t>::max();
    if (height > maximum / y_stride || uv_rows > maximum / uv_stride) {
        throw std::invalid_argument("CPU image plane size overflows");
    }
    const auto y_size = y_stride * height;
    const auto uv_size = uv_stride * uv_rows;
    // Consumers such as swscale may read a small SIMD tail. It is not exposed
    // as logical image data and is zero initialized with the backing vector.
    constexpr std::size_t padding = 64;
    if (uv_size > maximum - padding || y_size > maximum - padding - uv_size) {
        throw std::invalid_argument("CPU image size overflows");
    }
    bytes_.resize(y_size + uv_size + padding);
    planes_[0] = {bytes_.data(), y_size, static_cast<std::uint32_t>(y_stride)};
    planes_[1] = {bytes_.data() + y_size, uv_size, static_cast<std::uint32_t>(uv_stride)};
}

contracts::media::VideoPixelFormat OwnedCpuVideoFrameBuffer::pixel_format() const noexcept {
    return format_;
}

std::uint32_t OwnedCpuVideoFrameBuffer::width() const noexcept {
    return width_;
}

std::uint32_t OwnedCpuVideoFrameBuffer::height() const noexcept {
    return height_;
}

const contracts::media::VideoFrameResource& OwnedCpuVideoFrameBuffer::resource() const noexcept {
    return resource_;
}

std::span<std::byte> OwnedCpuVideoFrameBuffer::writable_plane(std::size_t index) noexcept {
    if (index >= planes_.size()) return {};
    const auto offset = index == 0 ? 0 : planes_[0].size_bytes;
    return {bytes_.data() + offset, planes_[index].size_bytes};
}

} // namespace semi::infra::gpu::detail
