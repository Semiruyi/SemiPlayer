#include "domain/resource/video_frame_store/video_frame_store.hpp"
#include "infrastructure/notifier/default_notifier.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include <gtest/gtest.h>

namespace semi::domain {
namespace {

VideoFrame make_frame(std::uint8_t marker, Generation::Value generation) {
    contracts::media::DecodedVideo decoded;
    decoded.width = 1;
    decoded.height = 1;
    decoded.pixel_format = contracts::media::VideoPixelFormat::Rgba8;
    decoded.planes.push_back({
        .bytes = {std::byte{marker}, std::byte{0}, std::byte{0}, std::byte{255}},
        .stride_bytes = 4,
    });
    decoded.pts_us = marker;
    return VideoFrame(std::move(decoded), generation);
}

std::uint8_t frame_marker(const VideoFrame& frame) {
    return std::to_integer<std::uint8_t>(frame.decoded().planes.front().bytes.front());
}

TEST(VideoFrameStore, PreservesFifoOrderAndGeneration) {
    VideoFrameStore store(std::make_shared<infra::DefaultNotifier>(), 2);

    EXPECT_EQ(store.try_push(make_frame(1, 10)), VideoFramePushResult::Accepted);
    EXPECT_EQ(store.try_push(make_frame(2, 11)), VideoFramePushResult::Accepted);
    EXPECT_TRUE(store.full());

    auto first = store.try_pop();
    ASSERT_TRUE(first.has_value());
    const auto* first_frame = std::get_if<VideoFrame>(&*first);
    ASSERT_NE(first_frame, nullptr);
    EXPECT_EQ(frame_marker(*first_frame), 1U);
    EXPECT_EQ(first_frame->generation(), 10U);

    auto second = store.try_pop();
    ASSERT_TRUE(second.has_value());
    const auto* second_frame = std::get_if<VideoFrame>(&*second);
    ASSERT_NE(second_frame, nullptr);
    EXPECT_EQ(frame_marker(*second_frame), 2U);
    EXPECT_EQ(second_frame->generation(), 11U);
    EXPECT_TRUE(store.empty());
}

TEST(VideoFrameStore, PreservesEndOfInputAfterFrames) {
    VideoFrameStore store(std::make_shared<infra::DefaultNotifier>(), 2);
    ASSERT_EQ(store.try_push(make_frame(1, 10)), VideoFramePushResult::Accepted);
    ASSERT_EQ(store.try_push(VideoFrameEndOfInput{.generation = 10}),
              VideoFramePushResult::Accepted);

    auto frame = store.try_pop();
    ASSERT_TRUE(frame.has_value());
    EXPECT_NE(std::get_if<VideoFrame>(&*frame), nullptr);

    auto end_of_input = store.try_pop();
    ASSERT_TRUE(end_of_input.has_value());
    const auto* marker = std::get_if<VideoFrameEndOfInput>(&*end_of_input);
    ASSERT_NE(marker, nullptr);
    EXPECT_EQ(marker->generation, 10U);
}

TEST(VideoFrame, ChecksGeneration) {
    const VideoFrame frame = make_frame(1, 7);

    EXPECT_TRUE(is_current_video_frame(frame, 7));
    EXPECT_FALSE(is_current_video_frame(frame, 8));
}

TEST(VideoFrameStore, FullPushDoesNotConsumeFrame) {
    VideoFrameStore store(std::make_shared<infra::DefaultNotifier>(), 1);
    ASSERT_EQ(store.try_push(make_frame(1, 10)), VideoFramePushResult::Accepted);

    VideoFrameStoreItem rejected = make_frame(2, 11);
    EXPECT_EQ(store.try_push(std::move(rejected)), VideoFramePushResult::Full);
    const auto* rejected_frame = std::get_if<VideoFrame>(&rejected);
    ASSERT_NE(rejected_frame, nullptr);
    EXPECT_EQ(frame_marker(*rejected_frame), 2U);
    EXPECT_EQ(rejected_frame->generation(), 11U);
}

TEST(VideoFrameStoreItem, ChecksFrameAndEndOfInputGeneration) {
    const VideoFrameStoreItem frame_item = make_frame(1, 7);
    const VideoFrameStoreItem end_item = VideoFrameEndOfInput{.generation = 7};
    const VideoFrameStoreItem stale_end_item = VideoFrameEndOfInput{.generation = 6};

    EXPECT_EQ(video_frame_store_item_generation(frame_item), 7U);
    EXPECT_EQ(video_frame_store_item_generation(end_item), 7U);
    EXPECT_TRUE(is_current_video_frame_store_item(frame_item, 7));
    EXPECT_TRUE(is_current_video_frame_store_item(end_item, 7));
    EXPECT_FALSE(is_current_video_frame_store_item(stale_end_item, 7));
}

TEST(VideoFrameStore, NotifiesConsumerAndProducerAtBoundaries) {
    auto notifier = std::make_shared<infra::DefaultNotifier>();
    VideoFrameStore store(notifier, 2);
    int not_empty_calls = 0;
    int not_full_calls = 0;
    auto not_empty_subscription = notifier->subscribe<VideoFrameStoreNotEmpty>(
        [&not_empty_calls, &store](const VideoFrameStoreNotEmpty&) {
            ++not_empty_calls;
            EXPECT_FALSE(store.empty());
        });
    auto not_full_subscription = notifier->subscribe<VideoFrameStoreNotFull>(
        [&not_full_calls, &store](const VideoFrameStoreNotFull&) {
            ++not_full_calls;
            EXPECT_FALSE(store.full());
        });

    EXPECT_EQ(store.try_push(make_frame(1, 10)), VideoFramePushResult::Accepted);
    EXPECT_EQ(store.try_push(make_frame(2, 11)), VideoFramePushResult::Accepted);
    EXPECT_EQ(not_empty_calls, 1);
    EXPECT_EQ(not_full_calls, 0);

    ASSERT_TRUE(store.try_pop().has_value());
    EXPECT_EQ(not_full_calls, 1);
    ASSERT_TRUE(store.try_pop().has_value());
    EXPECT_EQ(not_full_calls, 1);

    EXPECT_TRUE(not_empty_subscription->active());
    EXPECT_TRUE(not_full_subscription->active());
}

} // namespace
} // namespace semi::domain
