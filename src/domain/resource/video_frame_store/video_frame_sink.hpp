#pragma once

#include "domain/resource/video_frame_store/video_frame_store_item.hpp"

#include <cstdint>

namespace semi::domain {

enum class VideoFramePushResult : std::uint8_t {
    Accepted,
    Full,
};

class VideoFrameSink {
public:
    virtual ~VideoFrameSink() = default;

    VideoFrameSink(const VideoFrameSink&) = delete;
    VideoFrameSink& operator=(const VideoFrameSink&) = delete;
    VideoFrameSink(VideoFrameSink&&) = delete;
    VideoFrameSink& operator=(VideoFrameSink&&) = delete;

    [[nodiscard]] virtual VideoFramePushResult try_push(VideoFrameStoreItem&& item) = 0;

protected:
    VideoFrameSink() = default;
};

} // namespace semi::domain
