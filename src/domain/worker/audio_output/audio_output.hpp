#pragma once

#include "contracts/audio_output/audio_output_backend.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>

namespace semi::domain {

using contracts::audio_output::AudioOutputBackend;
using contracts::audio_output::AudioOutputBackendError;
using contracts::audio_output::AudioOutputBackendOperation;
using contracts::audio_output::AudioOutputConfigureResult;
using contracts::audio_output::AudioOutputDrainStatus;
using contracts::audio_output::AudioOutputOptions;
using contracts::audio_output::AudioOutputSubmitStatus;

enum class AudioOutputErrorCode : std::uint8_t {
    InvalidState,
    BackendFailure,
};

struct AudioOutputError {
    AudioOutputErrorCode code = AudioOutputErrorCode::BackendFailure;
    std::string message;
    std::optional<AudioOutputBackendError> backend_error;
};

// Control-plane interface for the audio output worker. The worker belongs to
// the module lifecycle (constructed with the module, joined on destruction), so
// there is no start()/stop(). Seek is represented by shared Generation.
class AudioOutput {
public:
    virtual ~AudioOutput() = default;

    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;
    AudioOutput(AudioOutput&&) = delete;
    AudioOutput& operator=(AudioOutput&&) = delete;

    [[nodiscard]] virtual std::expected<AudioOutputConfigureResult, AudioOutputError>
    configure(const AudioOutputOptions& options) = 0;

    virtual void unconfigure() noexcept = 0;

protected:
    AudioOutput() = default;
};

} // namespace semi::domain
