#include "domain/worker/video_sync/video_sync_clock.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <expected>
#include <optional>

namespace semi::domain {
namespace {

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

TEST(VideoSyncClockTest, RequiresMatchingAudioGenerationBeforeReturningAClock) {
    auto audio_output = std::make_shared<FakeAudioOutput>();
    VideoSyncClock clock(audio_output);
    clock.configure(true, 5);

    auto snapshot = clock.snapshot();
    EXPECT_TRUE(snapshot.external_clock_required);
    EXPECT_FALSE(snapshot.pts_us);

    audio_output->set_position(4, 100);
    EXPECT_FALSE(clock.snapshot().pts_us);

    audio_output->set_position(5, 120);
    snapshot = clock.snapshot();
    EXPECT_TRUE(snapshot.external_clock_required);
    ASSERT_TRUE(snapshot.pts_us);
    EXPECT_EQ(*snapshot.pts_us, 120);
}

TEST(VideoSyncClockTest, SwitchesToLocalClockAfterAudioPlaybackFinishes) {
    auto audio_output = std::make_shared<FakeAudioOutput>();
    audio_output->set_position(2, 700);
    VideoSyncClock clock(audio_output);
    clock.configure(true, 2);

    clock.on_audio_playback_finished(true);
    const auto snapshot = clock.snapshot();

    EXPECT_FALSE(snapshot.external_clock_required);
    ASSERT_TRUE(snapshot.pts_us);
    EXPECT_GE(*snapshot.pts_us, 700);
}

TEST(VideoSyncClockTest, AnchorsAndFreezesTheVideoOnlyClockWhenPaused) {
    VideoSyncClock clock(nullptr);
    clock.configure(false, 1);

    const auto anchored = clock.current_pts_for_frame(500, false);
    ASSERT_TRUE(anchored);
    EXPECT_EQ(*anchored, 500);

    clock.pause();
    const auto frozen = clock.snapshot().pts_us;
    ASSERT_TRUE(frozen);
    EXPECT_EQ(*frozen, 500);
}

} // namespace
} // namespace semi::domain
