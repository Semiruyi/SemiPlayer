#include "infrastructure/ffmpeg/demuxer/ffmpeg_demuxer_backend.hpp"
#include "infrastructure/ffmpeg/video_decoder/ffmpeg_video_decoder_backend.hpp"
#include "infrastructure/ffmpeg/video_decoder/ffmpeg_video_frame_buffer.hpp"

#include <gtest/gtest.h>

extern "C" {
#include <libavutil/frame.h>
}

#include <cstddef>
#include <memory>
#include <optional>
#include <variant>

namespace semi::infra::ffmpeg::video_decoder {
namespace {

using contracts::demuxer::packet::BackendEndOfStream;
using contracts::demuxer::packet::BackendPacket;
using contracts::demuxer::packet::EncodedPacket;
using contracts::media::VideoCodecConfig;
using contracts::media::VideoPixelFormat;
using contracts::video_decoder::VideoDecoderBackendOperation;

TEST(FfmpegVideoFrameBufferTest, ExposesNativePlanesAndOwnsTheAvFrame) {
    AvFramePtr frame(av_frame_alloc());
    ASSERT_NE(frame, nullptr);
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = 320;
    frame->height = 180;
    ASSERT_EQ(av_frame_get_buffer(frame.get(), 32), 0);
    ASSERT_EQ(av_frame_make_writable(frame.get()), 0);

    auto buffer = std::make_unique<FfmpegVideoFrameBuffer>(std::move(frame));

    EXPECT_EQ(buffer->pixel_format(), VideoPixelFormat::Yuv420p);
    EXPECT_EQ(buffer->width(), 320U);
    EXPECT_EQ(buffer->height(), 180U);
    ASSERT_EQ(buffer->plane_count(), 3U);
    for (std::size_t index = 0; index < buffer->plane_count(); ++index) {
        const auto plane = buffer->plane(index);
        EXPECT_NE(plane.data, nullptr);
        EXPECT_GT(plane.stride_bytes, 0U);
        EXPECT_EQ(plane.size_bytes, static_cast<std::size_t>(plane.stride_bytes) *
                                       (index == 0 ? 180U : 90U));
    }
    EXPECT_EQ(buffer->plane(3).size_bytes, 0U);
}

TEST(FfmpegVideoDecoderBackendTest, RejectsDecodeBeforeConfiguration) {
    FfmpegVideoDecoderBackend backend;

    const auto result = backend.decode({});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().operation, VideoDecoderBackendOperation::Decode);
}

TEST(FfmpegVideoDecoderBackendTest, DecodesSampleVideoAndKeepsFrameAfterUnconfigure) {
    demuxer::FfmpegDemuxerBackend demuxer;
    const auto opened = demuxer.open(SEMI_PLAYER_TEST_MEDIA_PATH);
    ASSERT_TRUE(opened.has_value());

    const contracts::media::StreamDescriptor* video_stream = nullptr;
    const VideoCodecConfig* video_config = nullptr;
    for (const auto& stream : opened->streams) {
        if (const auto* config = std::get_if<VideoCodecConfig>(&stream.config)) {
            video_stream = &stream;
            video_config = config;
            break;
        }
    }
    ASSERT_NE(video_stream, nullptr);
    ASSERT_NE(video_config, nullptr);

    FfmpegVideoDecoderBackend backend;
    const auto configured = backend.configure(*video_config);
    ASSERT_TRUE(configured.has_value()) << configured.error().message;

    std::unique_ptr<const contracts::media::VideoFrameBuffer> retained_buffer;
    std::optional<std::int64_t> retained_pts;
    for (int attempt = 0; attempt < 64 && !retained_buffer; ++attempt) {
        const auto read = demuxer.read_packet();
        ASSERT_TRUE(read.has_value());
        if (std::holds_alternative<BackendEndOfStream>(*read)) {
            break;
        }

        const auto& packet = std::get<BackendPacket>(*read);
        if (packet.stream_id.value != video_stream->id.value) {
            continue;
        }

        auto decoded = backend.decode(packet.packet);
        ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
        if (!decoded->empty()) {
            retained_buffer = std::move(decoded->front().buffer);
            retained_pts = decoded->front().pts_us;
        }
    }

    ASSERT_NE(retained_buffer, nullptr);
    EXPECT_EQ(retained_buffer->pixel_format(), VideoPixelFormat::Yuv420p);
    EXPECT_EQ(retained_buffer->width(), 320U);
    EXPECT_EQ(retained_buffer->height(), 180U);
    ASSERT_EQ(retained_buffer->plane_count(), 3U);
    EXPECT_NE(retained_buffer->plane(0).data, nullptr);
    EXPECT_GT(retained_buffer->plane(0).size_bytes, 0U);
    EXPECT_TRUE(retained_pts.has_value());

    const auto first_byte = retained_buffer->plane(0).bytes().front();
    backend.unconfigure();

    EXPECT_EQ(retained_buffer->plane(0).bytes().front(), first_byte);
}

TEST(FfmpegVideoDecoderBackendTest, ResetMakesTheCodecReusableAfterDrain) {
    demuxer::FfmpegDemuxerBackend demuxer;
    const auto opened = demuxer.open(SEMI_PLAYER_TEST_MEDIA_PATH);
    ASSERT_TRUE(opened.has_value());

    const contracts::media::StreamDescriptor* video_stream = nullptr;
    const VideoCodecConfig* video_config = nullptr;
    for (const auto& stream : opened->streams) {
        if (const auto* config = std::get_if<VideoCodecConfig>(&stream.config)) {
            video_stream = &stream;
            video_config = config;
            break;
        }
    }
    ASSERT_NE(video_stream, nullptr);
    ASSERT_NE(video_config, nullptr);

    std::optional<EncodedPacket> video_packet;
    for (;;) {
        const auto read = demuxer.read_packet();
        ASSERT_TRUE(read.has_value());
        if (std::holds_alternative<BackendEndOfStream>(*read)) {
            break;
        }
        const auto& packet = std::get<BackendPacket>(*read);
        if (packet.stream_id.value == video_stream->id.value) {
            video_packet = packet.packet;
            break;
        }
    }
    ASSERT_TRUE(video_packet.has_value());

    FfmpegVideoDecoderBackend backend;
    ASSERT_TRUE(backend.configure(*video_config).has_value());

    ASSERT_TRUE(backend.drain().has_value());
    const auto rejected = backend.decode(*video_packet);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().operation, VideoDecoderBackendOperation::Decode);

    backend.reset();
    const auto decoded = backend.decode(*video_packet);
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
}

} // namespace
} // namespace semi::infra::ffmpeg::video_decoder
