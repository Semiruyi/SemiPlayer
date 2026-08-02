#include "infrastructure/audio_output/null_audio_output_backend.hpp"

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
    NullAudioOutputBackend backend;

    const auto configured = backend.configure({});

    ASSERT_TRUE(configured.has_value()) << configured.error().message;
    EXPECT_EQ(configured->playback_format.sample_rate, 48000U);
    EXPECT_EQ(configured->playback_format.channels, 2U);
    EXPECT_EQ(configured->playback_format.sample_format, AudioSampleFormat::F32);
    EXPECT_FALSE(configured->playback_format.planar);
}

TEST(NullAudioOutputBackendTest, AcceptsMatchingPcmAndDrainsImmediately) {
    NullAudioOutputBackend backend;
    const auto configured = backend.configure({});
    ASSERT_TRUE(configured.has_value()) << configured.error().message;

    const auto submitted = backend.try_submit(make_audio(configured->playback_format));
    const auto drained = backend.try_drain();

    ASSERT_TRUE(submitted.has_value()) << submitted.error().message;
    ASSERT_TRUE(drained.has_value()) << drained.error().message;
    EXPECT_EQ(*submitted, AudioOutputSubmitStatus::Accepted);
    EXPECT_EQ(*drained, AudioOutputDrainStatus::Drained);
}

TEST(NullAudioOutputBackendTest, RejectsSubmitAndDrainBeforeConfiguration) {
    NullAudioOutputBackend backend;

    const auto submitted = backend.try_submit(make_audio({}));
    const auto drained = backend.try_drain();

    ASSERT_FALSE(submitted.has_value());
    ASSERT_FALSE(drained.has_value());
    EXPECT_EQ(submitted.error().operation, AudioOutputBackendOperation::Submit);
    EXPECT_EQ(drained.error().operation, AudioOutputBackendOperation::Drain);
}

TEST(NullAudioOutputBackendTest, RejectsUnexpectedPcmFormat) {
    NullAudioOutputBackend backend;
    const auto configured = backend.configure({});
    ASSERT_TRUE(configured.has_value()) << configured.error().message;

    auto wrong_format = configured->playback_format;
    wrong_format.sample_rate = 44100;
    const auto submitted = backend.try_submit(make_audio(wrong_format));

    ASSERT_FALSE(submitted.has_value());
    EXPECT_EQ(submitted.error().operation, AudioOutputBackendOperation::Submit);
}

TEST(NullAudioOutputBackendTest, ResetKeepsConfigurationReusable) {
    NullAudioOutputBackend backend;
    const auto configured = backend.configure({});
    ASSERT_TRUE(configured.has_value()) << configured.error().message;

    backend.reset();
    const auto submitted = backend.try_submit(make_audio(configured->playback_format));

    ASSERT_TRUE(submitted.has_value()) << submitted.error().message;
    EXPECT_EQ(*submitted, AudioOutputSubmitStatus::Accepted);
}

TEST(NullAudioOutputBackendTest, UnconfigureReleasesTheSession) {
    NullAudioOutputBackend backend;
    ASSERT_TRUE(backend.configure({}).has_value());

    backend.unconfigure();
    const auto submitted = backend.try_submit(make_audio({}));

    ASSERT_FALSE(submitted.has_value());
    EXPECT_EQ(submitted.error().operation, AudioOutputBackendOperation::Submit);
}

TEST(NullAudioOutputBackendTest, NotifiesProgressOnConfigureAndReset) {
    NullAudioOutputBackend backend;
    CountingProgressNotifier notifier;
    backend.set_progress_notifier(&notifier);

    ASSERT_TRUE(backend.configure({}).has_value());
    backend.reset();

    EXPECT_EQ(notifier.calls, 2);
}

} // namespace
} // namespace semi::infra::audio_output
