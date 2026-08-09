#pragma once

#include "contracts/media/media_types.hpp"
#include "infrastructure/ffmpeg/ffmpeg_raii.hpp"

extern "C" {
#include <libavutil/pixfmt.h>
}

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace semi::infra::ffmpeg::video_decoder {

class FfmpegVideoFrameBuffer final : public contracts::media::VideoFrameBuffer {
public:
    explicit FfmpegVideoFrameBuffer(AvFramePtr frame) noexcept;
    ~FfmpegVideoFrameBuffer() override = default;

    FfmpegVideoFrameBuffer(const FfmpegVideoFrameBuffer&) = delete;
    FfmpegVideoFrameBuffer& operator=(const FfmpegVideoFrameBuffer&) = delete;
    FfmpegVideoFrameBuffer(FfmpegVideoFrameBuffer&&) = delete;
    FfmpegVideoFrameBuffer& operator=(FfmpegVideoFrameBuffer&&) = delete;

    [[nodiscard]] static std::optional<contracts::media::VideoPixelFormat>
    media_pixel_format(AVPixelFormat format) noexcept;

    [[nodiscard]] contracts::media::VideoPixelFormat pixel_format() const noexcept override;
    [[nodiscard]] std::uint32_t width() const noexcept override;
    [[nodiscard]] std::uint32_t height() const noexcept override;
    [[nodiscard]] std::size_t plane_count() const noexcept override;
    [[nodiscard]] contracts::media::VideoPlaneView plane(std::size_t index) const noexcept override;

private:
    AvFramePtr frame_;
    contracts::media::VideoPixelFormat pixel_format_ = contracts::media::VideoPixelFormat::Unknown;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::size_t plane_count_ = 0;
    std::array<contracts::media::VideoPlaneView, AV_NUM_DATA_POINTERS> planes_{};
};

} // namespace semi::infra::ffmpeg::video_decoder
