#pragma once

#include "domain/resource/video_rendered_store/rendered_video_frame.hpp"

#include <variant>

namespace semi::domain {

struct RenderedVideoEndOfInput {
    Generation::Value generation = 0;
};

using VideoRenderedStoreItem = std::variant<RenderedVideoFrame, RenderedVideoEndOfInput>;

[[nodiscard]] inline Generation::Value
video_rendered_store_item_generation(const VideoRenderedStoreItem& item) noexcept {
    if (const auto* frame = std::get_if<RenderedVideoFrame>(&item)) {
        return frame->generation();
    }
    return std::get<RenderedVideoEndOfInput>(item).generation;
}

[[nodiscard]] inline bool is_current_video_rendered_store_item(
    const VideoRenderedStoreItem& item, Generation::Value current_generation) noexcept {
    return video_rendered_store_item_generation(item) == current_generation;
}

} // namespace semi::domain
