#include "domain/resource/video_rendered_store/video_rendered_store.hpp"

#include <utility>

namespace semi::domain {

VideoRenderedStore::VideoRenderedStore(std::shared_ptr<infra::Notifier> notifier,
                                       std::size_t capacity)
    : notifier_(std::move(notifier)), capacity_(capacity) {}

VideoRenderedPushResult VideoRenderedStore::try_push(VideoRenderedStoreItem&& item) {
    bool should_notify_not_empty = false;
    {
        std::lock_guard lock(mutex_);
        if (items_.size() >= capacity_) {
            return VideoRenderedPushResult::Full;
        }

        should_notify_not_empty = items_.empty();
        items_.push_back(std::move(item));
    }

    if (should_notify_not_empty) {
        notify_not_empty();
    }
    return VideoRenderedPushResult::Accepted;
}

std::optional<VideoRenderedStoreItem> VideoRenderedStore::try_pop() {
    std::optional<VideoRenderedStoreItem> item;
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

bool VideoRenderedStore::empty() const noexcept {
    std::lock_guard lock(mutex_);
    return items_.empty();
}

bool VideoRenderedStore::full() const noexcept {
    std::lock_guard lock(mutex_);
    return items_.size() >= capacity_;
}

std::size_t VideoRenderedStore::size() const noexcept {
    std::lock_guard lock(mutex_);
    return items_.size();
}

void VideoRenderedStore::notify_not_empty() noexcept {
    if (!notifier_) {
        return;
    }

    try {
        (void)notifier_->send(VideoRenderedStoreNotEmpty{});
    } catch (...) {
    }
}

void VideoRenderedStore::notify_not_full() noexcept {
    if (!notifier_) {
        return;
    }

    try {
        (void)notifier_->send(VideoRenderedStoreNotFull{});
    } catch (...) {
    }
}

} // namespace semi::domain
