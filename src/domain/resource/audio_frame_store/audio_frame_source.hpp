#pragma once

#include "domain/resource/audio_frame_store/audio_frame_store_item.hpp"

#include <optional>

namespace semi::domain {

// Consumer-side port for receiving ordered decoded PCM and end-of-input
// markers. The source never blocks; an empty source reports no item available.
class AudioFrameSource {
public:
    virtual ~AudioFrameSource() = default;

    AudioFrameSource(const AudioFrameSource&) = delete;
    AudioFrameSource& operator=(const AudioFrameSource&) = delete;
    AudioFrameSource(AudioFrameSource&&) = delete;
    AudioFrameSource& operator=(AudioFrameSource&&) = delete;

    [[nodiscard]] virtual std::optional<AudioFrameStoreItem> try_pop() = 0;

protected:
    AudioFrameSource() = default;
};

} // namespace semi::domain
