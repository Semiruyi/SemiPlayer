#pragma once

#include "domain/resource/video_rendered_store/video_rendered_store_item.hpp"

#include <optional>

namespace semi::domain {

class VideoRenderedSource {
public:
    virtual ~VideoRenderedSource() = default;

    VideoRenderedSource(const VideoRenderedSource&) = delete;
    VideoRenderedSource& operator=(const VideoRenderedSource&) = delete;
    VideoRenderedSource(VideoRenderedSource&&) = delete;
    VideoRenderedSource& operator=(VideoRenderedSource&&) = delete;

    [[nodiscard]] virtual std::optional<VideoRenderedStoreItem> try_pop() = 0;

protected:
    VideoRenderedSource() = default;
};

} // namespace semi::domain
