#include "domain/worker/audio_output/audio_playback_clock.hpp"

#include <gtest/gtest.h>

namespace semi::domain {

TEST(AudioPlaybackClockStateTest, ConsumedFramesEstablishAndAdvancePosition) {
    AudioPlaybackClockState clock;
    clock.configure(48'000, 7);
    EXPECT_FALSE(clock.current_position());

    ASSERT_TRUE(clock.prepare_pcm(7, 1'000'000));
    clock.on_audio_frames_consumed(7, 4'800);
    ASSERT_TRUE(clock.current_position());
    EXPECT_GE(clock.current_position()->pts_us, 1'100'000);
}

TEST(AudioPlaybackClockStateTest, PauseAndPreparedSeekFreezePosition) {
    AudioPlaybackClockState clock;
    clock.configure(48'000, 2);
    EXPECT_TRUE(clock.prepare_pcm(2, 5'000'000));
    clock.pause(2);
    EXPECT_EQ(clock.current_position()->pts_us, 5'000'000);

    clock.on_audio_frames_consumed(2, 480);
    EXPECT_EQ(clock.current_position()->pts_us, 5'010'000);
}

TEST(AudioPlaybackClockStateTest, DoesNotReplaceAConsumedAnchorWithLaterPcm) {
    AudioPlaybackClockState clock;
    clock.configure(48'000, 3);
    EXPECT_TRUE(clock.prepare_pcm(3, 1'000'000));
    clock.on_audio_frames_consumed(3, 480);

    EXPECT_FALSE(clock.prepare_pcm(3, 2'000'000));
    ASSERT_TRUE(clock.current_position());
    EXPECT_LT(clock.current_position()->pts_us, 2'000'000);
}

TEST(AudioPlaybackClockStateTest, ReadingCarriesTheTimelineGeneration) {
    AudioPlaybackClockState clock;
    clock.configure(48'000, 11);
    ASSERT_TRUE(clock.prepare_pcm(11, 4'000'000));
    clock.pause(11);

    const auto reading = clock.current_position();
    ASSERT_TRUE(reading);
    EXPECT_EQ(reading->generation, 11U);
    EXPECT_EQ(reading->pts_us, 4'000'000);

    clock.reset(11);
    EXPECT_FALSE(clock.current_position());

    EXPECT_FALSE(clock.prepare_pcm(12, 5'000'000));
    clock.reset(12);
    EXPECT_TRUE(clock.prepare_pcm(12, 5'000'000));
}

TEST(AudioPlaybackClockStateTest, ResetAndFinishRemoveProgress) {
    AudioPlaybackClockState clock;
    clock.configure(48'000, 1);
    ASSERT_TRUE(clock.prepare_pcm(1, 0));
    clock.on_audio_frames_consumed(1, 480);
    clock.finish(1);
    EXPECT_TRUE(clock.current_position());
    clock.reset(1);
    EXPECT_FALSE(clock.current_position());
}

TEST(AudioPlaybackClockStateTest, IgnoresFramesFromAnotherGeneration) {
    AudioPlaybackClockState clock;
    clock.configure(48'000, 7);
    ASSERT_TRUE(clock.prepare_pcm(7, 1'000'000));
    clock.on_audio_frames_consumed(7, 480);
    clock.pause(7);

    const auto before = clock.current_position();
    ASSERT_TRUE(before);

    clock.on_audio_frames_consumed(8, 480);

    ASSERT_TRUE(clock.current_position());
    EXPECT_EQ(clock.current_position()->generation, before->generation);
    EXPECT_EQ(clock.current_position()->pts_us, before->pts_us);
}

} // namespace semi::domain
