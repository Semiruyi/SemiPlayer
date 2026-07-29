#pragma once

#include "domain/resource/audio_frame_store/audio_frame_store_item.hpp"

#include <cstdint>

namespace semi::domain {

enum class AudioFramePushResult : std::uint8_t {
    Accepted,
    Full,
};

// Producer-side port for delivering ordered PCM and end-of-input items. The
// sink must not consume the item when it reports Full.
class AudioFrameSink {
public:
    virtual ~AudioFrameSink() = default;

    AudioFrameSink(const AudioFrameSink&) = delete;
    AudioFrameSink& operator=(const AudioFrameSink&) = delete;
    AudioFrameSink(AudioFrameSink&&) = delete;
    AudioFrameSink& operator=(AudioFrameSink&&) = delete;

    [[nodiscard]] virtual AudioFramePushResult try_push(AudioFrameStoreItem&& item) = 0;

protected:
    AudioFrameSink() = default;
};

} // namespace semi::domain
