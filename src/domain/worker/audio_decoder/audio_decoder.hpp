#pragma once

#include "contracts/audio_decoder/audio_decoder_backend.hpp"
#include "contracts/media/media_types.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>

namespace semi::domain {

using contracts::audio_decoder::AudioDecoderBackend;
using contracts::audio_decoder::AudioDecoderBackendError;
using contracts::audio_decoder::AudioDecoderBackendOperation;

enum class AudioDecoderErrorCode : std::uint8_t {
    InvalidState,
    BackendFailure,
};

struct AudioDecoderError {
    AudioDecoderErrorCode code = AudioDecoderErrorCode::BackendFailure;
    std::string message;
    std::optional<AudioDecoderBackendError> backend_error;
};

struct AudioDecoderConfigureResult {
    contracts::media::AudioPcmFormat decoded_format;
};

// Control-plane interface for the audio decoding worker. The worker belongs to
// the module lifecycle (constructed with the module, joined on destruction), so
// there is no start()/stop(). Playback pause is represented by downstream
// backpressure, so there is no pause() either.
class AudioDecoder {
public:
    virtual ~AudioDecoder() = default;

    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;
    AudioDecoder(AudioDecoder&&) = delete;
    AudioDecoder& operator=(AudioDecoder&&) = delete;

    [[nodiscard]] virtual std::expected<AudioDecoderConfigureResult, AudioDecoderError>
    configure(const contracts::media::AudioCodecConfig& config) = 0;

    virtual void unconfigure() noexcept = 0;

protected:
    AudioDecoder() = default;
};

} // namespace semi::domain
