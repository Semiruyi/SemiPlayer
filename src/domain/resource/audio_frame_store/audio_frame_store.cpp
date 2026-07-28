#include "domain/resource/audio_frame_store/audio_frame_store.hpp"

#include <memory>
#include <utility>

namespace semi::domain {

AudioFrameStore::AudioFrameStore(std::shared_ptr<infra::Notifier> notifier,
                                 std::size_t capacity)
    : notifier_(std::move(notifier)), capacity_(capacity) {}

AudioFramePushResult AudioFrameStore::try_push(AudioFrame&& frame) {
    bool should_notify_not_empty = false;
    {
        std::lock_guard lock(mutex_);
        if (frames_.size() >= capacity_) {
            return AudioFramePushResult::Full;
        }

        should_notify_not_empty = frames_.empty();
        frames_.push_back(std::move(frame));
    }

    if (should_notify_not_empty) {
        notify_not_empty();
    }
    return AudioFramePushResult::Accepted;
}

std::optional<AudioFrame> AudioFrameStore::try_pop() {
    std::optional<AudioFrame> frame;
    bool should_notify_not_full = false;
    {
        std::lock_guard lock(mutex_);
        if (frames_.empty()) {
            return std::nullopt;
        }

        should_notify_not_full = frames_.size() >= capacity_;
        frame.emplace(std::move(frames_.front()));
        frames_.pop_front();
    }

    if (should_notify_not_full) {
        notify_not_full();
    }
    return frame;
}

bool AudioFrameStore::empty() const noexcept {
    std::lock_guard lock(mutex_);
    return frames_.empty();
}

bool AudioFrameStore::full() const noexcept {
    std::lock_guard lock(mutex_);
    return frames_.size() >= capacity_;
}

std::size_t AudioFrameStore::size() const noexcept {
    std::lock_guard lock(mutex_);
    return frames_.size();
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
