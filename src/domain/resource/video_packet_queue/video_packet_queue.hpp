#pragma once

#include "domain/resource/video_packet_queue/video_packet_queue_events.hpp"
#include "domain/resource/video_packet_queue/video_packet_source.hpp"
#include "domain/resource/video_packet_queue/video_packet_sink.hpp"
#include "infrastructure/notifier/notifier.hpp"

#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>

namespace semi::domain {

class VideoPacketQueue final : public VideoPacketSink, public VideoPacketSource {
public:
    static constexpr std::size_t kDefaultCapacity = 64;

    explicit VideoPacketQueue(std::shared_ptr<infra::Notifier> notifier,
                              std::size_t capacity = kDefaultCapacity);
    ~VideoPacketQueue() override = default;

    VideoPacketQueue(const VideoPacketQueue&) = delete;
    VideoPacketQueue& operator=(const VideoPacketQueue&) = delete;
    VideoPacketQueue(VideoPacketQueue&&) = delete;
    VideoPacketQueue& operator=(VideoPacketQueue&&) = delete;

    [[nodiscard]] VideoPacketPushResult try_push(VideoPacketQueueItem&& item) override;
    [[nodiscard]] std::optional<VideoPacketQueueItem> try_pop() override;

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] bool full() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

    void clear() noexcept;

private:
    void notify_not_empty() noexcept;
    void notify_not_full() noexcept;

    std::shared_ptr<infra::Notifier> notifier_;
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<VideoPacketQueueItem> packets_;
};

} // namespace semi::domain
