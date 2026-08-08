#pragma once

#include "contracts/audio_output/audio_output_realtime_events.hpp"
#include "contracts/media/media_types.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>

namespace semi::contracts::audio_output {

struct AudioOutputOptions {
    std::optional<std::string> device_id;
};

struct AudioOutputConfigureResult {
    media::AudioPcmFormat playback_format;
};

// The operation that failed in an output implementation. Native error codes
// are backend-specific (for example, an OS audio API error code).
enum class AudioOutputBackendOperation : std::uint8_t {
    Configure,
    Pause,
    Resume,
    Reset,
    Submit,
    Drain,
};

struct AudioOutputBackendError {
    AudioOutputBackendOperation operation = AudioOutputBackendOperation::Configure;
    int native_code = 0;
    std::string message;
};

enum class AudioOutputSubmitStatus : std::uint8_t {
    Accepted,
    WouldBlock,
};

enum class AudioOutputDrainStatus : std::uint8_t {
    Drained,
    WouldBlock,
};

// Backend boundary for PCM output. Implementations own all native device state.
// Calls are made exclusively by the domain worker.
class AudioOutputBackend {
public:
    virtual ~AudioOutputBackend() = default;

    AudioOutputBackend(const AudioOutputBackend&) = delete;
    AudioOutputBackend& operator=(const AudioOutputBackend&) = delete;
    AudioOutputBackend(AudioOutputBackend&&) = delete;
    AudioOutputBackend& operator=(AudioOutputBackend&&) = delete;

    [[nodiscard]] virtual std::expected<AudioOutputConfigureResult, AudioOutputBackendError>
    configure(const AudioOutputOptions& options) = 0;

    [[nodiscard]] virtual std::expected<void, AudioOutputBackendError> pause() = 0;

    [[nodiscard]] virtual std::expected<void, AudioOutputBackendError> resume() = 0;

    [[nodiscard]] virtual std::expected<AudioOutputSubmitStatus, AudioOutputBackendError>
    try_submit(const media::DecodedAudio& audio) = 0;

    [[nodiscard]] virtual std::expected<AudioOutputDrainStatus, AudioOutputBackendError>
    try_drain() = 0;

    // Drops backend-buffered samples after the worker has observed a new generation.
    // If the backend has a real-time callback, this must not return until any
    // callback that can report consumption from the pre-reset buffer has stopped.
    [[nodiscard]] virtual std::expected<void, AudioOutputBackendError> reset() = 0;
    virtual void unconfigure() noexcept = 0;

protected:
    AudioOutputBackend() = default;
};

} // namespace semi::contracts::audio_output
