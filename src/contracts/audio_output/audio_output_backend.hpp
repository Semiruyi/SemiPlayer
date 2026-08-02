#pragma once

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

class AudioOutputBackendProgressNotifier {
public:
    virtual ~AudioOutputBackendProgressNotifier() = default;

    AudioOutputBackendProgressNotifier(const AudioOutputBackendProgressNotifier&) = delete;
    AudioOutputBackendProgressNotifier& operator=(const AudioOutputBackendProgressNotifier&) = delete;
    AudioOutputBackendProgressNotifier(AudioOutputBackendProgressNotifier&&) = delete;
    AudioOutputBackendProgressNotifier& operator=(AudioOutputBackendProgressNotifier&&) = delete;

    virtual void notify_audio_output_progress_available() noexcept = 0;

protected:
    AudioOutputBackendProgressNotifier() = default;
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

    virtual void set_progress_notifier(AudioOutputBackendProgressNotifier* notifier) noexcept = 0;

    [[nodiscard]] virtual std::expected<AudioOutputConfigureResult, AudioOutputBackendError>
    configure(const AudioOutputOptions& options) = 0;

    [[nodiscard]] virtual std::expected<AudioOutputSubmitStatus, AudioOutputBackendError>
    try_submit(const media::DecodedAudio& audio) = 0;

    [[nodiscard]] virtual std::expected<AudioOutputDrainStatus, AudioOutputBackendError>
    try_drain() = 0;

    // Drops backend-buffered samples after the worker has observed a new generation.
    virtual void reset() noexcept = 0;
    virtual void unconfigure() noexcept = 0;

protected:
    AudioOutputBackend() = default;
};

} // namespace semi::contracts::audio_output
