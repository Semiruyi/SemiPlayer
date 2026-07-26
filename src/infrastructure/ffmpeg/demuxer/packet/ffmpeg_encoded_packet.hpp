#pragma once

#include "contracts/demuxer/packet/encoded_packet.hpp"

extern "C" {
#include <libavcodec/packet.h>
#include <libavutil/rational.h>
}

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace semi::infra::ffmpeg::demuxer::packet {

enum class FfmpegEncodedPacketErrorCode : std::uint8_t {
    InvalidTimeBase,
    PacketAllocationFailed,
    PacketReferenceFailed,
};

struct FfmpegEncodedPacketError {
    FfmpegEncodedPacketErrorCode code =
        FfmpegEncodedPacketErrorCode::PacketReferenceFailed;
    int native_code = 0;
    std::string message;
};

// Owns an independent reference to an FFmpeg AVPacket and exposes it through
// the backend-neutral EncodedPacket contract. The source AVPacket may be
// unreferenced or reused after create() returns.
class FfmpegEncodedPacket final : public contracts::demuxer::packet::EncodedPacket {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<FfmpegEncodedPacket>,
                                        FfmpegEncodedPacketError>
    create(const AVPacket& packet, AVRational time_base);

    ~FfmpegEncodedPacket() override;

    FfmpegEncodedPacket(const FfmpegEncodedPacket&) = delete;
    FfmpegEncodedPacket& operator=(const FfmpegEncodedPacket&) = delete;
    FfmpegEncodedPacket(FfmpegEncodedPacket&&) = delete;
    FfmpegEncodedPacket& operator=(FfmpegEncodedPacket&&) = delete;

    [[nodiscard]] std::span<const std::byte> payload() const noexcept override;
    [[nodiscard]] std::optional<std::int64_t> pts_us() const noexcept override;
    [[nodiscard]] std::optional<std::int64_t> dts_us() const noexcept override;
    [[nodiscard]] std::optional<std::int64_t> duration_us() const noexcept override;

private:
    struct PacketDeleter {
        void operator()(AVPacket* packet) const noexcept;
    };

    using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;

    FfmpegEncodedPacket(
        PacketPtr packet,
        std::optional<std::int64_t> pts_us,
        std::optional<std::int64_t> dts_us,
        std::optional<std::int64_t> duration_us) noexcept;

    PacketPtr packet_;
    std::optional<std::int64_t> pts_us_;
    std::optional<std::int64_t> dts_us_;
    std::optional<std::int64_t> duration_us_;
};

} // namespace semi::infra::ffmpeg::demuxer::packet
