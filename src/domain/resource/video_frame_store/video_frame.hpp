#pragma once

#include "contracts/media/media_types.hpp"
#include "domain/resource/generation/generation.hpp"

#include <utility>

namespace semi::domain {

// One decoded video frame in the MVP playback pipeline.
class VideoFrame final {
public:
    VideoFrame(contracts::media::DecodedVideo decoded_video,
               Generation::Value generation) noexcept
        : decoded_video_(std::move(decoded_video)), generation_(generation) {}

    ~VideoFrame() = default;

    VideoFrame(const VideoFrame&) = delete;
    VideoFrame& operator=(const VideoFrame&) = delete;
    VideoFrame(VideoFrame&&) noexcept = default;
    VideoFrame& operator=(VideoFrame&&) noexcept = default;

    [[nodiscard]] Generation::Value generation() const noexcept {
        return generation_;
    }

    [[nodiscard]] const contracts::media::DecodedVideo& decoded() const noexcept {
        return decoded_video_;
    }

private:
    contracts::media::DecodedVideo decoded_video_;
    Generation::Value generation_;
};

[[nodiscard]] inline bool is_current_video_frame(
    const VideoFrame& frame, Generation::Value current_generation) noexcept {
    return frame.generation() == current_generation;
}

} // namespace semi::domain
