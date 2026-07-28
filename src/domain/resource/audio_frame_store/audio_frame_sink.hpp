#pragma once

#include "domain/resource/audio_frame_store/audio_frame.hpp"

#include <cstdint>

namespace semi::domain {

enum class AudioFramePushResult : std::uint8_t {
    Accepted,
    Full,
};

// Producer-side port for delivering decoded audio frames. The sink must not
// consume the frame when it reports Full.
class AudioFrameSink {
public:
    virtual ~AudioFrameSink() = default;

    AudioFrameSink(const AudioFrameSink&) = delete;
    AudioFrameSink& operator=(const AudioFrameSink&) = delete;
    AudioFrameSink(AudioFrameSink&&) = delete;
    AudioFrameSink& operator=(AudioFrameSink&&) = delete;

    [[nodiscard]] virtual AudioFramePushResult try_push(AudioFrame&& frame) = 0;

protected:
    AudioFrameSink() = default;
};

} // namespace semi::domain
