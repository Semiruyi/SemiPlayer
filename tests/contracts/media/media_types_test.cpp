#include "contracts/media/media_types.hpp"

#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

namespace semi::contracts::media {
namespace {

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

} // namespace
} // namespace semi::contracts::media
