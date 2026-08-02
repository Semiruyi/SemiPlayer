#pragma once

#include "contracts/media/media_types.hpp"

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace semi::contracts::audio_resampler {

// The operation that failed in a resampler implementation. Native error codes
// are backend-specific (for example, an FFmpeg error code).
enum class AudioResamplerBackendOperation : std::uint8_t {
    Configure,
    Resample,
    Drain,
};

struct AudioResamplerBackendError {
    AudioResamplerBackendOperation operation = AudioResamplerBackendOperation::Configure;
    int native_code = 0;
    std::string message;
};

using ResampledAudioBatch = std::vector<media::DecodedAudio>;

// Backend boundary for PCM format conversion. Implementations own all native
// resampler state; returned PCM owns its storage and has no native lifetime
// dependency. Calls are made exclusively by the domain worker.
class AudioResamplerBackend {
public:
    virtual ~AudioResamplerBackend() = default;

    AudioResamplerBackend(const AudioResamplerBackend&) = delete;
    AudioResamplerBackend& operator=(const AudioResamplerBackend&) = delete;
    AudioResamplerBackend(AudioResamplerBackend&&) = delete;
    AudioResamplerBackend& operator=(AudioResamplerBackend&&) = delete;

    [[nodiscard]] virtual std::expected<void, AudioResamplerBackendError>
    configure(const media::AudioPcmFormat& input_format,
              const media::AudioPcmFormat& output_format) = 0;

    // One input PCM frame may produce zero, one, or many output PCM frames.
    [[nodiscard]] virtual std::expected<ResampledAudioBatch, AudioResamplerBackendError>
    resample(const media::DecodedAudio& input) = 0;

    // Drains delayed resampler samples after an end-of-input marker.
    [[nodiscard]] virtual std::expected<ResampledAudioBatch, AudioResamplerBackendError>
    drain() = 0;

    // Clears delayed resampler data after the worker has observed a new generation.
    virtual void reset() noexcept = 0;
    virtual void unconfigure() noexcept = 0;

protected:
    AudioResamplerBackend() = default;
};

} // namespace semi::contracts::audio_resampler
