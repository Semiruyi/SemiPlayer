#pragma once

#include "domain/resource/audio_frame_store/audio_frame.hpp"

#include <optional>

namespace semi::domain {

// Consumer-side port for receiving decoded audio frames. The source never
// blocks; an empty source reports that no frame is currently available.
class AudioFrameSource {
public:
    virtual ~AudioFrameSource() = default;

    AudioFrameSource(const AudioFrameSource&) = delete;
    AudioFrameSource& operator=(const AudioFrameSource&) = delete;
    AudioFrameSource(AudioFrameSource&&) = delete;
    AudioFrameSource& operator=(AudioFrameSource&&) = delete;

    [[nodiscard]] virtual std::optional<AudioFrame> try_pop() = 0;

protected:
    AudioFrameSource() = default;
};

} // namespace semi::domain
