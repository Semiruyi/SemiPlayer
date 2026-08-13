#include "ioc/ioc_container.hpp"

#include "application/api_layer.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

namespace semi::application {
namespace {

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
}

TEST(IoCPipelineTest, SeeksThenPlaysSampleThroughConfiguredAudioOutput) {
    auto& container = ioc::IoCContainer::instance();
    auto api_layer = assemble_pipeline();
    ASSERT_NE(api_layer, nullptr);

    CommandResult result;
    open_sample(api_layer, result);

    const auto seek =
        api_layer->seek(1'000'000, contracts::demuxer::SeekMode::NextKeyframe);
    ASSERT_NE(seek, 0U);
    EXPECT_EQ(api_layer->await(seek, result), SEMI_OK);

    const auto play = api_layer->play();
    ASSERT_NE(play, 0U);
    EXPECT_EQ(api_layer->await(play, result), SEMI_OK);

    bool playback_finished = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline && !playback_finished) {
        PlayerEvent event;
        ASSERT_EQ(api_layer->poll_event(event), SEMI_OK);
        playback_finished = event.type == PlayerEventType::PlaybackFinished;
        if (!playback_finished) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    EXPECT_TRUE(playback_finished);

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
