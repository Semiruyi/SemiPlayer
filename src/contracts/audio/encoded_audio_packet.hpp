#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace semi::contracts::audio {

// Backend-owned encoded audio packet. The byte view remains valid for the
// lifetime of the implementing object and must not be mutated by callers.
class EncodedAudioPacket {
public:
    virtual ~EncodedAudioPacket() = default;

    EncodedAudioPacket(const EncodedAudioPacket&) = delete;
    EncodedAudioPacket& operator=(const EncodedAudioPacket&) = delete;
    EncodedAudioPacket(EncodedAudioPacket&&) = delete;
    EncodedAudioPacket& operator=(EncodedAudioPacket&&) = delete;

    [[nodiscard]] virtual std::span<const std::byte> payload() const noexcept = 0;
    [[nodiscard]] virtual std::int64_t pts_us() const noexcept = 0;
    [[nodiscard]] virtual std::optional<std::int64_t> duration_us() const noexcept = 0;

protected:
    EncodedAudioPacket() = default;
};

} // namespace semi::contracts::audio
