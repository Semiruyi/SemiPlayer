#include "domain/worker/audio_output/audio_playback_clock.hpp"

#include <gtest/gtest.h>

namespace semi::domain {

TEST(AudioPlaybackClockStateTest, ConsumedFramesEstablishAndAdvancePosition) {
    AudioPlaybackClockState clock;
    clock.configure(48'000);
    EXPECT_FALSE(clock.current_position());

    clock.on_audio_frames_consumed({.generation = 7, .first_pts_us = 1'000'000,
                                    .frames = 4'800, .sample_rate = 48'000});
    ASSERT_TRUE(clock.current_position());
    EXPECT_GE(clock.current_position()->pts_us, 1'100'000);
}

TEST(AudioPlaybackClockStateTest, PauseAndPreparedSeekFreezePosition) {
    AudioPlaybackClockState clock;
    clock.configure(48'000);
    EXPECT_TRUE(clock.prepare_pcm(2, 5'000'000));
    clock.pause();
    EXPECT_EQ(clock.current_position()->pts_us, 5'000'000);

    clock.on_audio_frames_consumed({.generation = 2, .first_pts_us = 5'000'000,
                                    .frames = 480, .sample_rate = 48'000});
    EXPECT_EQ(clock.current_position()->pts_us, 5'010'000);
}

TEST(AudioPlaybackClockStateTest, DoesNotReplaceAConsumedAnchorWithLaterPcm) {
    AudioPlaybackClockState clock;
    clock.configure(48'000);
    EXPECT_TRUE(clock.prepare_pcm(3, 1'000'000));
    clock.on_audio_frames_consumed({.generation = 3, .first_pts_us = 1'000'000,
                                    .frames = 480, .sample_rate = 48'000});

    EXPECT_FALSE(clock.prepare_pcm(3, 2'000'000));
    ASSERT_TRUE(clock.current_position());
    EXPECT_LT(clock.current_position()->pts_us, 2'000'000);
}

TEST(AudioPlaybackClockStateTest, ReadingCarriesTheTimelineGeneration) {
    AudioPlaybackClockState clock;
    clock.configure(48'000);
    ASSERT_TRUE(clock.prepare_pcm(11, 4'000'000));
    clock.pause();

    const auto reading = clock.current_position();
    ASSERT_TRUE(reading);
    EXPECT_EQ(reading->generation, 11U);
    EXPECT_EQ(reading->pts_us, 4'000'000);

    clock.reset();
    EXPECT_FALSE(clock.current_position());
}

TEST(AudioPlaybackClockStateTest, ResetAndFinishRemoveProgress) {
    AudioPlaybackClockState clock;
    clock.configure(48'000);
    clock.on_audio_frames_consumed({.generation = 1, .first_pts_us = 0,
                                    .frames = 480, .sample_rate = 48'000});
    clock.finish();
    EXPECT_TRUE(clock.current_position());
    clock.reset();
    EXPECT_FALSE(clock.current_position());
}

} // namespace semi::domain
