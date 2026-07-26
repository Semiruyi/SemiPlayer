#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace semi::contracts::audio {

// Backend-owned encoded audio packet. The byte view remains valid for the
// lifetime of the implementing object and must not be mutated by callers.
//
// Timestamp contract:
// - pts_us() is the presentation timestamp on the media timeline, in
//   microseconds. It may be absent when the backend has no valid PTS.
// - dts_us() is the decode timestamp on the media timeline, in microseconds.
//   It may be absent when the backend has no valid DTS.
// - Timestamps may be negative and must not be clamped by implementations.
// - PTS and DTS are independent values; callers must not assume pts >= dts.
// The backend is responsible for converting its native timestamp/time base to
// this contract and mapping an invalid native timestamp to std::nullopt.
class EncodedAudioPacket {
public:
    virtual ~EncodedAudioPacket() = default;

    EncodedAudioPacket(const EncodedAudioPacket&) = delete;
    EncodedAudioPacket& operator=(const EncodedAudioPacket&) = delete;
    EncodedAudioPacket(EncodedAudioPacket&&) = delete;
    EncodedAudioPacket& operator=(EncodedAudioPacket&&) = delete;

    [[nodiscard]] virtual std::span<const std::byte> payload() const noexcept = 0;
    // FFmpeg packets may not carry a presentation timestamp.
    [[nodiscard]] virtual std::optional<std::int64_t> pts_us() const noexcept = 0;
    [[nodiscard]] virtual std::optional<std::int64_t> dts_us() const noexcept = 0;
    [[nodiscard]] virtual std::optional<std::int64_t> duration_us() const noexcept = 0;

protected:
    EncodedAudioPacket() = default;
};

} // namespace semi::contracts::audio
