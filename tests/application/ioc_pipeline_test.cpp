#include "ioc/ioc_container.hpp"

#include "application/api_layer.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

namespace semi::application {
namespace {

TEST(IoCPipelineTest, SeeksThenPlaysSampleThroughConfiguredAudioOutput) {
    auto& container = ioc::IoCContainer::instance();
    ASSERT_TRUE(container.dispose());
    ASSERT_TRUE(container.assemble());

    auto api_layer = container.api_layer();
    ASSERT_NE(api_layer, nullptr);

    const CommandHandle open = api_layer->open(SEMI_PLAYER_TEST_MEDIA_PATH);
    ASSERT_NE(open, 0U);

    CommandResult result;
    EXPECT_EQ(api_layer->await(open, result), SEMI_OK);
    EXPECT_TRUE(result.has_media_info);
    EXPECT_TRUE(result.media_info.has_audio);

    const auto seek = api_layer->seek(1'000'000);
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

} // namespace
} // namespace semi::application
