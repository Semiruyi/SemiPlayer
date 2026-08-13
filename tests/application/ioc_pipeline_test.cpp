#include "ioc/ioc_container.hpp"

#include "application/api_layer.hpp"
#include "domain/resource/video_rendered_store/rendered_video_frame.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace semi::application {
namespace {

struct FrameSnapshot {
    std::size_t count = 0;
    domain::Generation::Value generation = 0;
    std::optional<std::int64_t> pts_us;
};

class FrameObservation final {
public:
    void record(const domain::RenderedVideoFrame& frame) {
        {
            std::lock_guard lock(mutex_);
            ++snapshot_.count;
            snapshot_.generation = frame.generation();
            snapshot_.pts_us = frame.rendered().pts_us;
        }
        cv_.notify_all();
    }

    [[nodiscard]] bool wait_for_count(std::size_t expected,
                                      std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] {
            return snapshot_.count >= expected;
        });
    }

    [[nodiscard]] FrameSnapshot snapshot() const {
        std::lock_guard lock(mutex_);
        return snapshot_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    FrameSnapshot snapshot_;
};

std::shared_ptr<ApiLayer> assemble_pipeline() {
    auto& container = ioc::IoCContainer::instance();
    EXPECT_TRUE(container.dispose());
    EXPECT_TRUE(container.assemble());

    auto api_layer = container.api_layer();
    EXPECT_NE(api_layer, nullptr);
    return api_layer;
}

void open_sample(const std::shared_ptr<ApiLayer>& api_layer, CommandResult& result) {
    const CommandHandle open = api_layer->open(SEMI_PLAYER_TEST_MEDIA_PATH);
    ASSERT_NE(open, 0U);
    ASSERT_EQ(api_layer->await(open, result), SEMI_OK);
    ASSERT_TRUE(result.has_media_info);
    ASSERT_TRUE(result.media_info.has_audio);
    ASSERT_TRUE(result.media_info.has_video);
}

void configure_frame_observation(const std::shared_ptr<ApiLayer>& api_layer,
                                 FrameObservation& frames,
                                 CommandResult& result) {
    VideoPresentationConfig config;
    config.on_frame = [&frames](const domain::RenderedVideoFrame& frame) {
        frames.record(frame);
    };
    const CommandHandle configure = api_layer->configure_video_output(std::move(config));
    ASSERT_NE(configure, 0U);
    ASSERT_EQ(api_layer->await(configure, result), SEMI_OK);
}

