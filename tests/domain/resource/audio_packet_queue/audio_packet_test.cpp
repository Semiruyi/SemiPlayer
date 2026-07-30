#include "domain/resource/audio_packet_queue/audio_packet.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace semi::domain {
namespace {

contracts::demuxer::packet::EncodedPacket make_encoded_packet(
    std::optional<std::int64_t> pts_us = 123'000,
    std::optional<std::int64_t> dts_us = 122'000) {
    return {
        .payload = {std::byte{0x01}, std::byte{0x02}},
        .pts_us = pts_us,
        .dts_us = dts_us,
        .duration_us = 21'333,
    };
}

static_assert(std::movable<AudioPacket>);
static_assert(!std::copyable<AudioPacket>);

TEST(AudioPacket, OwnsEncodedDataAndCarriesGeneration) {
    AudioPacket packet(make_encoded_packet(), 7);

    EXPECT_EQ(packet.generation(), 7u);
    ASSERT_TRUE(packet.encoded().pts_us.has_value());
    EXPECT_EQ(*packet.encoded().pts_us, 123'000);
    ASSERT_TRUE(packet.encoded().dts_us.has_value());
    EXPECT_EQ(*packet.encoded().dts_us, 122'000);
    EXPECT_EQ(packet.encoded().duration_us, 21'333);
    EXPECT_EQ(packet.encoded().payload.size(), 2u);
}

TEST(AudioPacket, TransfersEncodedDataWhenMoved) {
    AudioPacket original(make_encoded_packet(), 9);

    AudioPacket moved(std::move(original));

    EXPECT_EQ(moved.generation(), 9u);
    EXPECT_EQ(moved.encoded().payload.size(), 2u);
}

TEST(AudioPacket, PreservesMissingPresentationTimestamp) {
    AudioPacket packet(make_encoded_packet(std::nullopt), 3);

    EXPECT_FALSE(packet.encoded().pts_us.has_value());
}

TEST(AudioPacket, PreservesMissingDecodeTimestamp) {
    AudioPacket packet(make_encoded_packet(123'000, std::nullopt), 3);

    EXPECT_FALSE(packet.encoded().dts_us.has_value());
}

} // namespace
} // namespace semi::domain
