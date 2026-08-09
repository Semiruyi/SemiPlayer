#pragma once

#include "domain/resource/video_packet_queue/video_packet_queue_item.hpp"

#include <cstdint>

namespace semi::domain {

enum class VideoPacketPushResult : std::uint8_t {
    Accepted,
    Full,
};

class VideoPacketSink {
public:
    virtual ~VideoPacketSink() = default;

    VideoPacketSink(const VideoPacketSink&) = delete;
    VideoPacketSink& operator=(const VideoPacketSink&) = delete;
    VideoPacketSink(VideoPacketSink&&) = delete;
    VideoPacketSink& operator=(VideoPacketSink&&) = delete;

    [[nodiscard]] virtual VideoPacketPushResult try_push(VideoPacketQueueItem&& item) = 0;

protected:
    VideoPacketSink() = default;
};

} // namespace semi::domain
