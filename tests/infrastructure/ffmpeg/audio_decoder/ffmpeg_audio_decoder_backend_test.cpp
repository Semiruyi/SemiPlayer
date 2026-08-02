#include "infrastructure/ffmpeg/audio_decoder/ffmpeg_audio_decoder_backend.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace semi::infra::ffmpeg::audio_decoder {
namespace {

using contracts::audio_decoder::AudioDecoderBackendOperation;
using contracts::demuxer::packet::EncodedPacket;
using contracts::media::AudioCodecConfig;
using contracts::media::AudioSampleFormat;
using contracts::media::CodecCommon;

TEST(FfmpegAudioDecoderBackendTest, RejectsDecodeBeforeConfiguration) {
    FfmpegAudioDecoderBackend backend;

    const auto result = backend.decode({});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().operation, AudioDecoderBackendOperation::Decode);
}

TEST(FfmpegAudioDecoderBackendTest, DecodesOwnedU8PcmAndPreservesTimestamp) {
    FfmpegAudioDecoderBackend backend;
    const AudioCodecConfig config{
        .common = CodecCommon{.codec_name = "pcm_u8", .extradata = {}},
        .sample_rate = 8'000,
        .channels = 1,
    };
    const auto configured = backend.configure(config);
    ASSERT_TRUE(configured.has_value()) << configured.error().message;
    EXPECT_EQ(configured->decoded_format.sample_rate, 8'000U);
    EXPECT_EQ(configured->decoded_format.channels, 1U);
    EXPECT_EQ(configured->decoded_format.sample_format, AudioSampleFormat::U8);
    EXPECT_FALSE(configured->decoded_format.planar);

    const std::array<std::byte, 4> payload = {
        std::byte{0x80}, std::byte{0x81}, std::byte{0x7f}, std::byte{0x00}};
    const auto decoded = backend.decode(EncodedPacket{
        .payload = {payload.begin(), payload.end()},
        .pts_us = 123'456,
        .dts_us = std::nullopt,
        .duration_us = std::nullopt,
    });

    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
    ASSERT_EQ(decoded->size(), 1U);
    const auto& frame = decoded->front();
    EXPECT_EQ(frame.format.sample_rate, 8'000U);
    EXPECT_EQ(frame.format.channels, 1U);
    EXPECT_EQ(frame.format.sample_format, AudioSampleFormat::U8);
    EXPECT_FALSE(frame.format.planar);
    EXPECT_EQ(frame.samples_per_channel, 4U);
    ASSERT_EQ(frame.planes.size(), 1U);
    EXPECT_EQ(frame.planes.front(), std::vector<std::byte>(payload.begin(), payload.end()));
    ASSERT_TRUE(frame.pts_us.has_value());
    EXPECT_EQ(*frame.pts_us, 123'456);
}

TEST(FfmpegAudioDecoderBackendTest, MapsS64PcmToTheMediaContract) {
    FfmpegAudioDecoderBackend backend;
    const AudioCodecConfig config{
        .common = CodecCommon{.codec_name = "pcm_s64le", .extradata = {}},
        .sample_rate = 8'000,
        .channels = 1,
    };
    const auto configured = backend.configure(config);
    ASSERT_TRUE(configured.has_value()) << configured.error().message;
    EXPECT_EQ(configured->decoded_format.sample_format, AudioSampleFormat::S64);
    EXPECT_FALSE(configured->decoded_format.planar);

    const std::array<std::byte, 8> payload = {
        std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
    const auto decoded = backend.decode(EncodedPacket{
        .payload = {payload.begin(), payload.end()},
        .pts_us = std::nullopt,
        .dts_us = std::nullopt,
        .duration_us = std::nullopt,
    });

    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
    ASSERT_EQ(decoded->size(), 1U);
    const auto& frame = decoded->front();
    EXPECT_EQ(frame.format.sample_format, AudioSampleFormat::S64);
    EXPECT_EQ(frame.samples_per_channel, 1U);
    ASSERT_EQ(frame.planes.size(), 1U);
    EXPECT_EQ(frame.planes.front(), std::vector<std::byte>(payload.begin(), payload.end()));
}

TEST(FfmpegAudioDecoderBackendTest, ResetMakesTheCodecReusableAfterDrain) {
    FfmpegAudioDecoderBackend backend;
    const AudioCodecConfig config{
        .common = CodecCommon{.codec_name = "pcm_u8", .extradata = {}},
        .sample_rate = 8'000,
        .channels = 1,
    };
    ASSERT_TRUE(backend.configure(config).has_value());

    ASSERT_TRUE(backend.drain().has_value());
    const auto rejected = backend.decode(EncodedPacket{
        .payload = {std::byte{0x80}},
        .pts_us = std::nullopt,
        .dts_us = std::nullopt,
        .duration_us = std::nullopt,
    });
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().operation, AudioDecoderBackendOperation::Decode);

    backend.reset();
    const auto decoded = backend.decode(EncodedPacket{
        .payload = {std::byte{0x80}},
        .pts_us = std::nullopt,
        .dts_us = std::nullopt,
        .duration_us = std::nullopt,
    });
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
    ASSERT_EQ(decoded->size(), 1U);
}

TEST(FfmpegAudioDecoderBackendTest, RejectsUnknownCodecWithoutKeepingResources) {
    FfmpegAudioDecoderBackend backend;
    const auto configured = backend.configure(AudioCodecConfig{
        .common = CodecCommon{.codec_name = "not-a-real-audio-codec", .extradata = {}},
        .sample_rate = 8'000,
        .channels = 1,
    });

    ASSERT_FALSE(configured.has_value());
    EXPECT_EQ(configured.error().operation, AudioDecoderBackendOperation::Configure);
    EXPECT_NE(configured.error().message, "");
    EXPECT_FALSE(backend.decode({}).has_value());
}

} // namespace
} // namespace semi::infra::ffmpeg::audio_decoder
