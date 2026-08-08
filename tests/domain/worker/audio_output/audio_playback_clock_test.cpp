#include "domain/worker/audio_output/audio_playback_clock.hpp"

#include <gtest/gtest.h>

namespace semi::domain {

TEST(AudioPlaybackClockStateTest, ConsumedFramesEstablishAndAdvancePosition) {
    AudioPlaybackClockState clock;
    clock.configure(48'000);
    EXPECT_FALSE(clock.current_pts());

    clock.on_audio_frames_consumed({.generation = 7, .first_pts_us = 1'000'000,
                                    .frames = 4'800, .sample_rate = 48'000});
    ASSERT_TRUE(clock.current_pts());
    EXPECT_GE(*clock.current_pts(), 1'100'000);
}

TEST(AudioPlaybackClockStateTest, PauseAndPreparedSeekFreezePosition) {
    AudioPlaybackClockState clock;
    clock.configure(48'000);
    clock.prepare_pcm(2, 5'000'000);
    clock.pause();
    EXPECT_EQ(clock.current_pts(), 5'000'000);

    clock.on_audio_frames_consumed({.generation = 2, .first_pts_us = 5'000'000,
                                    .frames = 480, .sample_rate = 48'000});
    EXPECT_EQ(clock.current_pts(), 5'010'000);
}

TEST(AudioPlaybackClockStateTest, ResetAndFinishRemoveProgress) {
    AudioPlaybackClockState clock;
    clock.configure(48'000);
    clock.on_audio_frames_consumed({.generation = 1, .first_pts_us = 0,
                                    .frames = 480, .sample_rate = 48'000});
    clock.finish();
    EXPECT_TRUE(clock.current_pts());
    clock.reset();
    EXPECT_FALSE(clock.current_pts());
}

} // namespace semi::domain
