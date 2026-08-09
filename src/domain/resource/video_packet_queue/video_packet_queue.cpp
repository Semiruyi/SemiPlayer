#include "domain/resource/video_packet_queue/video_packet_queue.hpp"

#include <utility>

namespace semi::domain {

VideoPacketQueue::VideoPacketQueue(std::shared_ptr<infra::Notifier> notifier,
                                   std::size_t capacity)
    : notifier_(std::move(notifier)), capacity_(capacity) {}

VideoPacketPushResult VideoPacketQueue::try_push(VideoPacketQueueItem&& item) {
    bool should_notify_not_empty = false;
    {
        std::lock_guard lock(mutex_);
        if (packets_.size() >= capacity_) {
            return VideoPacketPushResult::Full;
        }

        should_notify_not_empty = packets_.empty();
        packets_.push_back(std::move(item));
    }

    if (should_notify_not_empty) {
        notify_not_empty();
    }
    return VideoPacketPushResult::Accepted;
}

std::optional<VideoPacketQueueItem> VideoPacketQueue::try_pop() {
    std::optional<VideoPacketQueueItem> packet;
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

bool VideoPacketQueue::empty() const noexcept {
    std::lock_guard lock(mutex_);
    return packets_.empty();
}

bool VideoPacketQueue::full() const noexcept {
    std::lock_guard lock(mutex_);
    return packets_.size() >= capacity_;
}

std::size_t VideoPacketQueue::size() const noexcept {
    std::lock_guard lock(mutex_);
    return packets_.size();
}

void VideoPacketQueue::clear() noexcept {
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

void VideoPacketQueue::notify_not_empty() noexcept {
    if (!notifier_) {
        return;
    }

    try {
        (void)notifier_->send(VideoQueueNotEmpty{});
    } catch (...) {
    }
}

void VideoPacketQueue::notify_not_full() noexcept {
    if (!notifier_) {
        return;
    }

    try {
        (void)notifier_->send(VideoQueueNotFull{});
    } catch (...) {
    }
}

} // namespace semi::domain
