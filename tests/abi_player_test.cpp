#include "semi_player/semi_player.h"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace {

semi_command_result_t await_ok(semi_handle_t handle) {
    EXPECT_NE(handle, 0U);
    semi_command_result_t result{};
    EXPECT_EQ(semi_player_handle_await(handle, &result), SEMI_OK);
    return result;
}

struct FrameCapture {
    std::mutex mutex;
    std::condition_variable cv;
    bool received = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t generation = 0;
    std::uint32_t struct_size = 0;
    semi_video_pixel_format_t pixel_format = 0;
    std::uint32_t has_pts = 0;
    std::uint32_t plane_count = 0;
    std::uint64_t plane_size = 0;
    std::uint32_t stride = 0;
};

void capture_frame(void* user_data, const semi_video_frame_t* frame) {
    auto& capture = *static_cast<FrameCapture*>(user_data);
    std::lock_guard lock(capture.mutex);
    capture.received = true;
    capture.width = frame->width;
    capture.height = frame->height;
    capture.generation = frame->generation;
    capture.struct_size = frame->struct_size;
    capture.pixel_format = frame->pixel_format;
    capture.has_pts = frame->has_pts;
    capture.plane_count = frame->plane_count;
    capture.plane_size = frame->planes[0].size_bytes;
    capture.stride = frame->planes[0].stride_bytes;
    capture.cv.notify_all();
}

TEST(SemiPlayerAbiTest, RunsSeekPauseResumeAndLifecycleThroughSharedLibrary) {
    ASSERT_EQ(semi_player_shutdown(), SEMI_OK);
    ASSERT_EQ(semi_player_init(), SEMI_OK);
    EXPECT_EQ(semi_player_init(), SEMI_OK);

    EXPECT_EQ(semi_player_configure_video_output(nullptr), 0U);
    semi_video_output_config_t too_small{};
    too_small.struct_size = sizeof(too_small.struct_size);
    semi_command_result_t invalid_config_result{};
    const semi_handle_t invalid_config =
        semi_player_configure_video_output(&too_small);
    ASSERT_NE(invalid_config, 0U);
    EXPECT_EQ(semi_player_handle_await(invalid_config, &invalid_config_result),
              SEMI_ERR_INVALID_ARGUMENT);

    FrameCapture capture;
    semi_video_output_config_t video_config{};
    video_config.struct_size = sizeof(video_config);
    video_config.pixel_format = SEMI_VIDEO_PIXEL_FORMAT_RGBA8888;
    video_config.on_frame = capture_frame;
    video_config.user_data = &capture;
    await_ok(semi_player_configure_video_output(&video_config));

    semi_player_event_t event{.type = SEMI_PLAYER_EVENT_PLAYBACK_FINISHED};
    ASSERT_EQ(semi_player_poll_event(&event), SEMI_OK);
    EXPECT_EQ(event.type, SEMI_PLAYER_EVENT_NONE);

    const semi_handle_t open = semi_player_open(SEMI_PLAYER_TEST_MEDIA_PATH);
    const semi_command_result_t open_result = await_ok(open);
    EXPECT_NE(open_result.has_media_info, 0U);
    EXPECT_NE(open_result.media_info.has_audio, 0U);
    EXPECT_NE(open_result.media_info.has_video, 0U);
    EXPECT_GT(open_result.media_info.duration_us, 0);

    semi_command_result_t consumed_result{};
    EXPECT_EQ(semi_player_handle_await(open, &consumed_result), SEMI_ERR_INVALID_HANDLE);

    const semi_handle_t play = semi_player_play();
    await_ok(play);
    {
        std::unique_lock lock(capture.mutex);
        ASSERT_TRUE(capture.cv.wait_for(lock, std::chrono::seconds(3), [&capture] {
            return capture.received;
        }));
        EXPECT_GT(capture.width, 0U);
        EXPECT_GT(capture.height, 0U);
        EXPECT_GT(capture.generation, 0U);
        EXPECT_EQ(capture.struct_size, sizeof(semi_video_frame_t));
        EXPECT_EQ(capture.pixel_format, SEMI_VIDEO_PIXEL_FORMAT_RGBA8888);
        EXPECT_NE(capture.has_pts, 0U);
        EXPECT_EQ(capture.plane_count, 1U);
        EXPECT_GE(capture.stride, capture.width * 4U);
        EXPECT_GE(capture.plane_size,
                  static_cast<std::uint64_t>(capture.stride) * capture.height);
    }

    const semi_handle_t first_seek = semi_player_seek(500'000);
    const semi_handle_t second_seek = semi_player_seek(1'250'000);
    const semi_handle_t pause = semi_player_pause();
    const semi_handle_t final_seek = semi_player_seek(2'000'000);
    const semi_handle_t resume = semi_player_play();
    await_ok(first_seek);
    await_ok(second_seek);
    await_ok(pause);
    await_ok(final_seek);
    await_ok(resume);

    await_ok(semi_player_close());

    const semi_command_result_t reopen_result =
        await_ok(semi_player_open(SEMI_PLAYER_TEST_MEDIA_PATH));
    EXPECT_NE(reopen_result.has_media_info, 0U);
    await_ok(semi_player_close());

    EXPECT_EQ(semi_player_shutdown(), SEMI_OK);
    EXPECT_EQ(semi_player_shutdown(), SEMI_OK);
    EXPECT_EQ(semi_player_init(), SEMI_OK);
    EXPECT_EQ(semi_player_shutdown(), SEMI_OK);
}

} // namespace
