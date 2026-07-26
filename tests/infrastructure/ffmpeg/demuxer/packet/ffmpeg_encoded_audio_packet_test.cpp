extern "C" {
#include <libavcodec/packet.h>
#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>
}

#include "infrastructure/ffmpeg/demuxer/packet/ffmpeg_encoded_audio_packet.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace semi::infra::ffmpeg::demuxer::packet {
namespace {

class ScopedPacket {
public:
    ScopedPacket() : packet_(av_packet_alloc()) {}

    ~ScopedPacket() {
        av_packet_free(&packet_);
    }

    ScopedPacket(const ScopedPacket&) = delete;
    ScopedPacket& operator=(const ScopedPacket&) = delete;

    [[nodiscard]] AVPacket* get() noexcept {
        return packet_;
    }

private:
    AVPacket* packet_;
};

TEST(FfmpegEncodedAudioPacketTest, OwnsPacketReferenceAndRescalesTimestamps) {
    ScopedPacket source;
    ASSERT_NE(source.get(), nullptr);
    ASSERT_GE(av_new_packet(source.get(), 3), 0);

    source.get()->data[0] = 0x11;
    source.get()->data[1] = 0x22;
    source.get()->data[2] = 0x33;
    source.get()->pts = 48'000;
    source.get()->dts = 47'000;
    source.get()->duration = 1'024;

    constexpr AVRational time_base{1, 48'000};
    const auto created = FfmpegEncodedAudioPacket::create(*source.get(), time_base);

    ASSERT_TRUE(created.has_value()) << created.error().message;
    av_packet_unref(source.get());

    const auto& packet = **created;
    const auto payload = packet.payload();
    ASSERT_EQ(payload.size(), 3U);
    EXPECT_EQ(payload[0], std::byte{0x11});
    EXPECT_EQ(payload[1], std::byte{0x22});
    EXPECT_EQ(payload[2], std::byte{0x33});
    ASSERT_TRUE(packet.pts_us().has_value());
    EXPECT_EQ(*packet.pts_us(), av_rescale_q(48'000, time_base, AV_TIME_BASE_Q));
    ASSERT_TRUE(packet.dts_us().has_value());
    EXPECT_EQ(*packet.dts_us(), av_rescale_q(47'000, time_base, AV_TIME_BASE_Q));
    ASSERT_TRUE(packet.duration_us().has_value());
    EXPECT_EQ(*packet.duration_us(), av_rescale_q(1'024, time_base, AV_TIME_BASE_Q));
}

TEST(FfmpegEncodedAudioPacketTest, MapsMissingTimestampsToNullopt) {
    ScopedPacket source;
    ASSERT_NE(source.get(), nullptr);
    ASSERT_GE(av_new_packet(source.get(), 1), 0);
    source.get()->pts = AV_NOPTS_VALUE;
    source.get()->dts = AV_NOPTS_VALUE;
    source.get()->duration = 0;

    const auto created = FfmpegEncodedAudioPacket::create(*source.get(), AVRational{1, 48'000});

    ASSERT_TRUE(created.has_value()) << created.error().message;
    EXPECT_FALSE((**created).pts_us().has_value());
    EXPECT_FALSE((**created).dts_us().has_value());
    EXPECT_FALSE((**created).duration_us().has_value());
}

TEST(FfmpegEncodedAudioPacketTest, RejectsInvalidTimeBase) {
    ScopedPacket source;
    ASSERT_NE(source.get(), nullptr);

    const auto created = FfmpegEncodedAudioPacket::create(*source.get(), AVRational{0, 48'000});

    ASSERT_FALSE(created.has_value());
    EXPECT_EQ(created.error().code, FfmpegEncodedAudioPacketErrorCode::InvalidTimeBase);
}

} // namespace
} // namespace semi::infra::ffmpeg::demuxer::packet
