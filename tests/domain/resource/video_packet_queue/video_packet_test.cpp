#include "domain/resource/video_packet_queue/video_packet.hpp"

#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

namespace semi::domain {
namespace {

TEST(VideoPacket, PreservesEncodedPacketAndGeneration) {
    VideoPacket packet({
                           .payload = {std::byte{7}},
                           .pts_us = 10,
                           .dts_us = 9,
                           .duration_us = 1'000,
                       },
                       3);

    ASSERT_EQ(packet.encoded().payload.size(), 1U);
    EXPECT_EQ(std::to_integer<std::uint8_t>(packet.encoded().payload.front()), 7U);
    EXPECT_EQ(packet.encoded().pts_us, 10);
    EXPECT_EQ(packet.generation(), 3U);
}

} // namespace
} // namespace semi::domain
