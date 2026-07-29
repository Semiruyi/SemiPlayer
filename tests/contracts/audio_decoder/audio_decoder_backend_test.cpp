#include "contracts/audio_decoder/audio_decoder_backend.hpp"

#include <gtest/gtest.h>

#include <utility>

namespace semi::contracts::audio_decoder {
namespace {

class FakeAudioDecoderBackend final : public AudioDecoderBackend {
public:
    std::expected<void, AudioDecoderBackendError>
    configure(const media::AudioCodecConfig&) override {
        return {};
    }

    std::expected<DecodedAudioBatch, AudioDecoderBackendError>
    decode(const demuxer::packet::EncodedPacket&) override {
        return DecodedAudioBatch{};
    }

    std::expected<DecodedAudioBatch, AudioDecoderBackendError> drain() override {
        return DecodedAudioBatch{};
    }

    void reset() noexcept override {}
    void unconfigure() noexcept override {}
};

TEST(AudioDecoderBackendContract, RepresentsAnEmptyDecodedBatch) {
    FakeAudioDecoderBackend backend;

    auto drained = backend.drain();

    ASSERT_TRUE(drained.has_value());
    EXPECT_TRUE(drained->empty());
}

TEST(AudioDecoderBackendContract, PreservesOperationInStructuredError) {
    const AudioDecoderBackendError error{
        .operation = AudioDecoderBackendOperation::Drain,
        .native_code = -123,
        .message = "decoder drain failed",
    };

    EXPECT_EQ(error.operation, AudioDecoderBackendOperation::Drain);
    EXPECT_EQ(error.native_code, -123);
    EXPECT_EQ(error.message, "decoder drain failed");
}

} // namespace
} // namespace semi::contracts::audio_decoder
