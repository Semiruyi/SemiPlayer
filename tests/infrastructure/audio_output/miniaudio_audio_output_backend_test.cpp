#include "infrastructure/audio_output/miniaudio_audio_output_backend.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace semi::infra::audio_output {
namespace {

using contracts::audio_output::AudioOutputBackendOperation;
using contracts::audio_output::AudioOutputDrainStatus;
using contracts::audio_output::AudioOutputSubmitStatus;
using contracts::media::AudioPcmFormat;
using contracts::media::AudioSampleFormat;
using contracts::media::DecodedAudio;

DecodedAudio make_audio(AudioPcmFormat format, std::uint32_t samples_per_channel = 128) {
    const std::size_t bytes_per_sample = sizeof(float);
    const std::size_t byte_count =
        static_cast<std::size_t>(samples_per_channel) * format.channels * bytes_per_sample;
    return DecodedAudio{
        .format = format,
        .samples_per_channel = samples_per_channel,
        .planes = {std::vector<std::byte>(byte_count, std::byte{0})},
        .pts_us = 0,
    };
}

TEST(MiniaudioAudioOutputBackendTest, RejectsSubmitAndDrainBeforeConfiguration) {
    MiniaudioAudioOutputBackend backend{nullptr};

    const auto submitted = backend.try_submit({.audio = make_audio({}), .generation = 1});
    const auto drained = backend.try_drain();

    ASSERT_FALSE(submitted.has_value());
    ASSERT_FALSE(drained.has_value());
    EXPECT_EQ(submitted.error().operation, AudioOutputBackendOperation::Submit);
    EXPECT_EQ(drained.error().operation, AudioOutputBackendOperation::Drain);
}

TEST(MiniaudioAudioOutputBackendTest, RejectsPauseAndResumeBeforeConfiguration) {
    MiniaudioAudioOutputBackend backend{nullptr};

    const auto paused = backend.pause();
    const auto resumed = backend.resume();

    ASSERT_FALSE(paused.has_value());
    ASSERT_FALSE(resumed.has_value());
    EXPECT_EQ(paused.error().operation, AudioOutputBackendOperation::Pause);
    EXPECT_EQ(resumed.error().operation, AudioOutputBackendOperation::Resume);
}

TEST(MiniaudioAudioOutputBackendTest, RejectsDeviceIdUntilSelectionIsImplemented) {
    MiniaudioAudioOutputBackend backend{nullptr};

    const auto configured = backend.configure({.device_id = "not-yet-supported"});

    ASSERT_FALSE(configured.has_value());
    EXPECT_EQ(configured.error().operation, AudioOutputBackendOperation::Configure);
}

TEST(MiniaudioAudioOutputBackendTest, ConfigureSubmitDrainWhenDeviceIsAvailable) {
    MiniaudioAudioOutputBackend backend{nullptr};
    const auto configured = backend.configure({});
    if (!configured.has_value()) {
        GTEST_SKIP() << "miniaudio playback device unavailable: " << configured.error().message;
    }

    EXPECT_EQ(configured->playback_format.sample_rate, 48000U);
    EXPECT_EQ(configured->playback_format.channels, 2U);
    EXPECT_EQ(configured->playback_format.sample_format, AudioSampleFormat::F32);
    EXPECT_FALSE(configured->playback_format.planar);

    const auto submitted = backend.try_submit({.audio = make_audio(configured->playback_format), .generation = 1});
    ASSERT_TRUE(submitted.has_value()) << submitted.error().message;
    EXPECT_EQ(*submitted, AudioOutputSubmitStatus::Accepted);

    const auto reset = backend.reset();
    ASSERT_TRUE(reset.has_value()) << reset.error().message;
    const auto drained = backend.try_drain();
    ASSERT_TRUE(drained.has_value()) << drained.error().message;
    EXPECT_EQ(*drained, AudioOutputDrainStatus::Drained);

    backend.unconfigure();
    const auto after_unconfigure = backend.try_submit({.audio = make_audio(configured->playback_format), .generation = 1});
    ASSERT_FALSE(after_unconfigure.has_value());
    EXPECT_EQ(after_unconfigure.error().operation, AudioOutputBackendOperation::Submit);
}

TEST(MiniaudioAudioOutputBackendTest, PausePreservesBufferedSamplesUntilResume) {
    MiniaudioAudioOutputBackend backend{nullptr};
    const auto configured = backend.configure({});
    if (!configured.has_value()) {
        GTEST_SKIP() << "miniaudio playback device unavailable: " << configured.error().message;
    }

    ASSERT_TRUE(backend.resume().has_value());
    const auto submitted = backend.try_submit({.audio = make_audio(configured->playback_format, 48'000), .generation = 1});
    ASSERT_TRUE(submitted.has_value()) << submitted.error().message;

    ASSERT_TRUE(backend.pause().has_value());
    const auto paused_drain = backend.try_drain();
    ASSERT_TRUE(paused_drain.has_value()) << paused_drain.error().message;
    EXPECT_EQ(*paused_drain, AudioOutputDrainStatus::WouldBlock);

    ASSERT_TRUE(backend.resume().has_value());
    const auto reset = backend.reset();
    ASSERT_TRUE(reset.has_value()) << reset.error().message;
    const auto drained = backend.try_drain();
    ASSERT_TRUE(drained.has_value()) << drained.error().message;
    EXPECT_EQ(*drained, AudioOutputDrainStatus::Drained);

    backend.unconfigure();
}

TEST(MiniaudioAudioOutputBackendTest, RejectsUnexpectedFormatWhenDeviceIsAvailable) {
    MiniaudioAudioOutputBackend backend{nullptr};

    const auto configured = backend.configure({});
    if (!configured.has_value()) {
        GTEST_SKIP() << "miniaudio playback device unavailable: " << configured.error().message;
    }

    auto wrong_format = configured->playback_format;
    wrong_format.sample_rate = 44100;
    const auto submitted = backend.try_submit({.audio = make_audio(wrong_format), .generation = 1});

    ASSERT_FALSE(submitted.has_value());
    EXPECT_EQ(submitted.error().operation, AudioOutputBackendOperation::Submit);
}

} // namespace
} // namespace semi::infra::audio_output
