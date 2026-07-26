#pragma once

#include "domain/resource/audio_packet_queue/audio_packet.hpp"

#include <cstdint>

namespace semi::domain {

enum class AudioPacketPushResult : std::uint8_t {
    Accepted,
    Full,
};

// Producer-side port for delivering domain audio packets. The sink must not
// consume the packet when it reports Full.
class AudioPacketSink {
public:
    virtual ~AudioPacketSink() = default;

    AudioPacketSink(const AudioPacketSink&) = delete;
    AudioPacketSink& operator=(const AudioPacketSink&) = delete;
    AudioPacketSink(AudioPacketSink&&) = delete;
    AudioPacketSink& operator=(AudioPacketSink&&) = delete;

    [[nodiscard]] virtual AudioPacketPushResult try_push(AudioPacket&& packet) = 0;

protected:
    AudioPacketSink() = default;
};

} // namespace semi::domain
