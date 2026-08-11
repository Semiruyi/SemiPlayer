#pragma once

#include "contracts/media/media_types.hpp"
#include "domain/resource/generation/generation.hpp"

#include <utility>

namespace semi::domain {

// One host-format video frame produced by VideoRenderer. The pixel buffer is
// owned by the frame and remains valid while the item is held by a consumer.
class RenderedVideoFrame final {
public:
    RenderedVideoFrame(contracts::media::RenderedVideo rendered,
                       Generation::Value generation) noexcept
        : rendered_(std::move(rendered)), generation_(generation) {}

    ~RenderedVideoFrame() = default;

    RenderedVideoFrame(const RenderedVideoFrame&) = delete;
    RenderedVideoFrame& operator=(const RenderedVideoFrame&) = delete;
    RenderedVideoFrame(RenderedVideoFrame&&) noexcept = default;
    RenderedVideoFrame& operator=(RenderedVideoFrame&&) noexcept = default;

    [[nodiscard]] Generation::Value generation() const noexcept {
        return generation_;
    }

    [[nodiscard]] const contracts::media::RenderedVideo& rendered() const noexcept {
        return rendered_;
    }

private:
    contracts::media::RenderedVideo rendered_;
    Generation::Value generation_ = 0;
};

[[nodiscard]] inline bool is_current_rendered_video_frame(
    const RenderedVideoFrame& frame, Generation::Value current_generation) noexcept {
    return frame.generation() == current_generation;
}

} // namespace semi::domain
