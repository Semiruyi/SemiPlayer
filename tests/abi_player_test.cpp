#include "semi_player/semi_player.h"

#include <gtest/gtest.h>

namespace {

semi_command_result_t await_ok(semi_handle_t handle) {
    EXPECT_NE(handle, 0U);
    semi_command_result_t result{};
    EXPECT_EQ(semi_player_handle_await(handle, &result), SEMI_OK);
    return result;
}

TEST(SemiPlayerAbiTest, RunsSeekPauseResumeAndLifecycleThroughSharedLibrary) {
    ASSERT_EQ(semi_player_shutdown(), SEMI_OK);
    ASSERT_EQ(semi_player_init(), SEMI_OK);
    EXPECT_EQ(semi_player_init(), SEMI_OK);

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
    const semi_handle_t first_seek = semi_player_seek(500'000);
    const semi_handle_t second_seek = semi_player_seek(1'250'000);
    const semi_handle_t pause = semi_player_pause();
    const semi_handle_t final_seek = semi_player_seek(2'000'000);
    const semi_handle_t resume = semi_player_play();
    await_ok(play);
    await_ok(first_seek);
    await_ok(second_seek);
    await_ok(pause);
    await_ok(final_seek);
    await_ok(resume);

    await_ok(semi_player_close());

    const semi_command_result_t reopen_result = await_ok(semi_player_open(SEMI_PLAYER_TEST_MEDIA_PATH));
    EXPECT_NE(reopen_result.has_media_info, 0U);
    await_ok(semi_player_close());

    EXPECT_EQ(semi_player_shutdown(), SEMI_OK);
    EXPECT_EQ(semi_player_shutdown(), SEMI_OK);
    EXPECT_EQ(semi_player_init(), SEMI_OK);
    EXPECT_EQ(semi_player_shutdown(), SEMI_OK);
}

} // namespace
