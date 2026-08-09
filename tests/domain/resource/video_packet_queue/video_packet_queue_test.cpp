#include "domain/resource/video_packet_queue/video_packet_queue.hpp"
#include "infrastructure/notifier/default_notifier.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include <gtest/gtest.h>

namespace semi::domain {
namespace {

VideoPacket make_packet(std::uint8_t marker, Generation::Value generation) {
    return VideoPacket({
                           .payload = {std::byte{marker}},
                           .pts_us = marker,
                           .dts_us = marker,
                           .duration_us = 1'000,
                       },
                       generation);
}

std::uint8_t packet_marker(const VideoPacket& packet) {
    return std::to_integer<std::uint8_t>(packet.encoded().payload.front());
}

const VideoPacket* packet_value(const VideoPacketQueueItem& item) noexcept {
    return std::get_if<VideoPacket>(&item);
}

TEST(VideoPacketQueue, PreservesFifoOrderAndGeneration) {
    VideoPacketQueue queue(std::make_shared<infra::DefaultNotifier>(), 2);

    EXPECT_EQ(queue.try_push(VideoPacketQueueItem{make_packet(1, 10)}),
              VideoPacketPushResult::Accepted);
    EXPECT_EQ(queue.try_push(VideoPacketQueueItem{make_packet(2, 11)}),
              VideoPacketPushResult::Accepted);
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

TEST(VideoPacketQueue, PreservesEndOfInputAfterAllPackets) {
    VideoPacketQueue queue(std::make_shared<infra::DefaultNotifier>(), 2);
    ASSERT_EQ(queue.try_push(VideoPacketQueueItem{make_packet(1, 10)}),
              VideoPacketPushResult::Accepted);
    ASSERT_EQ(queue.try_push(VideoPacketQueueItem{VideoPacketEndOfInput{.generation = 10}}),
              VideoPacketPushResult::Accepted);

    auto packet_item = queue.try_pop();
    ASSERT_TRUE(packet_item.has_value());
    ASSERT_NE(packet_value(*packet_item), nullptr);

    auto end_item = queue.try_pop();
    ASSERT_TRUE(end_item.has_value());
    const auto* end = std::get_if<VideoPacketEndOfInput>(&*end_item);
    ASSERT_NE(end, nullptr);
    EXPECT_EQ(end->generation, 10U);
}

TEST(VideoPacketQueueItem, ChecksPacketAndEndOfInputGeneration) {
    const VideoPacketQueueItem packet_item = make_packet(1, 7);
    const VideoPacketQueueItem end_item = VideoPacketEndOfInput{.generation = 7};
    const VideoPacketQueueItem stale_end_item = VideoPacketEndOfInput{.generation = 6};

    EXPECT_EQ(video_packet_queue_item_generation(packet_item), 7U);
    EXPECT_EQ(video_packet_queue_item_generation(end_item), 7U);
    EXPECT_TRUE(is_current_video_packet_queue_item(packet_item, 7));
    EXPECT_TRUE(is_current_video_packet_queue_item(end_item, 7));
    EXPECT_FALSE(is_current_video_packet_queue_item(stale_end_item, 7));
}

TEST(VideoPacketQueue, FullPushDoesNotConsumePacket) {
    VideoPacketQueue queue(std::make_shared<infra::DefaultNotifier>(), 1);
    ASSERT_EQ(queue.try_push(VideoPacketQueueItem{make_packet(1, 10)}),
              VideoPacketPushResult::Accepted);

    VideoPacketQueueItem rejected = make_packet(2, 11);
    EXPECT_EQ(queue.try_push(std::move(rejected)), VideoPacketPushResult::Full);
    const auto* rejected_packet = packet_value(rejected);
    ASSERT_NE(rejected_packet, nullptr);
    EXPECT_EQ(packet_marker(*rejected_packet), 2U);
    EXPECT_EQ(rejected_packet->generation(), 11U);
}

TEST(VideoPacketQueue, ClearDiscardsQueuedPackets) {
    VideoPacketQueue queue(std::make_shared<infra::DefaultNotifier>(), 1);
    ASSERT_EQ(queue.try_push(VideoPacketQueueItem{make_packet(1, 10)}),
              VideoPacketPushResult::Accepted);

    queue.clear();

    EXPECT_TRUE(queue.empty());
}

TEST(VideoPacketQueue, NotifiesConsumerAndProducerAtBoundaries) {
    auto notifier = std::make_shared<infra::DefaultNotifier>();
    VideoPacketQueue queue(notifier, 2);
    int not_empty_calls = 0;
    int not_full_calls = 0;
    auto not_empty_subscription = notifier->subscribe<VideoQueueNotEmpty>(
        [&not_empty_calls, &queue](const VideoQueueNotEmpty&) {
            ++not_empty_calls;
            EXPECT_FALSE(queue.empty());
        });
    auto not_full_subscription = notifier->subscribe<VideoQueueNotFull>(
        [&not_full_calls, &queue](const VideoQueueNotFull&) {
            ++not_full_calls;
            EXPECT_FALSE(queue.full());
        });

    EXPECT_EQ(queue.try_push(VideoPacketQueueItem{make_packet(1, 10)}),
              VideoPacketPushResult::Accepted);
    EXPECT_EQ(queue.try_push(VideoPacketQueueItem{make_packet(2, 11)}),
              VideoPacketPushResult::Accepted);
    EXPECT_EQ(not_empty_calls, 1);
    EXPECT_EQ(not_full_calls, 0);

    ASSERT_TRUE(queue.try_pop().has_value());
    EXPECT_EQ(not_full_calls, 1);
    ASSERT_TRUE(queue.try_pop().has_value());
    EXPECT_EQ(not_full_calls, 1);

    EXPECT_TRUE(not_empty_subscription->active());
    EXPECT_TRUE(not_full_subscription->active());
}

} // namespace
} // namespace semi::domain
