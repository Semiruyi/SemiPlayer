#include "domain/resource/audio_packet_queue/audio_packet_queue.hpp"

#include <memory>
#include <utility>

namespace semi::domain {

AudioPacketQueue::AudioPacketQueue(std::shared_ptr<infra::Notifier> notifier,
                                   std::size_t capacity)
    : notifier_(std::move(notifier)), capacity_(capacity) {}

AudioPacketPushResult AudioPacketQueue::try_push(AudioPacketQueueItem&& item) {
    bool should_notify_not_empty = false;
    {
        std::lock_guard lock(mutex_);
        if (packets_.size() >= capacity_) {
            return AudioPacketPushResult::Full;
        }

        should_notify_not_empty = packets_.empty();
        packets_.push_back(std::move(item));
    }

    if (should_notify_not_empty) {
        notify_not_empty();
    }
    return AudioPacketPushResult::Accepted;
}

std::optional<AudioPacketQueueItem> AudioPacketQueue::try_pop() {
    std::optional<AudioPacketQueueItem> packet;
    bool should_notify_not_full = false;
    {
        std::lock_guard lock(mutex_);
        if (packets_.empty()) {
            return std::nullopt;
        }

        should_notify_not_full = packets_.size() >= capacity_;
        packet.emplace(std::move(packets_.front()));
        packets_.pop_front();
    }

    if (should_notify_not_full) {
        notify_not_full();
    }
    return packet;
}

bool AudioPacketQueue::empty() const noexcept {
    std::lock_guard lock(mutex_);
    return packets_.empty();
}

bool AudioPacketQueue::full() const noexcept {
    std::lock_guard lock(mutex_);
    return packets_.size() >= capacity_;
}

std::size_t AudioPacketQueue::size() const noexcept {
    std::lock_guard lock(mutex_);
    return packets_.size();
}

void AudioPacketQueue::clear() noexcept {
    bool should_notify_not_full = false;
    {
        std::lock_guard lock(mutex_);
        should_notify_not_full = packets_.size() >= capacity_ && !packets_.empty();
        packets_.clear();
    }

    if (should_notify_not_full) {
        notify_not_full();
    }
}

void AudioPacketQueue::notify_not_empty() noexcept {
    if (!notifier_) {
        return;
    }

    try {
        (void)notifier_->send(AudioQueueNotEmpty{});
    } catch (...) {
        // The queue state is already committed; notification is only a wake-up hint.
    }
}

void AudioPacketQueue::notify_not_full() noexcept {
    if (!notifier_) {
        return;
    }

    try {
        (void)notifier_->send(AudioQueueNotFull{});
    } catch (...) {
        // The queue state is already committed; notification is only a wake-up hint.
    }
}

} // namespace semi::domain
