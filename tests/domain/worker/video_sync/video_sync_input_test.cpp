#include "domain/worker/video_sync/video_sync_input.hpp"

#include "domain/resource/video_rendered_store/video_rendered_store_item.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <utility>

namespace semi::domain {
namespace {

class QueueSource final : public VideoRenderedSource {
public:
    std::optional<VideoRenderedStoreItem> try_pop() override {
        if (items_.empty()) {
            return std::nullopt;
        }
        auto item = std::move(items_.front());
        items_.pop_front();
        return item;
    }

    void push(VideoRenderedStoreItem item) {
        items_.push_back(std::move(item));
    }

private:
    std::deque<VideoRenderedStoreItem> items_;
};

contracts::media::RenderedVideo make_rendered_video(std::int64_t pts_us) {
    return contracts::media::RenderedVideo{
        .pixel_format = contracts::media::VideoPixelFormat::Rgba8,
        .width = 1,
        .height = 1,
        .stride_bytes = 4,
        .pixels = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{255}},
        .pts_us = pts_us,
    };
}

VideoRenderedStoreItem make_frame(Generation::Value generation, std::int64_t pts_us) {
    return VideoRenderedStoreItem{
        std::in_place_type<RenderedVideoFrame>,
        make_rendered_video(pts_us),
        generation,
    };
}

TEST(VideoSyncInputTest, DropsStaleItemsBeforeReturningTheCurrentFrame) {
    auto source = std::make_shared<QueueSource>();
    source->push(make_frame(3, 10));
    source->push(make_frame(4, 20));

    VideoSyncInput input(source);
    input.mark_available();

    const auto result = input.try_pop_current(4);

    ASSERT_EQ(result.kind, VideoSyncInputResultKind::Frame);
    ASSERT_TRUE(result.frame);
    EXPECT_EQ(result.frame->generation(), 4U);
    EXPECT_EQ(result.frame->rendered().pts_us, 20);
    EXPECT_EQ(result.stale_items_dropped, 1U);
    EXPECT_TRUE(result.frame_popped);
}

TEST(VideoSyncInputTest, StopsAfterEndOfInputAndCanBeResetForTheNextGeneration) {
    auto source = std::make_shared<QueueSource>();
    source->push(RenderedVideoEndOfInput{.generation = 7});

    VideoSyncInput input(source);
    input.mark_available();

    const auto end_result = input.try_pop_current(7);
    ASSERT_EQ(end_result.kind, VideoSyncInputResultKind::EndOfInput);
    EXPECT_TRUE(input.end_of_input_observed());

    input.mark_available();
    EXPECT_EQ(input.try_pop_current(7).kind, VideoSyncInputResultKind::NotReady);

    input.reset();
    source->push(make_frame(8, 30));
    input.mark_available();
    const auto frame_result = input.try_pop_current(8);

    EXPECT_EQ(frame_result.kind, VideoSyncInputResultKind::Frame);
    ASSERT_TRUE(frame_result.frame);
    EXPECT_EQ(frame_result.frame->generation(), 8U);
}

} // namespace
} // namespace semi::domain
