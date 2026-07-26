#include "infrastructure/ffmpeg/demuxer/packet/ffmpeg_encoded_packet.hpp"

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
}

#include <array>
#include <new>
#include <utility>

namespace semi::infra::ffmpeg::demuxer::packet {
namespace {

using ErrorCode = FfmpegEncodedPacketErrorCode;

bool valid_time_base(AVRational time_base) noexcept {
    return time_base.num > 0 && time_base.den > 0;
}

std::string ffmpeg_message(int error_code) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    if (av_strerror(error_code, buffer.data(), buffer.size()) < 0) {
        return "FFmpeg error " + std::to_string(error_code);
    }
    return buffer.data();
}

FfmpegEncodedPacketError make_error(ErrorCode code,
                                    int native_code,
                                    std::string message) {
    return FfmpegEncodedPacketError{
        .code = code,
        .native_code = native_code,
        .message = std::move(message),
    };
}

std::optional<std::int64_t> rescale_timestamp(std::int64_t timestamp,
                                              AVRational time_base) noexcept {
    if (timestamp == AV_NOPTS_VALUE) {
        return std::nullopt;
    }
    return av_rescale_q(timestamp, time_base, AV_TIME_BASE_Q);
}

std::optional<std::int64_t> rescale_duration(std::int64_t duration,
                                             AVRational time_base) noexcept {
    // FFmpeg uses zero/AV_NOPTS_VALUE for an unavailable packet duration.
    if (duration <= 0 || duration == AV_NOPTS_VALUE) {
        return std::nullopt;
    }
    return av_rescale_q(duration, time_base, AV_TIME_BASE_Q);
}

} // namespace

void FfmpegEncodedPacket::PacketDeleter::operator()(AVPacket* packet) const noexcept {
    av_packet_free(&packet);
}

FfmpegEncodedPacket::FfmpegEncodedPacket(
    PacketPtr packet,
    std::optional<std::int64_t> pts_us,
    std::optional<std::int64_t> dts_us,
    std::optional<std::int64_t> duration_us) noexcept
    : packet_(std::move(packet)),
      pts_us_(pts_us),
      dts_us_(dts_us),
      duration_us_(duration_us) {}

FfmpegEncodedPacket::~FfmpegEncodedPacket() = default;

std::expected<std::unique_ptr<FfmpegEncodedPacket>, FfmpegEncodedPacketError>
FfmpegEncodedPacket::create(const AVPacket& packet, AVRational time_base) {
    if (!valid_time_base(time_base)) {
        return std::unexpected(make_error(
            ErrorCode::InvalidTimeBase,
            0,
            "FFmpeg packet time base must have positive numerator and denominator"));
    }

    try {
        PacketPtr owned_packet(av_packet_alloc());
        if (!owned_packet) {
            return std::unexpected(make_error(
                ErrorCode::PacketAllocationFailed,
                AVERROR(ENOMEM),
                "failed to allocate FFmpeg packet"));
        }

        const int status = av_packet_ref(owned_packet.get(), &packet);
        if (status < 0) {
            return std::unexpected(make_error(
                ErrorCode::PacketReferenceFailed,
                status,
                ffmpeg_message(status)));
        }

        return std::unique_ptr<FfmpegEncodedPacket>(new FfmpegEncodedPacket(
            std::move(owned_packet),
            rescale_timestamp(packet.pts, time_base),
            rescale_timestamp(packet.dts, time_base),
            rescale_duration(packet.duration, time_base)));
    } catch (const std::bad_alloc&) {
        return std::unexpected(make_error(
            ErrorCode::PacketAllocationFailed,
            AVERROR(ENOMEM),
            "failed to allocate FFmpeg packet"));
    }
}

std::span<const std::byte> FfmpegEncodedPacket::payload() const noexcept {
    if (!packet_ || packet_->data == nullptr || packet_->size <= 0) {
        return {};
    }
    return {reinterpret_cast<const std::byte*>(packet_->data),
            static_cast<std::size_t>(packet_->size)};
}

std::optional<std::int64_t> FfmpegEncodedPacket::pts_us() const noexcept {
    return pts_us_;
}

std::optional<std::int64_t> FfmpegEncodedPacket::dts_us() const noexcept {
    return dts_us_;
}

std::optional<std::int64_t> FfmpegEncodedPacket::duration_us() const noexcept {
    return duration_us_;
}

} // namespace semi::infra::ffmpeg::demuxer::packet
