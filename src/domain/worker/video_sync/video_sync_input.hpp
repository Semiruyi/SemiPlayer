#pragma once

#include "domain/resource/generation/generation.hpp"
#include "domain/resource/video_rendered_store/video_rendered_store_source.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>

namespace semi::domain {

enum class VideoSyncInputResultKind : std::uint8_t {
    NotReady,
    Empty,
    Frame,
    EndOfInput,
};

struct VideoSyncInputResult final {
    VideoSyncInputResultKind kind = VideoSyncInputResultKind::NotReady;
    std::optional<RenderedVideoFrame> frame;
    std::uint64_t stale_items_dropped = 0;
    bool frame_popped = false;
};

// Owns the boundary between VideoSync and VideoRenderedSource. It filters
// stale generations and turns the store's variant into a small result used by
// the scheduler. The availability hint may be written by notifier callbacks;
// all other state is consumed by the VideoSync worker.
class VideoSyncInput final {
public:
    explicit VideoSyncInput(std::shared_ptr<VideoRenderedSource> source) noexcept;

    VideoSyncInput(const VideoSyncInput&) = delete;
    VideoSyncInput& operator=(const VideoSyncInput&) = delete;
    VideoSyncInput(VideoSyncInput&&) = delete;
    VideoSyncInput& operator=(VideoSyncInput&&) = delete;

    [[nodiscard]] bool has_source() const noexcept;
    [[nodiscard]] bool has_available_hint() const noexcept;
    [[nodiscard]] bool end_of_input_observed() const noexcept;

    void reset() noexcept;
    void mark_available() noexcept;

    [[nodiscard]] VideoSyncInputResult
    try_pop_current(Generation::Value current_generation) noexcept;

private:
    std::shared_ptr<VideoRenderedSource> source_;
    std::atomic_bool input_available_hint_ = false;
    std::atomic_bool end_of_input_observed_ = false;
};

} // namespace semi::domain
