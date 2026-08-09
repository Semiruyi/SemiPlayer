#include "domain/resource/video_frame_store/video_frame_store.hpp"

#include <utility>

namespace semi::domain {

VideoFrameStore::VideoFrameStore(std::shared_ptr<infra::Notifier> notifier,
                                 std::size_t capacity)
    : notifier_(std::move(notifier)), capacity_(capacity) {}

VideoFramePushResult VideoFrameStore::try_push(VideoFrameStoreItem&& item) {
    bool should_notify_not_empty = false;
    {
        std::lock_guard lock(mutex_);
        if (items_.size() >= capacity_) {
            return VideoFramePushResult::Full;
        }

        should_notify_not_empty = items_.empty();
        items_.push_back(std::move(item));
    }

    if (should_notify_not_empty) {
        notify_not_empty();
    }
    return VideoFramePushResult::Accepted;
}

std::optional<VideoFrameStoreItem> VideoFrameStore::try_pop() {
    std::optional<VideoFrameStoreItem> item;
    bool should_notify_not_full = false;
    {
        std::lock_guard lock(mutex_);
        if (items_.empty()) {
            return std::nullopt;
        }

        should_notify_not_full = items_.size() >= capacity_;
        item.emplace(std::move(items_.front()));
        items_.pop_front();
    }

    if (should_notify_not_full) {
        notify_not_full();
    }
    return item;
}

bool VideoFrameStore::empty() const noexcept {
    std::lock_guard lock(mutex_);
    return items_.empty();
}

bool VideoFrameStore::full() const noexcept {
    std::lock_guard lock(mutex_);
    return items_.size() >= capacity_;
}

std::size_t VideoFrameStore::size() const noexcept {
    std::lock_guard lock(mutex_);
    return items_.size();
}

void VideoFrameStore::notify_not_empty() noexcept {
    if (!notifier_) {
        return;
    }

    try {
        (void)notifier_->send(VideoFrameStoreNotEmpty{});
    } catch (...) {
    }
}

void VideoFrameStore::notify_not_full() noexcept {
    if (!notifier_) {
        return;
    }

    try {
        (void)notifier_->send(VideoFrameStoreNotFull{});
    } catch (...) {
    }
}

} // namespace semi::domain
