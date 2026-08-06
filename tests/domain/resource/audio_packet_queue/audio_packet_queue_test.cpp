#include "domain/resource/audio_packet_queue/audio_packet_queue.hpp"
#include "infrastructure/notifier/default_notifier.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

#include <gtest/gtest.h>

namespace semi::domain {
namespace {

AudioPacket make_packet(std::uint8_t marker, Generation::Value generation) {
    return AudioPacket({
                           .payload = {std::byte{marker}},
                           .pts_us = marker,
                           .dts_us = marker,
                           .duration_us = 1'000,
                       },
                       generation);
}

std::uint8_t packet_marker(const AudioPacket& packet) {
    return std::to_integer<std::uint8_t>(packet.encoded().payload.front());
}

const AudioPacket* packet_value(const AudioPacketQueueItem& item) noexcept {
    return std::get_if<AudioPacket>(&item);
}

TEST(AudioPacketQueue, PreservesFifoOrderAndGeneration) {
    AudioPacketQueue queue(std::make_shared<infra::DefaultNotifier>(), 2);

    EXPECT_EQ(queue.try_push(AudioPacketQueueItem{make_packet(1, 10)}), AudioPacketPushResult::Accepted);
    EXPECT_EQ(queue.try_push(AudioPacketQueueItem{make_packet(2, 11)}), AudioPacketPushResult::Accepted);
    EXPECT_TRUE(queue.full());

    auto first = queue.try_pop();
    ASSERT_TRUE(first.has_value());
    const auto* first_packet = packet_value(*first);
    ASSERT_NE(first_packet, nullptr);
    EXPECT_EQ(packet_marker(*first_packet), 1U);
    EXPECT_EQ(first_packet->generation(), 10U);

    auto second = queue.try_pop();
    ASSERT_TRUE(second.has_value());
    const auto* second_packet = packet_value(*second);
    ASSERT_NE(second_packet, nullptr);
    EXPECT_EQ(packet_marker(*second_packet), 2U);
    EXPECT_EQ(second_packet->generation(), 11U);
    EXPECT_TRUE(queue.empty());
}

TEST(AudioPacketQueue, PreservesEndOfInputAfterAllPackets) {
    AudioPacketQueue queue(std::make_shared<infra::DefaultNotifier>(), 2);

    ASSERT_EQ(queue.try_push(AudioPacketQueueItem{make_packet(1, 10)}), AudioPacketPushResult::Accepted);
    ASSERT_EQ(queue.try_push(AudioPacketQueueItem{AudioPacketEndOfInput{.generation = 10}}),
              AudioPacketPushResult::Accepted);

    auto packet_item = queue.try_pop();
    ASSERT_TRUE(packet_item.has_value());
    const auto* packet = packet_value(*packet_item);
    ASSERT_NE(packet, nullptr);
    EXPECT_EQ(packet_marker(*packet), 1U);

    auto end_item = queue.try_pop();
    ASSERT_TRUE(end_item.has_value());
    const auto* end = std::get_if<AudioPacketEndOfInput>(&*end_item);
    ASSERT_NE(end, nullptr);
    EXPECT_EQ(end->generation, 10U);
}

TEST(AudioPacketQueueItem, ChecksPacketAndEndOfInputGeneration) {
    const AudioPacketQueueItem packet_item = make_packet(1, 7);
    const AudioPacketQueueItem end_item = AudioPacketEndOfInput{.generation = 7};
    const AudioPacketQueueItem stale_end_item = AudioPacketEndOfInput{.generation = 6};

    EXPECT_EQ(audio_packet_queue_item_generation(packet_item), 7U);
    EXPECT_EQ(audio_packet_queue_item_generation(end_item), 7U);
    EXPECT_TRUE(is_current_audio_packet_queue_item(packet_item, 7));
    EXPECT_TRUE(is_current_audio_packet_queue_item(end_item, 7));
    EXPECT_FALSE(is_current_audio_packet_queue_item(stale_end_item, 7));
}

TEST(AudioPacketQueue, EndOfInputRespectsBackpressure) {
    AudioPacketQueue queue(std::make_shared<infra::DefaultNotifier>(), 1);
    ASSERT_EQ(queue.try_push(AudioPacketQueueItem{make_packet(1, 10)}), AudioPacketPushResult::Accepted);

    AudioPacketQueueItem end_item = AudioPacketEndOfInput{.generation = 10};
    EXPECT_EQ(queue.try_push(std::move(end_item)), AudioPacketPushResult::Full);
    ASSERT_TRUE(std::holds_alternative<AudioPacketEndOfInput>(end_item));

    auto packet_item = queue.try_pop();
    ASSERT_TRUE(packet_item.has_value());
    ASSERT_NE(packet_value(*packet_item), nullptr);

    EXPECT_EQ(queue.try_push(std::move(end_item)), AudioPacketPushResult::Accepted);
    auto queued_end = queue.try_pop();
    ASSERT_TRUE(queued_end.has_value());
    ASSERT_NE(std::get_if<AudioPacketEndOfInput>(&*queued_end), nullptr);
}

TEST(AudioPacketQueue, FullPushDoesNotConsumePacket) {
    AudioPacketQueue queue(std::make_shared<infra::DefaultNotifier>(), 1);
    ASSERT_EQ(queue.try_push(AudioPacketQueueItem{make_packet(1, 10)}),
              AudioPacketPushResult::Accepted);

    AudioPacketQueueItem rejected = make_packet(2, 11);
    EXPECT_EQ(queue.try_push(std::move(rejected)), AudioPacketPushResult::Full);
    const auto* rejected_packet = packet_value(rejected);
    ASSERT_NE(rejected_packet, nullptr);
    EXPECT_EQ(packet_marker(*rejected_packet), 2U);
    EXPECT_EQ(rejected_packet->generation(), 11U);
}

TEST(AudioPacketQueue, ClearDiscardsQueuedPackets) {
    AudioPacketQueue queue(std::make_shared<infra::DefaultNotifier>(), 1);
    ASSERT_EQ(queue.try_push(AudioPacketQueueItem{make_packet(1, 10)}),
              AudioPacketPushResult::Accepted);

    queue.clear();

    EXPECT_TRUE(queue.empty());
}

TEST(AudioPacketQueue, ZeroCapacityQueueAlwaysReportsFull) {
    AudioPacketQueue queue(std::make_shared<infra::DefaultNotifier>(), 0);
    AudioPacketQueueItem packet = make_packet(1, 10);

    EXPECT_EQ(queue.try_push(std::move(packet)), AudioPacketPushResult::Full);
    const auto* packet_value_after_push = packet_value(packet);
    ASSERT_NE(packet_value_after_push, nullptr);
    EXPECT_EQ(packet_marker(*packet_value_after_push), 1U);
    EXPECT_TRUE(queue.full());
}

TEST(AudioPacketQueue, NotifiesConsumerOnEmptyBoundaryAndProducerOnFullBoundary) {
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

    EXPECT_EQ(queue.try_push(AudioPacketQueueItem{make_packet(1, 10)}), AudioPacketPushResult::Accepted);
    EXPECT_EQ(queue.try_push(AudioPacketQueueItem{make_packet(2, 11)}), AudioPacketPushResult::Accepted);
    EXPECT_EQ(not_empty_calls, 1);
    EXPECT_EQ(not_full_calls, 0);

    ASSERT_TRUE(queue.try_pop().has_value());
    EXPECT_EQ(not_full_calls, 1);
    ASSERT_TRUE(queue.try_pop().has_value());
    EXPECT_EQ(not_full_calls, 1);

    EXPECT_EQ(queue.try_push(AudioPacketQueueItem{make_packet(3, 12)}), AudioPacketPushResult::Accepted);
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

    ASSERT_EQ(queue.try_push(AudioPacketQueueItem{make_packet(1, 10)}), AudioPacketPushResult::Accepted);
    queue.clear();

    EXPECT_EQ(not_full_calls, 1);
    EXPECT_TRUE(subscription->active());
}

} // namespace
} // namespace semi::domain
