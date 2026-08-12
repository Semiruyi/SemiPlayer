#pragma once

#include "semi_player/semi_player.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace semi::example {

struct OwnedVideoFrame {
    std::vector<std::uint8_t> pixels;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride_bytes = 0;
    std::int64_t pts_us = 0;
    bool has_pts = false;
};

class LatestFrameMailbox final {
public:
    void set_wake_event(Uint32 event_type) noexcept;
    void publish(const semi_video_frame_t* frame) noexcept;
    [[nodiscard]] bool take_latest(OwnedVideoFrame& destination) noexcept;

private:
    void wake_main_thread() noexcept;

    std::mutex mutex_;
    OwnedVideoFrame write_frame_;
    OwnedVideoFrame pending_frame_;
    bool has_pending_frame_ = false;
    std::atomic<Uint32> wake_event_{0};
    std::atomic<bool> wake_queued_{false};
};

class VideoPresenter final {
public:
    explicit VideoPresenter(SDL_Renderer* renderer) noexcept;
    ~VideoPresenter();

    VideoPresenter(const VideoPresenter&) = delete;
    VideoPresenter& operator=(const VideoPresenter&) = delete;

    [[nodiscard]] bool present_latest(LatestFrameMailbox& mailbox) noexcept;
    [[nodiscard]] bool redraw() noexcept;
    [[nodiscard]] std::int64_t current_pts_us() const noexcept;

private:
    [[nodiscard]] bool ensure_texture(std::uint32_t width,
                                      std::uint32_t height) noexcept;
    [[nodiscard]] bool upload_and_present() noexcept;
    [[nodiscard]] bool draw_current_texture() noexcept;

    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* texture_ = nullptr;
    std::uint32_t texture_width_ = 0;
    std::uint32_t texture_height_ = 0;
    OwnedVideoFrame display_frame_;
};

} // namespace semi::example
