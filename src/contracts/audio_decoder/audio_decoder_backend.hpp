#pragma once

#include "contracts/demuxer/packet/encoded_packet.hpp"
#include "contracts/media/media_types.hpp"

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace semi::contracts::audio_decoder {

// The operation that failed in a decoder implementation. Native error codes
// are backend-specific (for example, an FFmpeg error code).
enum class AudioDecoderBackendOperation : std::uint8_t {
    Configure,
    Decode,
    Drain,
};

struct AudioDecoderBackendError {
    AudioDecoderBackendOperation operation = AudioDecoderBackendOperation::Configure;
    int native_code = 0;
    std::string message;
};

using DecodedAudioBatch = std::vector<media::DecodedAudio>;

// Backend boundary for compressed audio decoding. Implementations own all
// native codec state; returned PCM owns its storage and has no native lifetime
// dependency. Calls are made exclusively by the domain worker.
class AudioDecoderBackend {
public:
    virtual ~AudioDecoderBackend() = default;

    AudioDecoderBackend(const AudioDecoderBackend&) = delete;
    AudioDecoderBackend& operator=(const AudioDecoderBackend&) = delete;
    AudioDecoderBackend(AudioDecoderBackend&&) = delete;
    AudioDecoderBackend& operator=(AudioDecoderBackend&&) = delete;

    [[nodiscard]] virtual std::expected<void, AudioDecoderBackendError>
    configure(const media::AudioCodecConfig& config) = 0;

    // A packet may produce zero, one, or many PCM frames.
    [[nodiscard]] virtual std::expected<DecodedAudioBatch, AudioDecoderBackendError>
    decode(const demuxer::packet::EncodedPacket& packet) = 0;

    // Drains delayed codec frames after an end-of-input marker.
    [[nodiscard]] virtual std::expected<DecodedAudioBatch, AudioDecoderBackendError> drain() = 0;

    // Clears delayed codec data after the worker has observed a new generation.
    virtual void reset() noexcept = 0;
    virtual void unconfigure() noexcept = 0;

protected:
    AudioDecoderBackend() = default;
};

} // namespace semi::contracts::audio_decoder