bool wait_for_playback_finished(const std::shared_ptr<ApiLayer>& api_layer,
                                std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        PlayerEvent event;
        if (api_layer->poll_event(event) != SEMI_OK) {
            return false;
        }
        if (event.type == PlayerEventType::PlaybackFinished) {
            return true;
        }
        if (event.type != PlayerEventType::None) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

bool observes_no_event(const std::shared_ptr<ApiLayer>& api_layer,
                       std::chrono::milliseconds duration) {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline) {
        PlayerEvent event;
        if (api_layer->poll_event(event) != SEMI_OK || event.type != PlayerEventType::None) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

TEST(IoCPipelineTest, PresentsFinalFrameBeforePublishingSinglePlaybackFinished) {
    auto& container = ioc::IoCContainer::instance();
    auto api_layer = assemble_pipeline();
    ASSERT_NE(api_layer, nullptr);

    CommandResult result;
    FrameObservation frames;
    configure_frame_observation(api_layer, frames, result);
    open_sample(api_layer, result);
    const auto duration_us = result.media_info.duration_us;

    const auto seek =
        api_layer->seek(1'000'000, contracts::demuxer::SeekMode::NextKeyframe);
    ASSERT_NE(seek, 0U);
    EXPECT_EQ(api_layer->await(seek, result), SEMI_OK);

    const auto play = api_layer->play();
    ASSERT_NE(play, 0U);
    EXPECT_EQ(api_layer->await(play, result), SEMI_OK);

    ASSERT_TRUE(wait_for_playback_finished(api_layer, std::chrono::seconds(5)));

    const auto final_frame = frames.snapshot();
    ASSERT_GT(final_frame.count, 0U);
    ASSERT_TRUE(final_frame.pts_us.has_value());
    EXPECT_GE(*final_frame.pts_us, duration_us - 100'000);
    EXPECT_TRUE(observes_no_event(api_layer, std::chrono::milliseconds(200)));

    const CommandHandle close = api_layer->close();
    ASSERT_NE(close, 0U);
    EXPECT_EQ(api_layer->await(close, result), SEMI_OK);

    EXPECT_TRUE(container.dispose());
}

TEST(IoCPipelineTest, SeekNearEndFinishesWithTheNewGeneration) {
    auto& container = ioc::IoCContainer::instance();
    auto api_layer = assemble_pipeline();
    ASSERT_NE(api_layer, nullptr);

    CommandResult result;
    FrameObservation frames;
    configure_frame_observation(api_layer, frames, result);
    open_sample(api_layer, result);
    const auto duration_us = result.media_info.duration_us;

    const CommandHandle play = api_layer->play();
    ASSERT_NE(play, 0U);
    ASSERT_EQ(api_layer->await(play, result), SEMI_OK);
    ASSERT_TRUE(frames.wait_for_count(1, std::chrono::seconds(3)));
    const auto generation_before_seek = frames.snapshot().generation;

    const CommandHandle seek = api_layer->seek(
        duration_us - 250'000,
        contracts::demuxer::SeekMode::PreviousKeyframe);
    ASSERT_NE(seek, 0U);
    ASSERT_EQ(api_layer->await(seek, result), SEMI_OK);
    ASSERT_TRUE(wait_for_playback_finished(api_layer, std::chrono::seconds(5)));

    const auto final_frame = frames.snapshot();
    EXPECT_GT(final_frame.generation, generation_before_seek);
    ASSERT_TRUE(final_frame.pts_us.has_value());
    EXPECT_GE(*final_frame.pts_us, duration_us - 100'000);

    const CommandHandle close = api_layer->close();
    ASSERT_NE(close, 0U);
    EXPECT_EQ(api_layer->await(close, result), SEMI_OK);
    EXPECT_TRUE(container.dispose());
}

TEST(IoCPipelineTest, SerializesRepeatedSeeksWithPauseAndResumeWhilePlaying) {
    auto& container = ioc::IoCContainer::instance();
    auto api_layer = assemble_pipeline();
    ASSERT_NE(api_layer, nullptr);

    CommandResult result;
    open_sample(api_layer, result);

    const CommandHandle play = api_layer->play();
    const CommandHandle first_seek =
        api_layer->seek(500'000, contracts::demuxer::SeekMode::NextKeyframe);
    const CommandHandle second_seek =
        api_layer->seek(1'250'000, contracts::demuxer::SeekMode::NextKeyframe);
    const CommandHandle pause = api_layer->pause();
    const CommandHandle final_seek =
        api_layer->seek(2'000'000, contracts::demuxer::SeekMode::PreviousKeyframe);
    const CommandHandle resume = api_layer->play();
    ASSERT_NE(play, 0U);
    ASSERT_NE(first_seek, 0U);
    ASSERT_NE(second_seek, 0U);
    ASSERT_NE(pause, 0U);
    ASSERT_NE(final_seek, 0U);
    ASSERT_NE(resume, 0U);

    EXPECT_EQ(api_layer->await(play, result), SEMI_OK);
    EXPECT_EQ(api_layer->await(first_seek, result), SEMI_OK);
    EXPECT_EQ(api_layer->await(second_seek, result), SEMI_OK);
    EXPECT_EQ(api_layer->await(pause, result), SEMI_OK);
    EXPECT_EQ(api_layer->await(final_seek, result), SEMI_OK);
    EXPECT_EQ(api_layer->await(resume, result), SEMI_OK);

    const CommandHandle close = api_layer->close();
    ASSERT_NE(close, 0U);
    EXPECT_EQ(api_layer->await(close, result), SEMI_OK);
    EXPECT_TRUE(container.dispose());
}

TEST(IoCPipelineTest, ClosesPlayingSessionBeforeOpeningAnotherMedia) {
    auto& container = ioc::IoCContainer::instance();
    auto api_layer = assemble_pipeline();
    ASSERT_NE(api_layer, nullptr);

    CommandResult result;
    open_sample(api_layer, result);

    const CommandHandle play = api_layer->play();
    ASSERT_NE(play, 0U);
    ASSERT_EQ(api_layer->await(play, result), SEMI_OK);

    const CommandHandle close = api_layer->close();
    ASSERT_NE(close, 0U);
    ASSERT_EQ(api_layer->await(close, result), SEMI_OK);

    open_sample(api_layer, result);
    const CommandHandle second_close = api_layer->close();
    ASSERT_NE(second_close, 0U);
    EXPECT_EQ(api_layer->await(second_close, result), SEMI_OK);
    EXPECT_TRUE(container.dispose());
}

} // namespace
} // namespace semi::application
