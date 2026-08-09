#pragma once

#include "domain/resource/video_packet_queue/video_packet.hpp"

#include <variant>

namespace semi::domain {

struct VideoPacketEndOfInput {
    Generation::Value generation = 0;
};

using VideoPacketQueueItem = std::variant<VideoPacket, VideoPacketEndOfInput>;

[[nodiscard]] inline Generation::Value
video_packet_queue_item_generation(const VideoPacketQueueItem& item) noexcept {
    if (const auto* packet = std::get_if<VideoPacket>(&item)) {
        return packet->generation();
    }
    return std::get<VideoPacketEndOfInput>(item).generation;
}

[[nodiscard]] inline bool is_current_video_packet_queue_item(
    const VideoPacketQueueItem& item, Generation::Value current_generation) noexcept {
    return video_packet_queue_item_generation(item) == current_generation;
}

} // namespace semi::domain
