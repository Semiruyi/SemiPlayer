#pragma once

#include "contracts/media/media_types.hpp"

#include <array>

namespace semi::infra::gpu::detail {

// Readback-internal CPU owner. Writable planes are for construction/filling only;
// publish through unique_ptr<const VideoFrameBuffer> once complete.
class OwnedCpuVideoFrameBuffer final : public contracts::media::VideoFrameBuffer {
public:
    // Supports NV12/P010, including odd visible dimensions. Throws
    // invalid_argument for unsupported layout, bad_alloc on allocation failure.
    OwnedCpuVideoFrameBuffer(contracts::media::VideoPixelFormat format,
                             std::uint32_t width, std::uint32_t height);
    OwnedCpuVideoFrameBuffer(const OwnedCpuVideoFrameBuffer&) = delete;
    OwnedCpuVideoFrameBuffer& operator=(const OwnedCpuVideoFrameBuffer&) = delete;

    [[nodiscard]] contracts::media::VideoPixelFormat pixel_format() const noexcept override;
    [[nodiscard]] std::uint32_t width() const noexcept override;
    [[nodiscard]] std::uint32_t height() const noexcept override;
    [[nodiscard]] const contracts::media::VideoFrameResource& resource() const noexcept override;
    [[nodiscard]] std::span<std::byte> writable_plane(std::size_t index) noexcept;

private:
    contracts::media::VideoPixelFormat format_;
    std::uint32_t width_;
    std::uint32_t height_;
    std::vector<std::byte> bytes_;
    std::array<contracts::media::VideoPlaneView, 2> planes_{};
    contracts::media::CpuVideoFrameResource resource_{planes_};
};

} // namespace semi::infra::gpu::detail
