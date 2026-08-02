#include "contracts/audio_output/audio_output_backend.hpp"

#include <gtest/gtest.h>

#include <atomic>

namespace semi::contracts::audio_output {
namespace {

media::AudioPcmFormat playback_format() {
    return media::AudioPcmFormat{
        .sample_rate = 48000,
        .channels = 2,
        .sample_format = media::AudioSampleFormat::F32,
        .planar = false,
    };
}

class FakeAudioOutputBackend final : public AudioOutputBackend {
public:
    void set_progress_notifier(AudioOutputBackendProgressNotifier* notifier) noexcept override {
        progress_notifier = notifier;
    }

    std::expected<AudioOutputConfigureResult, AudioOutputBackendError>
    configure(const AudioOutputOptions& options) override {
        last_options = options;
        return AudioOutputConfigureResult{.playback_format = playback_format()};
    }

    std::expected<AudioOutputSubmitStatus, AudioOutputBackendError>
    try_submit(const media::DecodedAudio&) override {
        return submit_status;
    }

    std::expected<AudioOutputDrainStatus, AudioOutputBackendError> try_drain() override {
        return drain_status;
    }

    void reset() noexcept override { ++reset_calls; }
    void unconfigure() noexcept override { ++unconfigure_calls; }

    AudioOutputSubmitStatus submit_status = AudioOutputSubmitStatus::Accepted;
    AudioOutputDrainStatus drain_status = AudioOutputDrainStatus::Drained;
    AudioOutputBackendProgressNotifier* progress_notifier = nullptr;
    AudioOutputOptions last_options;
    std::atomic_int reset_calls = 0;
    std::atomic_int unconfigure_calls = 0;
};

class CountingProgressNotifier final : public AudioOutputBackendProgressNotifier {
public:
    void notify_audio_output_progress_available() noexcept override { ++calls; }

    std::atomic_int calls = 0;
};

TEST(AudioOutputBackendContract, ConfigureReturnsPlaybackFormat) {
    FakeAudioOutputBackend backend;

    const auto configured = backend.configure(AudioOutputOptions{.device_id = "default"});

    ASSERT_TRUE(configured.has_value());
    EXPECT_EQ(configured->playback_format.sample_rate, 48000U);
    ASSERT_TRUE(backend.last_options.device_id.has_value());
    EXPECT_EQ(*backend.last_options.device_id, "default");
}

TEST(AudioOutputBackendContract, RepresentsSubmitAndDrainBackpressure) {
    FakeAudioOutputBackend backend;
    backend.submit_status = AudioOutputSubmitStatus::WouldBlock;
    backend.drain_status = AudioOutputDrainStatus::WouldBlock;

    const auto submitted = backend.try_submit({});
    const auto drained = backend.try_drain();

    ASSERT_TRUE(submitted.has_value());
    ASSERT_TRUE(drained.has_value());
    EXPECT_EQ(*submitted, AudioOutputSubmitStatus::WouldBlock);
    EXPECT_EQ(*drained, AudioOutputDrainStatus::WouldBlock);
}

TEST(AudioOutputBackendContract, PreservesOperationInStructuredError) {
    const AudioOutputBackendError error{
        .operation = AudioOutputBackendOperation::Submit,
        .native_code = -123,
        .message = "audio output submit failed",
    };

    EXPECT_EQ(error.operation, AudioOutputBackendOperation::Submit);
    EXPECT_EQ(error.native_code, -123);
    EXPECT_EQ(error.message, "audio output submit failed");
}

TEST(AudioOutputBackendProgressNotifier, SignalsProgressWithoutExposingBackendState) {
    CountingProgressNotifier notifier;

    notifier.notify_audio_output_progress_available();

    EXPECT_EQ(notifier.calls, 1);
}

} // namespace
} // namespace semi::contracts::audio_output
