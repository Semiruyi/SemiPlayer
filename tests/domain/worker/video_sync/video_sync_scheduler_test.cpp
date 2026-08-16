#include "domain/worker/video_sync/video_sync_scheduler.hpp"

#include "domain/resource/video_rendered_store/video_rendered_store_item.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
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

class FakeAudioOutput final : public AudioOutput {
public:
    std::expected<AudioOutputConfigureResult, AudioOutputError>
    configure(const AudioOutputOptions&) override {
        return AudioOutputConfigureResult{
            .playback_format = contracts::media::AudioPcmFormat{
                .sample_rate = 48'000,
                .channels = 2,
                .sample_format = contracts::media::AudioSampleFormat::F32,
                .planar = false,
            },
        };
    }

    std::expected<void, AudioOutputError> start_playback() override { return {}; }
    std::expected<void, AudioOutputError> pause_playback() override { return {}; }
    std::optional<PlaybackPosition> current_position() const noexcept override {
        return position_;
    }
    void unconfigure() noexcept override {}

    void set_position(Generation::Value generation, std::int64_t pts_us) noexcept {
        position_ = PlaybackPosition{.generation = generation, .pts_us = pts_us};
    }

private:
    std::optional<PlaybackPosition> position_;
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

TEST(VideoFrameSchedulerTest, PresentsNewestDueFrameAndKeepsFutureFramePending) {
    auto source = std::make_shared<QueueSource>();
    source->push(make_frame(1, 50));
    source->push(make_frame(1, 80));
    source->push(make_frame(1, 150));

    auto audio_output = std::make_shared<FakeAudioOutput>();
    audio_output->set_position(1, 100);
    VideoSyncInput input(source);
    input.mark_available();
    VideoSyncClock clock(audio_output);
    clock.configure(true, 1);
    VideoFrameScheduler scheduler;

    const auto result = scheduler.step(input, clock, 1, true);

    ASSERT_TRUE(result.frame);
    EXPECT_EQ(result.frame->rendered().pts_us, 80);
    EXPECT_EQ(result.frames_popped, 3U);
    EXPECT_EQ(result.catchup_drops, 1U);
    EXPECT_TRUE(result.wait_scheduled);
    EXPECT_TRUE(scheduler.has_pending_frame());
}

TEST(VideoFrameSchedulerTest, WaitsForExternalClockBeforeReleasingThePendingFrame) {
    auto source = std::make_shared<QueueSource>();
    source->push(make_frame(2, 100));
    auto audio_output = std::make_shared<FakeAudioOutput>();

    VideoSyncInput input(source);
    input.mark_available();
    VideoSyncClock clock(audio_output);
    clock.configure(true, 2);
    VideoFrameScheduler scheduler;

    auto result = scheduler.step(input, clock, 2, true);
    EXPECT_TRUE(result.audio_clock_unavailable);
    EXPECT_FALSE(result.frame);
    EXPECT_TRUE(scheduler.has_pending_frame());
    EXPECT_TRUE(scheduler.waiting_for_audio_position());

    audio_output->set_position(2, 100);
    scheduler.on_audio_position_ready();
    result = scheduler.step(input, clock, 2, true);

    ASSERT_TRUE(result.frame);
    EXPECT_EQ(result.frame->rendered().pts_us, 100);
    EXPECT_FALSE(scheduler.has_pending_frame());
}

TEST(VideoFrameSchedulerTest, PresentsOneFrameImmediatelyForPausedGeneration) {
    auto source = std::make_shared<QueueSource>();
    source->push(make_frame(3, 1'000));

    VideoSyncInput input(source);
    input.mark_available();
    VideoSyncClock clock(nullptr);
    clock.configure(false, 3);
    VideoFrameScheduler scheduler;
    scheduler.on_generation_changed(false);

    const auto result = scheduler.step(input, clock, 3, false);

    ASSERT_TRUE(result.frame);
    EXPECT_EQ(result.frame->rendered().pts_us, 1'000);
    scheduler.on_frame_presented(false);
    EXPECT_FALSE(scheduler.paused_generation_pending());
}

} // namespace
} // namespace semi::domain
