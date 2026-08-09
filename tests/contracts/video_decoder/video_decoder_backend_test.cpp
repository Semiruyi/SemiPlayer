#include "contracts/video_decoder/video_decoder_backend.hpp"

#include <gtest/gtest.h>

namespace semi::contracts::video_decoder {
namespace {

class FakeVideoDecoderBackend final : public VideoDecoderBackend {
public:
    std::expected<void, VideoDecoderBackendError>
    configure(const media::VideoCodecConfig&) override {
        return {};
    }

    std::expected<DecodedVideoBatch, VideoDecoderBackendError>
    decode(const demuxer::packet::EncodedPacket&) override {
        return DecodedVideoBatch{};
    }

    std::expected<DecodedVideoBatch, VideoDecoderBackendError> drain() override {
        return DecodedVideoBatch{};
    }

    void reset() noexcept override {}
    void unconfigure() noexcept override {}
};

TEST(VideoDecoderBackendContract, RepresentsAnEmptyDecodedBatch) {
    FakeVideoDecoderBackend backend;

    const auto configured = backend.configure({});
    const auto decoded = backend.decode({});
    const auto drained = backend.drain();

    ASSERT_TRUE(configured.has_value());
    ASSERT_TRUE(decoded.has_value());
    EXPECT_TRUE(decoded->empty());
    ASSERT_TRUE(drained.has_value());
    EXPECT_TRUE(drained->empty());
}

TEST(VideoDecoderBackendContract, PreservesOperationInStructuredError) {
    const VideoDecoderBackendError error{
        .operation = VideoDecoderBackendOperation::Drain,
        .native_code = -123,
        .message = "decoder drain failed",
    };

    EXPECT_EQ(error.operation, VideoDecoderBackendOperation::Drain);
    EXPECT_EQ(error.native_code, -123);
    EXPECT_EQ(error.message, "decoder drain failed");
}

} // namespace
} // namespace semi::contracts::video_decoder
