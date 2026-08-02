#include "infrastructure/ffmpeg/audio_resampler/ffmpeg_audio_resampler_backend.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace semi::infra::ffmpeg::audio_resampler {
namespace {

using contracts::audio_resampler::AudioResamplerBackendOperation;
using contracts::media::AudioPcmFormat;
using contracts::media::AudioSampleFormat;
using contracts::media::DecodedAudio;

AudioPcmFormat s16_packed(std::uint32_t sample_rate, std::uint32_t channels) {
    return AudioPcmFormat{
        .sample_rate = sample_rate,
        .channels = channels,
        .sample_format = AudioSampleFormat::S16,
        .planar = false,
    };
}

AudioPcmFormat s16_planar(std::uint32_t sample_rate, std::uint32_t channels) {
    return AudioPcmFormat{
        .sample_rate = sample_rate,
        .channels = channels,
        .sample_format = AudioSampleFormat::S16,
        .planar = true,
    };
}

AudioPcmFormat f32_packed(std::uint32_t sample_rate, std::uint32_t channels) {
    return AudioPcmFormat{
        .sample_rate = sample_rate,
        .channels = channels,
        .sample_format = AudioSampleFormat::F32,
        .planar = false,
    };
}

std::vector<std::byte> bytes_from_i16(std::initializer_list<std::int16_t> samples) {
    std::vector<std::byte> bytes(samples.size() * sizeof(std::int16_t));
    std::size_t offset = 0;
    for (const auto sample : samples) {
        std::memcpy(bytes.data() + offset, &sample, sizeof(sample));
        offset += sizeof(sample);
    }
    return bytes;
}

std::vector<std::byte> zero_i16_bytes(std::size_t samples) {
    return std::vector<std::byte>(samples * sizeof(std::int16_t));
}

std::vector<float> floats_from_bytes(const std::vector<std::byte>& bytes) {
    std::vector<float> values(bytes.size() / sizeof(float));
    if (!values.empty()) {
        std::memcpy(values.data(), bytes.data(), bytes.size());
    }
    return values;
}

DecodedAudio make_s16_packed_audio(std::uint32_t sample_rate,
                                   std::uint32_t channels,
                                   std::uint32_t samples_per_channel,
                                   std::vector<std::byte> payload,
                                   std::optional<std::int64_t> pts_us = std::nullopt) {
    return DecodedAudio{
        .format = s16_packed(sample_rate, channels),
        .samples_per_channel = samples_per_channel,
        .planes = {std::move(payload)},
        .pts_us = pts_us,
    };
}

DecodedAudio make_s16_planar_audio(std::uint32_t sample_rate,
                                   std::uint32_t channels,
                                   std::uint32_t samples_per_channel,
                                   std::vector<std::vector<std::byte>> planes) {
    return DecodedAudio{
        .format = s16_planar(sample_rate, channels),
        .samples_per_channel = samples_per_channel,
        .planes = std::move(planes),
        .pts_us = 42,
    };
}

TEST(FfmpegAudioResamplerBackendTest, RejectsResampleBeforeConfiguration) {
    FfmpegAudioResamplerBackend backend;

    const auto result = backend.resample({});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().operation, AudioResamplerBackendOperation::Resample);
}

