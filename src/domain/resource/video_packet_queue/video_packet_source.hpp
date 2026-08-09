#pragma once

#include "domain/resource/video_packet_queue/video_packet_queue_item.hpp"

#include <optional>

namespace semi::domain {

class VideoPacketSource {
public:
    virtual ~VideoPacketSource() = default;

    VideoPacketSource(const VideoPacketSource&) = delete;
    VideoPacketSource& operator=(const VideoPacketSource&) = delete;
    VideoPacketSource(VideoPacketSource&&) = delete;
    VideoPacketSource& operator=(VideoPacketSource&&) = delete;

    [[nodiscard]] virtual std::optional<VideoPacketQueueItem> try_pop() = 0;

protected:
    VideoPacketSource() = default;
};

} // namespace semi::domain
