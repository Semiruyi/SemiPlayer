#include "contracts/media/media_types.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace semi::contracts::media {
namespace {

class FakeVideoFrameBuffer final : public VideoFrameBuffer {
public:
    FakeVideoFrameBuffer(VideoPixelFormat pixel_format,
                         std::uint32_t width,
                         std::uint32_t height,
                         std::vector<std::vector<std::byte>> planes,
                         std::vector<std::uint32_t> strides)
        : pixel_format_(pixel_format),
          width_(width),
          height_(height),
          planes_(std::move(planes)),
          strides_(std::move(strides)) {
        views_.reserve(planes_.size());
        for (std::size_t index = 0; index < planes_.size(); ++index) {
            views_.push_back(VideoPlaneView{
                .data = planes_[index].data(),
                .size_bytes = planes_[index].size(),
                .stride_bytes = strides_[index],
            });
        }
    }

    [[nodiscard]] VideoPixelFormat pixel_format() const noexcept override {
        return pixel_format_;
    }

    [[nodiscard]] std::uint32_t width() const noexcept override {
        return width_;
    }

    [[nodiscard]] std::uint32_t height() const noexcept override {
        return height_;
    }

    [[nodiscard]] std::size_t plane_count() const noexcept override {
        return views_.size();
    }

    [[nodiscard]] VideoPlaneView plane(std::size_t index) const noexcept override {
        if (index >= views_.size()) {
            return {};
        }
        return views_[index];
    }

private:
    VideoPixelFormat pixel_format_;
    std::uint32_t width_;
    std::uint32_t height_;
    std::vector<std::vector<std::byte>> planes_;
    std::vector<std::uint32_t> strides_;
    std::vector<VideoPlaneView> views_;
};

TEST(DecodedAudioTest, RepresentsPlanarPcmWithOptionalTimestamp) {
    DecodedAudio decoded{
        .format = AudioPcmFormat{
            .sample_rate = 48'000,
            .channels = 2,
            .sample_format = AudioSampleFormat::F32,
            .planar = true,
        },
        .samples_per_channel = 2,
        .planes = {
            {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04},
             std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08}},
            {std::byte{0x11}, std::byte{0x12}, std::byte{0x13}, std::byte{0x14},
             std::byte{0x15}, std::byte{0x16}, std::byte{0x17}, std::byte{0x18}},
        },
        .pts_us = 123'456,
    };

    EXPECT_EQ(decoded.format.sample_rate, 48'000U);
    EXPECT_EQ(decoded.format.channels, 2U);
    EXPECT_EQ(decoded.format.sample_format, AudioSampleFormat::F32);
    EXPECT_TRUE(decoded.format.planar);
    EXPECT_EQ(decoded.samples_per_channel, 2U);
    ASSERT_EQ(decoded.planes.size(), 2U);
    EXPECT_EQ(decoded.planes[0].size(), 8U);
    EXPECT_EQ(decoded.planes[1].size(), 8U);
    ASSERT_TRUE(decoded.pts_us.has_value());
    EXPECT_EQ(*decoded.pts_us, 123'456);
}

TEST(DecodedAudioTest, DefaultsToAnUnknownEmptyFormatWithoutTimestamp) {
    DecodedAudio decoded;

    EXPECT_EQ(decoded.format.sample_rate, 0U);
    EXPECT_EQ(decoded.format.channels, 0U);
    EXPECT_EQ(decoded.format.sample_format, AudioSampleFormat::Unknown);
    EXPECT_FALSE(decoded.format.planar);
    EXPECT_EQ(decoded.samples_per_channel, 0U);
    EXPECT_TRUE(decoded.planes.empty());
    EXPECT_FALSE(decoded.pts_us.has_value());
}

TEST(DecodedVideoTest, ExposesFormatPlanesAndTimestampThroughOwnedBuffer) {
    DecodedVideo decoded{
        .buffer = std::make_unique<FakeVideoFrameBuffer>(
            VideoPixelFormat::Rgba8,
            2,
            1,
            std::vector<std::vector<std::byte>>{{std::byte{0x01}, std::byte{0x02},
                                                   std::byte{0x03}, std::byte{0x04},
                                                   std::byte{0x05}, std::byte{0x06},
                                                   std::byte{0x07}, std::byte{0x08}}},
            std::vector<std::uint32_t>{8}),
        .pts_us = 123'456,
    };

    ASSERT_NE(decoded.buffer, nullptr);
    EXPECT_EQ(decoded.buffer->width(), 2U);
    EXPECT_EQ(decoded.buffer->height(), 1U);
    EXPECT_EQ(decoded.buffer->pixel_format(), VideoPixelFormat::Rgba8);
    ASSERT_EQ(decoded.buffer->plane_count(), 1U);
    const auto plane = decoded.buffer->plane(0);
    EXPECT_EQ(plane.bytes().size(), 8U);
    EXPECT_EQ(plane.stride_bytes, 8U);
    EXPECT_EQ(plane.bytes().front(), std::byte{0x01});
    ASSERT_TRUE(decoded.pts_us.has_value());
    EXPECT_EQ(*decoded.pts_us, 123'456);
}

TEST(DecodedVideoTest, BufferOwnerKeepsPlaneDataAlive) {
    auto buffer = std::make_unique<FakeVideoFrameBuffer>(
        VideoPixelFormat::Yuv420p,
        2,
        2,
        std::vector<std::vector<std::byte>>{{std::byte{0x11}, std::byte{0x12}}},
        std::vector<std::uint32_t>{2});
    DecodedVideo decoded{.buffer = std::move(buffer), .pts_us = std::nullopt};
    EXPECT_EQ(buffer, nullptr);

    ASSERT_NE(decoded.buffer, nullptr);
    const auto plane = decoded.buffer->plane(0);
    ASSERT_EQ(plane.bytes().size(), 2U);
    EXPECT_EQ(plane.bytes().front(), std::byte{0x11});
}

TEST(DecodedVideoTest, DefaultsToAnUnknownEmptyFormatWithoutTimestamp) {
    DecodedVideo decoded;

    EXPECT_EQ(decoded.buffer, nullptr);
    EXPECT_FALSE(decoded.pts_us.has_value());
}

} // namespace
} // namespace semi::contracts::media
