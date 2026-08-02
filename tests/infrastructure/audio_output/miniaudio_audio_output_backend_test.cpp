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

class CountingProgressNotifier final
    : public contracts::audio_output::AudioOutputBackendProgressNotifier {
public:
    void notify_audio_output_progress_available() noexcept override { ++calls; }

    std::atomic_int calls = 0;
};

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
    MiniaudioAudioOutputBackend backend;

    const auto submitted = backend.try_submit(make_audio({}));
    const auto drained = backend.try_drain();

    ASSERT_FALSE(submitted.has_value());
    ASSERT_FALSE(drained.has_value());
    EXPECT_EQ(submitted.error().operation, AudioOutputBackendOperation::Submit);
    EXPECT_EQ(drained.error().operation, AudioOutputBackendOperation::Drain);
}

TEST(MiniaudioAudioOutputBackendTest, RejectsDeviceIdUntilSelectionIsImplemented) {
    MiniaudioAudioOutputBackend backend;

    const auto configured = backend.configure({.device_id = "not-yet-supported"});

    ASSERT_FALSE(configured.has_value());
    EXPECT_EQ(configured.error().operation, AudioOutputBackendOperation::Configure);
}

TEST(MiniaudioAudioOutputBackendTest, ConfigureSubmitDrainWhenDeviceIsAvailable) {
    MiniaudioAudioOutputBackend backend;
    CountingProgressNotifier notifier;
    backend.set_progress_notifier(&notifier);

    const auto configured = backend.configure({});
    if (!configured.has_value()) {
        GTEST_SKIP() << "miniaudio playback device unavailable: " << configured.error().message;
    }

    EXPECT_EQ(configured->playback_format.sample_rate, 48000U);
    EXPECT_EQ(configured->playback_format.channels, 2U);
    EXPECT_EQ(configured->playback_format.sample_format, AudioSampleFormat::F32);
    EXPECT_FALSE(configured->playback_format.planar);

    const auto submitted = backend.try_submit(make_audio(configured->playback_format));
    ASSERT_TRUE(submitted.has_value()) << submitted.error().message;
    EXPECT_EQ(*submitted, AudioOutputSubmitStatus::Accepted);

    backend.reset();
    const auto drained = backend.try_drain();
    ASSERT_TRUE(drained.has_value()) << drained.error().message;
    EXPECT_EQ(*drained, AudioOutputDrainStatus::Drained);

    backend.unconfigure();
    const auto after_unconfigure = backend.try_submit(make_audio(configured->playback_format));
    ASSERT_FALSE(after_unconfigure.has_value());
    EXPECT_EQ(after_unconfigure.error().operation, AudioOutputBackendOperation::Submit);
}

TEST(MiniaudioAudioOutputBackendTest, RejectsUnexpectedFormatWhenDeviceIsAvailable) {
    MiniaudioAudioOutputBackend backend;

    const auto configured = backend.configure({});
    if (!configured.has_value()) {
        GTEST_SKIP() << "miniaudio playback device unavailable: " << configured.error().message;
    }

    auto wrong_format = configured->playback_format;
    wrong_format.sample_rate = 44100;
    const auto submitted = backend.try_submit(make_audio(wrong_format));

    ASSERT_FALSE(submitted.has_value());
    EXPECT_EQ(submitted.error().operation, AudioOutputBackendOperation::Submit);
}

} // namespace
} // namespace semi::infra::audio_output
