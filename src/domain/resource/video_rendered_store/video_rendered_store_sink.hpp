#pragma once

#include "domain/resource/video_rendered_store/video_rendered_store_item.hpp"

#include <cstdint>

namespace semi::domain {

enum class VideoRenderedPushResult : std::uint8_t {
    Accepted,
    Full,
};

class VideoRenderedSink {
public:
    virtual ~VideoRenderedSink() = default;

    VideoRenderedSink(const VideoRenderedSink&) = delete;
    VideoRenderedSink& operator=(const VideoRenderedSink&) = delete;
    VideoRenderedSink(VideoRenderedSink&&) = delete;
    VideoRenderedSink& operator=(VideoRenderedSink&&) = delete;

    [[nodiscard]] virtual VideoRenderedPushResult
    try_push(VideoRenderedStoreItem&& item) = 0;

protected:
    VideoRenderedSink() = default;
};

} // namespace semi::domain
