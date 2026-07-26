#include "domain/resource/audio_packet_queue/audio_packet_queue.hpp"
#include "infrastructure/notifier/default_notifier.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>

#include <gtest/gtest.h>

namespace semi::domain {
namespace {

class TestEncodedPacket final : public contracts::demuxer::packet::EncodedPacket {
public:
    explicit TestEncodedPacket(std::uint8_t marker, bool* destroyed = nullptr)
        : marker_(marker), destroyed_(destroyed) {
        payload_[0] = std::byte{marker};
    }

    ~TestEncodedPacket() override {
        if (destroyed_ != nullptr) {
            *destroyed_ = true;
        }
    }

    [[nodiscard]] std::span<const std::byte> payload() const noexcept override {
        return payload_;
    }

    [[nodiscard]] std::optional<std::int64_t> pts_us() const noexcept override {
        return marker_;
    }

    [[nodiscard]] std::optional<std::int64_t> dts_us() const noexcept override {
        return marker_;
    }

    [[nodiscard]] std::optional<std::int64_t> duration_us() const noexcept override {
        return 1'000;
    }

private:
    std::uint8_t marker_;
    bool* destroyed_;
    std::array<std::byte, 1> payload_{std::byte{0x01}};
};

AudioPacket make_packet(std::uint8_t marker, Generation::Value generation,
                        bool* destroyed = nullptr) {
    return AudioPacket(std::make_unique<TestEncodedPacket>(marker, destroyed), generation);
}

std::uint8_t packet_marker(const AudioPacket& packet) {
    return std::to_integer<std::uint8_t>(packet.encoded().payload().front());
}

TEST(AudioPacketQueue, PreservesFifoOrderAndGeneration) {
    AudioPacketQueue queue(std::make_shared<infra::DefaultNotifier>(), 2);

    EXPECT_EQ(queue.try_push(make_packet(1, 10)), AudioPacketPushResult::Accepted);
    EXPECT_EQ(queue.try_push(make_packet(2, 11)), AudioPacketPushResult::Accepted);
    EXPECT_TRUE(queue.full());

    auto first = queue.try_pop();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(packet_marker(*first), 1U);
    EXPECT_EQ(first->generation(), 10U);

    auto second = queue.try_pop();
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(packet_marker(*second), 2U);
    EXPECT_EQ(second->generation(), 11U);
    EXPECT_TRUE(queue.empty());
}

TEST(AudioPacketQueue, FullPushDoesNotConsumePacket) {
    AudioPacketQueue queue(std::make_shared<infra::DefaultNotifier>(), 1);
    ASSERT_EQ(queue.try_push(make_packet(1, 10)), AudioPacketPushResult::Accepted);

    auto rejected = make_packet(2, 11);
    EXPECT_EQ(queue.try_push(std::move(rejected)), AudioPacketPushResult::Full);
    EXPECT_EQ(packet_marker(rejected), 2U);
    EXPECT_EQ(rejected.generation(), 11U);
}

TEST(AudioPacketQueue, ClearReleasesQueuedPackets) {
    bool destroyed = false;
    AudioPacketQueue queue(std::make_shared<infra::DefaultNotifier>(), 1);
    ASSERT_EQ(queue.try_push(make_packet(1, 10, &destroyed)), AudioPacketPushResult::Accepted);
    ASSERT_FALSE(destroyed);

    queue.clear();

    EXPECT_TRUE(destroyed);
    EXPECT_TRUE(queue.empty());
}

TEST(AudioPacketQueue, ZeroCapacityQueueAlwaysReportsFull) {
    AudioPacketQueue queue(std::make_shared<infra::DefaultNotifier>(), 0);
    auto packet = make_packet(1, 10);

    EXPECT_EQ(queue.try_push(std::move(packet)), AudioPacketPushResult::Full);
    EXPECT_EQ(packet_marker(packet), 1U);
    EXPECT_TRUE(queue.full());
}

TEST(AudioPacketQueue, NotifiesOnlyOnEmptyAndFullBoundaryTransitions) {
    auto notifier = std::make_shared<infra::DefaultNotifier>();
    AudioPacketQueue queue(notifier, 2);
    int not_empty_calls = 0;
    int not_full_calls = 0;
    auto not_empty_subscription = notifier->subscribe<AudioQueueNotEmpty>(
        [&not_empty_calls, &queue](const AudioQueueNotEmpty&) {
            ++not_empty_calls;
            EXPECT_FALSE(queue.empty());
        });
    auto not_full_subscription = notifier->subscribe<AudioQueueNotFull>(
        [&not_full_calls, &queue](const AudioQueueNotFull&) {
            ++not_full_calls;
            EXPECT_FALSE(queue.full());
        });

    EXPECT_EQ(queue.try_push(make_packet(1, 10)), AudioPacketPushResult::Accepted);
    EXPECT_EQ(queue.try_push(make_packet(2, 11)), AudioPacketPushResult::Accepted);
    EXPECT_EQ(not_empty_calls, 1);
    EXPECT_EQ(not_full_calls, 0);

    ASSERT_TRUE(queue.try_pop().has_value());
    EXPECT_EQ(not_full_calls, 1);
    ASSERT_TRUE(queue.try_pop().has_value());
    EXPECT_EQ(not_full_calls, 1);

    EXPECT_EQ(queue.try_push(make_packet(3, 12)), AudioPacketPushResult::Accepted);
    EXPECT_EQ(not_empty_calls, 2);
    EXPECT_EQ(not_full_calls, 1);

    EXPECT_TRUE(not_empty_subscription->active());
    EXPECT_TRUE(not_full_subscription->active());
}

TEST(AudioPacketQueue, ClearFromFullNotifiesProducer) {
    auto notifier = std::make_shared<infra::DefaultNotifier>();
    AudioPacketQueue queue(notifier, 1);
    int not_full_calls = 0;
    auto subscription = notifier->subscribe<AudioQueueNotFull>(
        [&not_full_calls, &queue](const AudioQueueNotFull&) {
            ++not_full_calls;
            EXPECT_FALSE(queue.full());
        });

    ASSERT_EQ(queue.try_push(make_packet(1, 10)), AudioPacketPushResult::Accepted);
    queue.clear();

    EXPECT_EQ(not_full_calls, 1);
    EXPECT_TRUE(subscription->active());
}

} // namespace
} // namespace semi::domain
