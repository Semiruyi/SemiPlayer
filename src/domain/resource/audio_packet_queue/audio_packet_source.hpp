#pragma once

#include "domain/resource/audio_packet_queue/audio_packet_queue_item.hpp"

#include <optional>

namespace semi::domain {

// Consumer-side port for receiving ordered encoded audio input. The source
// never blocks; an empty source reports no item available.
class AudioPacketSource {
public:
    virtual ~AudioPacketSource() = default;

    AudioPacketSource(const AudioPacketSource&) = delete;
    AudioPacketSource& operator=(const AudioPacketSource&) = delete;
    AudioPacketSource(AudioPacketSource&&) = delete;
    AudioPacketSource& operator=(AudioPacketSource&&) = delete;

    [[nodiscard]] virtual std::optional<AudioPacketQueueItem> try_pop() = 0;

protected:
    AudioPacketSource() = default;
};

} // namespace semi::domain
