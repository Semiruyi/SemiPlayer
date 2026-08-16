#include "domain/worker/video_sync/video_sync_input.hpp"

#include "domain/resource/video_rendered_store/video_rendered_store_item.hpp"

#include <utility>

namespace semi::domain {

VideoSyncInput::VideoSyncInput(std::shared_ptr<VideoRenderedSource> source) noexcept
    : source_(std::move(source)) {}

bool VideoSyncInput::has_source() const noexcept {
    return static_cast<bool>(source_);
}

bool VideoSyncInput::has_available_hint() const noexcept {
    return input_available_hint_.load(std::memory_order_acquire);
}

bool VideoSyncInput::end_of_input_observed() const noexcept {
    return end_of_input_observed_.load(std::memory_order_acquire);
}

void VideoSyncInput::reset() noexcept {
    input_available_hint_.store(false, std::memory_order_release);
    end_of_input_observed_.store(false, std::memory_order_release);
}

void VideoSyncInput::mark_available() noexcept {
    input_available_hint_.store(true, std::memory_order_release);
}

VideoSyncInputResult
VideoSyncInput::try_pop_current(Generation::Value current_generation) noexcept {
    VideoSyncInputResult result;
    if (end_of_input_observed_.load(std::memory_order_acquire) ||
        !input_available_hint_.exchange(false, std::memory_order_acq_rel)) {
        return result;
    }

    if (!source_) {
        result.kind = VideoSyncInputResultKind::Empty;
        return result;
    }

    for (;;) {
        auto popped = source_->try_pop();
        if (!popped) {
            result.kind = VideoSyncInputResultKind::Empty;
            return result;
        }

        if (video_rendered_store_item_generation(*popped) != current_generation) {
            ++result.stale_items_dropped;
            continue;
        }

        if (auto* frame = std::get_if<RenderedVideoFrame>(&*popped)) {
            result.kind = VideoSyncInputResultKind::Frame;
            result.frame.emplace(std::move(*frame));
            result.frame_popped = true;
            input_available_hint_.store(true, std::memory_order_release);
            return result;
        }

        result.kind = VideoSyncInputResultKind::EndOfInput;
        end_of_input_observed_.store(true, std::memory_order_release);
        return result;
    }
}

} // namespace semi::domain
