#include "domain/resource/video_rendered_store/video_rendered_store.hpp"
#include "infrastructure/notifier/default_notifier.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace semi::domain {
namespace {

RenderedVideoFrame make_frame(std::uint8_t marker, Generation::Value generation) {
    contracts::media::RenderedVideo rendered{
        .pixel_format = contracts::media::VideoPixelFormat::Rgba8,
        .width = 1,
        .height = 1,
        .stride_bytes = 4,
        .pixels = {std::byte{marker}, std::byte{0}, std::byte{0}, std::byte{255}},
        .pts_us = static_cast<std::int64_t>(marker),
    };
    return RenderedVideoFrame(std::move(rendered), generation);
}

std::uint8_t frame_marker(const RenderedVideoFrame& frame) {
    return std::to_integer<std::uint8_t>(frame.rendered().pixels.front());
}

TEST(VideoRenderedStore, PreservesFifoOrderAndMetadata) {
    VideoRenderedStore store(std::make_shared<infra::DefaultNotifier>(), 2);

    EXPECT_EQ(store.try_push(make_frame(1, 10)), VideoRenderedPushResult::Accepted);
    EXPECT_EQ(store.try_push(make_frame(2, 11)), VideoRenderedPushResult::Accepted);
    EXPECT_TRUE(store.full());

    auto first = store.try_pop();
    ASSERT_TRUE(first.has_value());
    const auto* first_frame = std::get_if<RenderedVideoFrame>(&*first);
    ASSERT_NE(first_frame, nullptr);
    EXPECT_EQ(frame_marker(*first_frame), 1U);
    EXPECT_EQ(first_frame->generation(), 10U);
    EXPECT_EQ(first_frame->rendered().pts_us, 1);

    auto second = store.try_pop();
    ASSERT_TRUE(second.has_value());
    const auto* second_frame = std::get_if<RenderedVideoFrame>(&*second);
    ASSERT_NE(second_frame, nullptr);
    EXPECT_EQ(frame_marker(*second_frame), 2U);
    EXPECT_EQ(second_frame->generation(), 11U);
}

TEST(VideoRenderedStore, PreservesEndOfInputAfterFrames) {
    VideoRenderedStore store(std::make_shared<infra::DefaultNotifier>(), 2);
    ASSERT_EQ(store.try_push(make_frame(1, 10)), VideoRenderedPushResult::Accepted);
    ASSERT_EQ(store.try_push(RenderedVideoEndOfInput{.generation = 10}),
              VideoRenderedPushResult::Accepted);

    ASSERT_TRUE(store.try_pop().has_value());
    auto end = store.try_pop();
    ASSERT_TRUE(end.has_value());
    const auto* marker = std::get_if<RenderedVideoEndOfInput>(&*end);
    ASSERT_NE(marker, nullptr);
    EXPECT_EQ(marker->generation, 10U);
}

TEST(RenderedVideoFrame, ChecksGeneration) {
    const auto frame = make_frame(1, 7);

    EXPECT_TRUE(is_current_rendered_video_frame(frame, 7));
    EXPECT_FALSE(is_current_rendered_video_frame(frame, 8));
}

TEST(VideoRenderedStore, FullPushDoesNotConsumeFrame) {
    VideoRenderedStore store(std::make_shared<infra::DefaultNotifier>(), 1);
    ASSERT_EQ(store.try_push(make_frame(1, 10)), VideoRenderedPushResult::Accepted);

    VideoRenderedStoreItem rejected = make_frame(2, 11);
    EXPECT_EQ(store.try_push(std::move(rejected)), VideoRenderedPushResult::Full);
    const auto* rejected_frame = std::get_if<RenderedVideoFrame>(&rejected);
    ASSERT_NE(rejected_frame, nullptr);
    EXPECT_EQ(frame_marker(*rejected_frame), 2U);
    EXPECT_EQ(rejected_frame->generation(), 11U);
}

TEST(VideoRenderedStoreItem, ChecksFrameAndEndOfInputGeneration) {
    const VideoRenderedStoreItem frame_item = make_frame(1, 7);
    const VideoRenderedStoreItem end_item = RenderedVideoEndOfInput{.generation = 7};
    const VideoRenderedStoreItem stale_end_item = RenderedVideoEndOfInput{.generation = 6};

    EXPECT_EQ(video_rendered_store_item_generation(frame_item), 7U);
    EXPECT_EQ(video_rendered_store_item_generation(end_item), 7U);
    EXPECT_TRUE(is_current_video_rendered_store_item(frame_item, 7));
    EXPECT_TRUE(is_current_video_rendered_store_item(end_item, 7));
    EXPECT_FALSE(is_current_video_rendered_store_item(stale_end_item, 7));
}

TEST(VideoRenderedStore, NotifiesConsumerAndProducerAtBoundaries) {
    auto notifier = std::make_shared<infra::DefaultNotifier>();
    VideoRenderedStore store(notifier, 2);
    int not_empty_calls = 0;
    int not_full_calls = 0;
    auto not_empty_subscription = notifier->subscribe<VideoRenderedStoreNotEmpty>(
        [&not_empty_calls, &store](const VideoRenderedStoreNotEmpty&) {
            ++not_empty_calls;
            EXPECT_FALSE(store.empty());
        });
    auto not_full_subscription = notifier->subscribe<VideoRenderedStoreNotFull>(
        [&not_full_calls, &store](const VideoRenderedStoreNotFull&) {
            ++not_full_calls;
            EXPECT_FALSE(store.full());
        });

    EXPECT_EQ(store.try_push(make_frame(1, 10)), VideoRenderedPushResult::Accepted);
    EXPECT_EQ(store.try_push(make_frame(2, 11)), VideoRenderedPushResult::Accepted);
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
