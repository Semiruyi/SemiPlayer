#include "video_presenter.hpp"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

namespace semi::example {

namespace {

bool is_valid_frame(const semi_video_frame_t& frame) noexcept {
    if (frame.struct_size < sizeof(semi_video_frame_t) ||
        frame.pixel_format != SEMI_VIDEO_PIXEL_FORMAT_RGBA8888 ||
        frame.width == 0 || frame.height == 0 || frame.plane_count != 1) {
        return false;
    }

    const auto& plane = frame.planes[0];
    if (plane.data == nullptr ||
        frame.width > std::numeric_limits<std::uint32_t>::max() / 4U) {
        return false;
    }

    const std::uint32_t tight_stride = frame.width * 4U;
    if (plane.stride_bytes < tight_stride) {
        return false;
    }

    const std::uint64_t required_source_bytes =
        static_cast<std::uint64_t>(plane.stride_bytes) * frame.height;
    const std::uint64_t required_destination_bytes =
        static_cast<std::uint64_t>(tight_stride) * frame.height;
    return plane.size_bytes >= required_source_bytes &&
        required_destination_bytes <= std::numeric_limits<std::size_t>::max();
}

} // namespace

void LatestFrameMailbox::set_wake_event(Uint32 event_type) noexcept {
    wake_event_.store(event_type, std::memory_order_release);
}

void LatestFrameMailbox::publish(const semi_video_frame_t* frame) noexcept {
    if (frame == nullptr || !is_valid_frame(*frame)) {
        return;
    }

    try {
        const std::uint32_t tight_stride = frame->width * 4U;
        const std::size_t byte_count =
            static_cast<std::size_t>(tight_stride) * frame->height;

        {
            std::scoped_lock lock(mutex_);
            write_frame_.pixels.resize(byte_count);
            for (std::uint32_t row = 0; row < frame->height; ++row) {
                const auto* source = frame->planes[0].data +
                    static_cast<std::size_t>(row) * frame->planes[0].stride_bytes;
                auto* destination = write_frame_.pixels.data() +
                    static_cast<std::size_t>(row) * tight_stride;
                std::memcpy(destination, source, tight_stride);
            }
            write_frame_.width = frame->width;
            write_frame_.height = frame->height;
            write_frame_.stride_bytes = tight_stride;
            write_frame_.has_pts = frame->has_pts != 0;
            write_frame_.pts_us = frame->pts_us;

            std::swap(write_frame_, pending_frame_);
            has_pending_frame_ = true;
        }

        wake_main_thread();
    } catch (...) {
        // C callbacks must never let exceptions cross the ABI boundary.
    }
}

bool LatestFrameMailbox::take_latest(OwnedVideoFrame& destination) noexcept {
    std::scoped_lock lock(mutex_);
    if (!has_pending_frame_) {
        return false;
    }

    std::swap(destination, pending_frame_);
    has_pending_frame_ = false;
    wake_queued_.store(false, std::memory_order_release);
    return true;
}

void LatestFrameMailbox::wake_main_thread() noexcept {
    const Uint32 event_type = wake_event_.load(std::memory_order_acquire);
    if (event_type == 0 || wake_queued_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    SDL_Event event{};
    event.type = event_type;
    if (!SDL_PushEvent(&event)) {
        wake_queued_.store(false, std::memory_order_release);
    }
}

VideoPresenter::VideoPresenter(SDL_Renderer* renderer) noexcept
    : renderer_(renderer) {}

VideoPresenter::~VideoPresenter() {
    SDL_DestroyTexture(texture_);
}

bool VideoPresenter::present_latest(LatestFrameMailbox& mailbox) noexcept {
    if (!mailbox.take_latest(display_frame_)) {
        return true;
    }
    return upload_and_present();
}

std::int64_t VideoPresenter::current_pts_us() const noexcept {
    return display_frame_.has_pts ? display_frame_.pts_us : 0;
}

bool VideoPresenter::redraw() noexcept {
    return texture_ == nullptr || draw_current_texture();
}

bool VideoPresenter::ensure_texture(std::uint32_t width,
                                    std::uint32_t height) noexcept {
    if (texture_ != nullptr && texture_width_ == width &&
        texture_height_ == height) {
        return true;
    }

    if (width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        height > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        std::fprintf(stderr, "[sdl host] frame dimensions exceed SDL limits\n");
        return false;
    }

    SDL_DestroyTexture(texture_);
    texture_ = nullptr;
    texture_width_ = 0;
    texture_height_ = 0;
    texture_ = SDL_CreateTexture(renderer_,
                                 SDL_PIXELFORMAT_RGBA32,
                                 SDL_TEXTUREACCESS_STREAMING,
                                 static_cast<int>(width),
                                 static_cast<int>(height));
    if (texture_ == nullptr) {
        std::fprintf(stderr, "[sdl host] create texture failed: %s\n", SDL_GetError());
        return false;
    }
    texture_width_ = width;
    texture_height_ = height;
    if (!SDL_SetTextureScaleMode(texture_, SDL_SCALEMODE_LINEAR)) {
        std::fprintf(stderr, "[sdl host] set texture scale mode failed: %s\n",
                     SDL_GetError());
    }
    return true;
}

bool VideoPresenter::upload_and_present() noexcept {
    if (!ensure_texture(display_frame_.width, display_frame_.height)) {
        return false;
    }

    void* texture_pixels = nullptr;
    int texture_pitch = 0;
    if (!SDL_LockTexture(texture_, nullptr, &texture_pixels, &texture_pitch)) {
        std::fprintf(stderr, "[sdl host] lock texture failed: %s\n", SDL_GetError());
        return false;
    }

    if (texture_pitch < 0 ||
        static_cast<std::uint32_t>(texture_pitch) < display_frame_.stride_bytes) {
        std::fprintf(stderr, "[sdl host] texture pitch is smaller than the video row\n");
        SDL_UnlockTexture(texture_);
        return false;
    }

    const std::size_t copy_bytes = display_frame_.stride_bytes;
    for (std::uint32_t row = 0; row < display_frame_.height; ++row) {
        const auto* source = display_frame_.pixels.data() +
            static_cast<std::size_t>(row) * display_frame_.stride_bytes;
        auto* destination = static_cast<std::uint8_t*>(texture_pixels) +
            static_cast<std::size_t>(row) * static_cast<std::size_t>(texture_pitch);
        std::memcpy(destination, source, copy_bytes);
    }
    SDL_UnlockTexture(texture_);

    return draw_current_texture();
}

bool VideoPresenter::draw_current_texture() noexcept {
    int output_width = 0;
    int output_height = 0;
    if (!SDL_GetRenderOutputSize(renderer_, &output_width, &output_height)) {
        std::fprintf(stderr, "[sdl host] get output size failed: %s\n", SDL_GetError());
        return false;
    }
    if (output_width <= 0 || output_height <= 0) {
        return true;
    }

    const float source_aspect = static_cast<float>(display_frame_.width) /
        static_cast<float>(display_frame_.height);
    const float output_aspect = static_cast<float>(output_width) /
        static_cast<float>(output_height);
    SDL_FRect destination{};
    if (output_aspect > source_aspect) {
        destination.h = static_cast<float>(output_height);
        destination.w = destination.h * source_aspect;
        destination.x = (static_cast<float>(output_width) - destination.w) * 0.5F;
    } else {
        destination.w = static_cast<float>(output_width);
        destination.h = destination.w / source_aspect;
        destination.y = (static_cast<float>(output_height) - destination.h) * 0.5F;
    }

    if (!SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255) ||
        !SDL_RenderClear(renderer_) ||
        !SDL_RenderTexture(renderer_, texture_, nullptr, &destination) ||
        !SDL_RenderPresent(renderer_)) {
        std::fprintf(stderr, "[sdl host] render failed: %s\n", SDL_GetError());
        return false;
    }
    return true;
}

} // namespace semi::example
