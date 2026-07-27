#pragma once

#include "domain/resource/audio_packet_queue/audio_packet.hpp"

#include <variant>

namespace semi::domain {

// Ordered control item indicating that no more packets will be produced for
// this input generation. It is not an encoded media packet. Consumers must
// check its generation before interpreting it as end-of-input.
struct AudioPacketEndOfInput {
    Generation::Value generation = 0;
};

using AudioPacketQueueItem = std::variant<AudioPacket, AudioPacketEndOfInput>;

[[nodiscard]] inline Generation::Value
audio_packet_queue_item_generation(const AudioPacketQueueItem& item) noexcept {
    if (const auto* packet = std::get_if<AudioPacket>(&item)) {
        return packet->generation();
    }
    return std::get<AudioPacketEndOfInput>(item).generation;
}

// Generation-only invalidation applies equally to packets and end markers.
[[nodiscard]] inline bool is_current_audio_packet_queue_item(
    const AudioPacketQueueItem& item, Generation::Value current_generation) noexcept {
    return audio_packet_queue_item_generation(item) == current_generation;
}

} // namespace semi::domain
