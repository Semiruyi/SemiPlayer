#include "contracts/audio_resampler/audio_resampler_backend.hpp"

#include <gtest/gtest.h>

namespace semi::contracts::audio_resampler {
namespace {

class FakeAudioResamplerBackend final : public AudioResamplerBackend {
public:
    std::expected<void, AudioResamplerBackendError>
    configure(const media::AudioPcmFormat&, const media::AudioPcmFormat&) override {
        return {};
    }

    std::expected<ResampledAudioBatch, AudioResamplerBackendError>
    resample(const media::DecodedAudio&) override {
        return ResampledAudioBatch{};
    }

    std::expected<ResampledAudioBatch, AudioResamplerBackendError> drain() override {
        return ResampledAudioBatch{};
    }

    void reset() noexcept override {}
    void unconfigure() noexcept override {}
};

TEST(AudioResamplerBackendContract, RepresentsAnEmptyResampledBatch) {
    FakeAudioResamplerBackend backend;

    auto drained = backend.drain();

    ASSERT_TRUE(drained.has_value());
    EXPECT_TRUE(drained->empty());
}

TEST(AudioResamplerBackendContract, PreservesOperationInStructuredError) {
    const AudioResamplerBackendError error{
        .operation = AudioResamplerBackendOperation::Drain,
        .native_code = -123,
        .message = "resampler drain failed",
    };

    EXPECT_EQ(error.operation, AudioResamplerBackendOperation::Drain);
    EXPECT_EQ(error.native_code, -123);
    EXPECT_EQ(error.message, "resampler drain failed");
}

} // namespace
} // namespace semi::contracts::audio_resampler
