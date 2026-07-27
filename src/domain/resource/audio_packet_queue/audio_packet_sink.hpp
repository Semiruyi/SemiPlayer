#pragma once

#include "domain/resource/audio_packet_queue/audio_packet_queue_item.hpp"

#include <cstdint>

namespace semi::domain {

enum class AudioPacketPushResult : std::uint8_t {
    Accepted,
    Full,
};

// Producer-side port for delivering ordered audio input items. The sink must
// not consume the item when it reports Full.
class AudioPacketSink {
public:
    virtual ~AudioPacketSink() = default;

    AudioPacketSink(const AudioPacketSink&) = delete;
    AudioPacketSink& operator=(const AudioPacketSink&) = delete;
    AudioPacketSink(AudioPacketSink&&) = delete;
    AudioPacketSink& operator=(AudioPacketSink&&) = delete;

    [[nodiscard]] virtual AudioPacketPushResult try_push(AudioPacketQueueItem&& item) = 0;

protected:
    AudioPacketSink() = default;
};

} // namespace semi::domain