TEST(FfmpegAudioResamplerBackendTest, ConvertsPackedS16ToPackedF32AndPreservesTimestamp) {
    FfmpegAudioResamplerBackend backend;
    ASSERT_TRUE(backend.configure(s16_packed(48'000, 1), f32_packed(48'000, 1)).has_value());

    const auto output = backend.resample(make_s16_packed_audio(
        48'000,
        1,
        4,
        bytes_from_i16({0, 16'384, -16'384, 32'767}),
        123'456));

    ASSERT_TRUE(output.has_value()) << output.error().message;
    ASSERT_EQ(output->size(), 1U);
    const auto& frame = output->front();
    EXPECT_EQ(frame.format.sample_rate, 48'000U);
    EXPECT_EQ(frame.format.channels, 1U);
    EXPECT_EQ(frame.format.sample_format, AudioSampleFormat::F32);
    EXPECT_FALSE(frame.format.planar);
    EXPECT_EQ(frame.samples_per_channel, 4U);
    ASSERT_EQ(frame.planes.size(), 1U);
    ASSERT_TRUE(frame.pts_us.has_value());
    EXPECT_EQ(*frame.pts_us, 123'456);

    const auto values = floats_from_bytes(frame.planes.front());
    ASSERT_EQ(values.size(), 4U);
    EXPECT_NEAR(values[0], 0.0F, 0.0001F);
    EXPECT_NEAR(values[1], 0.5F, 0.0001F);
    EXPECT_NEAR(values[2], -0.5F, 0.0001F);
    EXPECT_NEAR(values[3], 1.0F, 0.0001F);
}

TEST(FfmpegAudioResamplerBackendTest, ConvertsPlanarStereoToPackedStereo) {
    FfmpegAudioResamplerBackend backend;
    ASSERT_TRUE(backend.configure(s16_planar(48'000, 2), f32_packed(48'000, 2)).has_value());

    const auto output = backend.resample(make_s16_planar_audio(
        48'000,
        2,
        2,
        {bytes_from_i16({0, 32'767}), bytes_from_i16({16'384, -16'384})}));

    ASSERT_TRUE(output.has_value()) << output.error().message;
    ASSERT_EQ(output->size(), 1U);
    const auto& frame = output->front();
    EXPECT_EQ(frame.format.channels, 2U);
    EXPECT_FALSE(frame.format.planar);
    EXPECT_EQ(frame.samples_per_channel, 2U);
    ASSERT_EQ(frame.planes.size(), 1U);

    const auto values = floats_from_bytes(frame.planes.front());
    ASSERT_EQ(values.size(), 4U);
    EXPECT_NEAR(values[0], 0.0F, 0.0001F);
    EXPECT_NEAR(values[1], 0.5F, 0.0001F);
    EXPECT_NEAR(values[2], 1.0F, 0.0001F);
    EXPECT_NEAR(values[3], -0.5F, 0.0001F);
}

TEST(FfmpegAudioResamplerBackendTest, ResamplesSampleRateAndDrainsDelayedSamples) {
    FfmpegAudioResamplerBackend backend;
    ASSERT_TRUE(backend.configure(s16_packed(44'100, 1), f32_packed(48'000, 1)).has_value());

    const auto output = backend.resample(make_s16_packed_audio(
        44'100,
        1,
        100,
        zero_i16_bytes(100)));
    ASSERT_TRUE(output.has_value()) << output.error().message;

    const auto drained = backend.drain();
    ASSERT_TRUE(drained.has_value()) << drained.error().message;

    std::uint32_t total_samples = 0;
    for (const auto& frame : *output) {
        total_samples += frame.samples_per_channel;
    }
    for (const auto& frame : *drained) {
        total_samples += frame.samples_per_channel;
    }
    EXPECT_GT(total_samples, 100U);
}

TEST(FfmpegAudioResamplerBackendTest, ResetMakesTheResamplerReusableAfterDrain) {
    FfmpegAudioResamplerBackend backend;
    ASSERT_TRUE(backend.configure(s16_packed(48'000, 1), f32_packed(48'000, 1)).has_value());

    ASSERT_TRUE(backend.drain().has_value());
    const auto rejected = backend.resample(make_s16_packed_audio(
        48'000,
        1,
        1,
        bytes_from_i16({0})));
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().operation, AudioResamplerBackendOperation::Resample);

    backend.reset();
    const auto output = backend.resample(make_s16_packed_audio(
        48'000,
        1,
        1,
        bytes_from_i16({0})));
    ASSERT_TRUE(output.has_value()) << output.error().message;
    ASSERT_EQ(output->size(), 1U);
}

TEST(FfmpegAudioResamplerBackendTest, RejectsInputFormatMismatch) {
    FfmpegAudioResamplerBackend backend;
    ASSERT_TRUE(backend.configure(s16_packed(48'000, 1), f32_packed(48'000, 1)).has_value());

    const auto output = backend.resample(make_s16_packed_audio(
        44'100,
        1,
        1,
        bytes_from_i16({0})));

    ASSERT_FALSE(output.has_value());
    EXPECT_EQ(output.error().operation, AudioResamplerBackendOperation::Resample);
}

TEST(FfmpegAudioResamplerBackendTest, RejectsInvalidConfigurationWithoutKeepingResources) {
    FfmpegAudioResamplerBackend backend;

    const auto configured = backend.configure(AudioPcmFormat{}, f32_packed(48'000, 1));

    ASSERT_FALSE(configured.has_value());
    EXPECT_EQ(configured.error().operation, AudioResamplerBackendOperation::Configure);
    EXPECT_FALSE(backend.drain().has_value());
}

} // namespace
} // namespace semi::infra::ffmpeg::audio_resampler
