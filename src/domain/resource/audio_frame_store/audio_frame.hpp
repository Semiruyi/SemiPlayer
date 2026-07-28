#pragma once

#include "contracts/media/media_types.hpp"
#include "domain/resource/generation/generation.hpp"

#include <utility>

namespace semi::domain {

// One decoded audio frame in the playback pipeline. The media contract owns
// the PCM representation; generation is the domain-side invalidation marker.
class AudioFrame final {
public:
    AudioFrame(contracts::media::DecodedAudio decoded_audio,
               Generation::Value generation) noexcept
        : decoded_audio_(std::move(decoded_audio)), generation_(generation) {}

    ~AudioFrame() = default;

    AudioFrame(const AudioFrame&) = delete;
    AudioFrame& operator=(const AudioFrame&) = delete;
    AudioFrame(AudioFrame&&) noexcept = default;
    AudioFrame& operator=(AudioFrame&&) noexcept = default;

    [[nodiscard]] Generation::Value generation() const noexcept {
        return generation_;
    }

    [[nodiscard]] const contracts::media::DecodedAudio& decoded() const noexcept {
        return decoded_audio_;
    }

private:
    contracts::media::DecodedAudio decoded_audio_;
    Generation::Value generation_;
};

[[nodiscard]] inline bool is_current_audio_frame(
    const AudioFrame& frame, Generation::Value current_generation) noexcept {
    return frame.generation() == current_generation;
}

} // namespace semi::domain
