#pragma once

#include "domain/resource/audio_packet_queue/audio_packet_queue_events.hpp"
#include "domain/resource/audio_packet_queue/audio_packet_source.hpp"
#include "domain/resource/audio_packet_queue/audio_packet_sink.hpp"
#include "infrastructure/notifier/notifier.hpp"

#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>

namespace semi::domain {

class AudioPacketQueue final : public AudioPacketSink, public AudioPacketSource {
public:
    static constexpr std::size_t kDefaultCapacity = 64;

    explicit AudioPacketQueue(std::shared_ptr<infra::Notifier> notifier,
                              std::size_t capacity = kDefaultCapacity);
    ~AudioPacketQueue() override = default;

    AudioPacketQueue(const AudioPacketQueue&) = delete;
    AudioPacketQueue& operator=(const AudioPacketQueue&) = delete;
    AudioPacketQueue(AudioPacketQueue&&) = delete;
    AudioPacketQueue& operator=(AudioPacketQueue&&) = delete;

    [[nodiscard]] AudioPacketPushResult try_push(AudioPacketQueueItem&& item) override;
    [[nodiscard]] std::optional<AudioPacketQueueItem> try_pop() override;

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
    std::deque<AudioPacketQueueItem> packets_;
};

} // namespace semi::domain
