#include "domain/resource/audio_packet_queue/audio_packet.hpp"

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>

#include <gtest/gtest.h>

namespace semi::domain {
namespace {

class TestEncodedAudioPacket final : public contracts::audio::EncodedAudioPacket {
public:
    explicit TestEncodedAudioPacket(
        bool& destroyed,
        std::optional<std::int64_t> pts_us = 123'000,
        std::optional<std::int64_t> dts_us = 122'000)
        : destroyed_(destroyed), pts_us_(pts_us), dts_us_(dts_us) {}

    ~TestEncodedAudioPacket() override {
        destroyed_ = true;
    }

    [[nodiscard]] std::span<const std::byte> payload() const noexcept override {
        return payload_;
    }

    [[nodiscard]] std::optional<std::int64_t> pts_us() const noexcept override {
        return pts_us_;
    }

    [[nodiscard]] std::optional<std::int64_t> dts_us() const noexcept override {
        return dts_us_;
    }

    [[nodiscard]] std::optional<std::int64_t> duration_us() const noexcept override {
        return 21'333;
    }

private:
    bool& destroyed_;
    std::optional<std::int64_t> pts_us_;
    std::optional<std::int64_t> dts_us_;
    std::array<std::byte, 2> payload_{std::byte{0x01}, std::byte{0x02}};
};

static_assert(std::movable<AudioPacket>);
static_assert(!std::copyable<AudioPacket>);

TEST(AudioPacket, OwnsEncodedPacketAndCarriesGeneration) {
    bool destroyed = false;
    auto encoded = std::make_unique<TestEncodedAudioPacket>(destroyed);
    const auto* encoded_address = encoded.get();

    {
        AudioPacket packet(std::move(encoded), 7);

        EXPECT_EQ(packet.generation(), 7u);
        EXPECT_EQ(&packet.encoded(), encoded_address);
        ASSERT_TRUE(packet.encoded().pts_us().has_value());
        EXPECT_EQ(*packet.encoded().pts_us(), 123'000);
        ASSERT_TRUE(packet.encoded().dts_us().has_value());
        EXPECT_EQ(*packet.encoded().dts_us(), 122'000);
        EXPECT_EQ(packet.encoded().duration_us(), 21'333);
        EXPECT_EQ(packet.encoded().payload().size(), 2u);
        EXPECT_FALSE(destroyed);
    }

    EXPECT_TRUE(destroyed);
}

TEST(AudioPacket, TransfersEncodedPacketOwnershipWhenMoved) {
    bool destroyed = false;
    auto encoded = std::make_unique<TestEncodedAudioPacket>(destroyed);
    const auto* encoded_address = encoded.get();
    AudioPacket original(std::move(encoded), 9);

    AudioPacket moved(std::move(original));

    EXPECT_EQ(moved.generation(), 9u);
    EXPECT_EQ(&moved.encoded(), encoded_address);
    EXPECT_FALSE(destroyed);
}

TEST(AudioPacket, PreservesMissingPresentationTimestamp) {
    bool destroyed = false;
    auto encoded = std::make_unique<TestEncodedAudioPacket>(destroyed, std::nullopt);

    AudioPacket packet(std::move(encoded), 3);

    EXPECT_FALSE(packet.encoded().pts_us().has_value());
}

TEST(AudioPacket, PreservesMissingDecodeTimestamp) {
    bool destroyed = false;
    auto encoded = std::make_unique<TestEncodedAudioPacket>(destroyed, 123'000, std::nullopt);

    AudioPacket packet(std::move(encoded), 3);

    EXPECT_FALSE(packet.encoded().dts_us().has_value());
}

} // namespace
} // namespace semi::domain
