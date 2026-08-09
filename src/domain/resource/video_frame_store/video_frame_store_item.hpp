#pragma once

#include "domain/resource/video_frame_store/video_frame.hpp"

#include <variant>

namespace semi::domain {

struct VideoFrameEndOfInput {
    Generation::Value generation = 0;
};

using VideoFrameStoreItem = std::variant<VideoFrame, VideoFrameEndOfInput>;

[[nodiscard]] inline Generation::Value
video_frame_store_item_generation(const VideoFrameStoreItem& item) noexcept {
    if (const auto* frame = std::get_if<VideoFrame>(&item)) {
        return frame->generation();
    }
    return std::get<VideoFrameEndOfInput>(item).generation;
}

[[nodiscard]] inline bool is_current_video_frame_store_item(
    const VideoFrameStoreItem& item, Generation::Value current_generation) noexcept {
    return video_frame_store_item_generation(item) == current_generation;
}

} // namespace semi::domain
