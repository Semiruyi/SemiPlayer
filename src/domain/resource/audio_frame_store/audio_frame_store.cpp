#include "domain/resource/audio_frame_store/audio_frame_store.hpp"

#include <memory>
#include <utility>

namespace semi::domain {

AudioFrameStore::AudioFrameStore(std::shared_ptr<infra::Notifier> notifier,
                                 std::size_t capacity)
    : notifier_(std::move(notifier)), capacity_(capacity) {}

AudioFramePushResult AudioFrameStore::try_push(AudioFrameStoreItem&& item) {
    bool should_notify_not_empty = false;
    {
        std::lock_guard lock(mutex_);
        if (items_.size() >= capacity_) {
            return AudioFramePushResult::Full;
        }

        should_notify_not_empty = items_.empty();
        items_.push_back(std::move(item));
    }

    if (should_notify_not_empty) {
        notify_not_empty();
    }
    return AudioFramePushResult::Accepted;
}

std::optional<AudioFrameStoreItem> AudioFrameStore::try_pop() {
    std::optional<AudioFrameStoreItem> item;
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

bool AudioFrameStore::empty() const noexcept {
    std::lock_guard lock(mutex_);
    return items_.empty();
}

bool AudioFrameStore::full() const noexcept {
    std::lock_guard lock(mutex_);
    return items_.size() >= capacity_;
}

std::size_t AudioFrameStore::size() const noexcept {
    std::lock_guard lock(mutex_);
    return items_.size();
}

void AudioFrameStore::notify_not_empty() noexcept {
    if (!notifier_) {
        return;
    }

    try {
        (void)notifier_->send(AudioFrameStoreNotEmpty{});
    } catch (...) {
        // The store state is already committed; notification is only a wake-up hint.
    }
}

void AudioFrameStore::notify_not_full() noexcept {
    if (!notifier_) {
        return;
    }

    try {
        (void)notifier_->send(AudioFrameStoreNotFull{});
    } catch (...) {
        // The store state is already committed; notification is only a wake-up hint.
    }
}

} // namespace semi::domain
