#pragma once

#include "contracts/audio_resampler/audio_resampler_backend.hpp"
#include "contracts/media/media_types.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>

namespace semi::domain {

using contracts::audio_resampler::AudioResamplerBackend;
using contracts::audio_resampler::AudioResamplerBackendError;
using contracts::audio_resampler::AudioResamplerBackendOperation;

enum class AudioResamplerErrorCode : std::uint8_t {
    InvalidState,
    BackendFailure,
};

struct AudioResamplerError {
    AudioResamplerErrorCode code = AudioResamplerErrorCode::BackendFailure;
    std::string message;
    std::optional<AudioResamplerBackendError> backend_error;
};

// Control-plane interface for the audio resampling worker. The worker belongs
// to the module lifecycle (constructed with the module, joined on destruction),
// so there is no start()/stop(). Playback pause is represented by downstream
// backpressure, so there is no pause() either.
class AudioResampler {
public:
    virtual ~AudioResampler() = default;

    AudioResampler(const AudioResampler&) = delete;
    AudioResampler& operator=(const AudioResampler&) = delete;
    AudioResampler(AudioResampler&&) = delete;
    AudioResampler& operator=(AudioResampler&&) = delete;

    [[nodiscard]] virtual std::expected<void, AudioResamplerError>
    configure(const contracts::media::AudioPcmFormat& input_format,
              const contracts::media::AudioPcmFormat& output_format) = 0;

    virtual void unconfigure() noexcept = 0;

protected:
    AudioResampler() = default;
};

} // namespace semi::domain
