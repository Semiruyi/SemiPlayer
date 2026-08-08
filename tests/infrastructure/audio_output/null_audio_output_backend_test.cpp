#include "infrastructure/audio_output/null_audio_output_backend.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace semi::infra::audio_output {
namespace {

using contracts::audio_output::AudioOutputBackendOperation;
using contracts::audio_output::AudioOutputDrainStatus;
using contracts::audio_output::AudioOutputSubmitStatus;
using contracts::media::AudioPcmFormat;
using contracts::media::AudioSampleFormat;
using contracts::media::DecodedAudio;

class FrameSink final : public infra::RealTimeNotificationSink<std::uint32_t> {
public:
    void on_realtime_notification(const std::uint32_t& confirmed_frames) noexcept override {
        total_frames += confirmed_frames;
    }

    std::uint32_t total_frames = 0;
};

DecodedAudio make_audio(AudioPcmFormat format) {
    return DecodedAudio{
        .format = format,
        .samples_per_channel = 1,
        .planes = {{std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
                    std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}}},
        .pts_us = 0,
    };
}

TEST(NullAudioOutputBackendTest, ConfigureReturnsFixedPlaybackFormat) {
    NullAudioOutputBackend backend{nullptr};

    const auto configured = backend.configure({});

    ASSERT_TRUE(configured.has_value()) << configured.error().message;
    EXPECT_EQ(configured->playback_format.sample_rate, 48000U);
    EXPECT_EQ(configured->playback_format.channels, 2U);
    EXPECT_EQ(configured->playback_format.sample_format, AudioSampleFormat::F32);
    EXPECT_FALSE(configured->playback_format.planar);
}

TEST(NullAudioOutputBackendTest, AcceptsMatchingPcmAndDrainsImmediately) {
    NullAudioOutputBackend backend{nullptr};
    const auto configured = backend.configure({});
    ASSERT_TRUE(configured.has_value()) << configured.error().message;

    const auto submitted = backend.try_submit(make_audio(configured->playback_format));
    const auto drained = backend.try_drain();

    ASSERT_TRUE(submitted.has_value()) << submitted.error().message;
    ASSERT_TRUE(drained.has_value()) << drained.error().message;
    EXPECT_EQ(*submitted, AudioOutputSubmitStatus::Accepted);
    EXPECT_EQ(*drained, AudioOutputDrainStatus::Drained);
}

TEST(NullAudioOutputBackendTest, PauseAndResumeAreNoOpsWhenConfigured) {
    NullAudioOutputBackend backend{nullptr};
    ASSERT_TRUE(backend.configure({}).has_value());

    EXPECT_TRUE(backend.pause().has_value());
    EXPECT_TRUE(backend.resume().has_value());
}

TEST(NullAudioOutputBackendTest, RejectsPauseAndResumeBeforeConfiguration) {
    NullAudioOutputBackend backend{nullptr};

    const auto paused = backend.pause();
    const auto resumed = backend.resume();

    ASSERT_FALSE(paused.has_value());
    ASSERT_FALSE(resumed.has_value());
    EXPECT_EQ(paused.error().operation, AudioOutputBackendOperation::Pause);
    EXPECT_EQ(resumed.error().operation, AudioOutputBackendOperation::Resume);
}

TEST(NullAudioOutputBackendTest, RejectsSubmitAndDrainBeforeConfiguration) {
    NullAudioOutputBackend backend{nullptr};

    const auto submitted = backend.try_submit(make_audio({}));
    const auto drained = backend.try_drain();

    ASSERT_FALSE(submitted.has_value());
    ASSERT_FALSE(drained.has_value());
    EXPECT_EQ(submitted.error().operation, AudioOutputBackendOperation::Submit);
    EXPECT_EQ(drained.error().operation, AudioOutputBackendOperation::Drain);
}

TEST(NullAudioOutputBackendTest, RejectsUnexpectedPcmFormat) {
    NullAudioOutputBackend backend{nullptr};
    const auto configured = backend.configure({});
    ASSERT_TRUE(configured.has_value()) << configured.error().message;

    auto wrong_format = configured->playback_format;
    wrong_format.sample_rate = 44100;
    const auto submitted = backend.try_submit(make_audio(wrong_format));

    ASSERT_FALSE(submitted.has_value());
    EXPECT_EQ(submitted.error().operation, AudioOutputBackendOperation::Submit);
}

TEST(NullAudioOutputBackendTest, ResetKeepsConfigurationReusable) {
    NullAudioOutputBackend backend{nullptr};
    const auto configured = backend.configure({});
    ASSERT_TRUE(configured.has_value()) << configured.error().message;

    const auto reset = backend.reset();
    ASSERT_TRUE(reset.has_value()) << reset.error().message;
    const auto submitted = backend.try_submit(make_audio(configured->playback_format));

    ASSERT_TRUE(submitted.has_value()) << submitted.error().message;
    EXPECT_EQ(*submitted, AudioOutputSubmitStatus::Accepted);
}

TEST(NullAudioOutputBackendTest, PublishesConsumedFramesThroughTheRealtimeNotifier) {
    auto notifier = std::make_shared<contracts::audio_output::AudioOutputRealTimeNotifier>();
    NullAudioOutputBackend backend{notifier};
    FrameSink sink;

    ASSERT_TRUE(notifier->register_sink(sink));
    ASSERT_TRUE(notifier->seal());

    const auto configured = backend.configure({});
    ASSERT_TRUE(configured.has_value()) << configured.error().message;
    const auto submitted = backend.try_submit(make_audio(configured->playback_format));

    ASSERT_TRUE(submitted.has_value()) << submitted.error().message;
    EXPECT_EQ(sink.total_frames, 1U);

    backend.unconfigure();
    ASSERT_TRUE(notifier->unseal());
    EXPECT_TRUE(notifier->unregister_sink(sink));
}

TEST(NullAudioOutputBackendTest, UnconfigureReleasesTheSession) {
    NullAudioOutputBackend backend{nullptr};
    ASSERT_TRUE(backend.configure({}).has_value());

    backend.unconfigure();
    const auto submitted = backend.try_submit(make_audio({}));

    ASSERT_FALSE(submitted.has_value());
    EXPECT_EQ(submitted.error().operation, AudioOutputBackendOperation::Submit);
}

} // namespace
} // namespace semi::infra::audio_output
