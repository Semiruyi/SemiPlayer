#pragma once

#include "domain/resource/video_frame_store/video_frame_store_item.hpp"

#include <optional>

namespace semi::domain {

class VideoFrameSource {
public:
    virtual ~VideoFrameSource() = default;

    VideoFrameSource(const VideoFrameSource&) = delete;
    VideoFrameSource& operator=(const VideoFrameSource&) = delete;
    VideoFrameSource(VideoFrameSource&&) = delete;
    VideoFrameSource& operator=(VideoFrameSource&&) = delete;

    [[nodiscard]] virtual std::optional<VideoFrameStoreItem> try_pop() = 0;

protected:
    VideoFrameSource() = default;
};

} // namespace semi::domain
