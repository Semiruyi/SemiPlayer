#pragma once

#include "domain/resource/audio_frame_store/audio_frame_sink.hpp"
#include "domain/resource/audio_frame_store/audio_frame_source.hpp"
#include "domain/resource/audio_frame_store/audio_frame_store_events.hpp"
#include "infrastructure/notifier/notifier.hpp"

#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>

namespace semi::domain {

class AudioFrameStore final : public AudioFrameSink, public AudioFrameSource {
public:
    static constexpr std::size_t kDefaultCapacity = 64;

    explicit AudioFrameStore(std::shared_ptr<infra::Notifier> notifier,
                             std::size_t capacity = kDefaultCapacity);
    ~AudioFrameStore() override = default;

    AudioFrameStore(const AudioFrameStore&) = delete;
    AudioFrameStore& operator=(const AudioFrameStore&) = delete;
    AudioFrameStore(AudioFrameStore&&) = delete;
    AudioFrameStore& operator=(AudioFrameStore&&) = delete;

    [[nodiscard]] AudioFramePushResult try_push(AudioFrameStoreItem&& item) override;
    [[nodiscard]] std::optional<AudioFrameStoreItem> try_pop() override;

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] bool full() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    void notify_not_empty() noexcept;
    void notify_not_full() noexcept;

    std::shared_ptr<infra::Notifier> notifier_;
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<AudioFrameStoreItem> items_;
};

} // namespace semi::domain
