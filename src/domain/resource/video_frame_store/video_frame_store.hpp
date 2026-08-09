#pragma once

#include "domain/resource/video_frame_store/video_frame_sink.hpp"
#include "domain/resource/video_frame_store/video_frame_source.hpp"
#include "domain/resource/video_frame_store/video_frame_store_events.hpp"
#include "infrastructure/notifier/notifier.hpp"

#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>

namespace semi::domain {

class VideoFrameStore final : public VideoFrameSink, public VideoFrameSource {
public:
    static constexpr std::size_t kDefaultCapacity = 64;

    explicit VideoFrameStore(std::shared_ptr<infra::Notifier> notifier,
                             std::size_t capacity = kDefaultCapacity);
    ~VideoFrameStore() override = default;

    VideoFrameStore(const VideoFrameStore&) = delete;
    VideoFrameStore& operator=(const VideoFrameStore&) = delete;
    VideoFrameStore(VideoFrameStore&&) = delete;
    VideoFrameStore& operator=(VideoFrameStore&&) = delete;

    [[nodiscard]] VideoFramePushResult try_push(VideoFrameStoreItem&& item) override;
    [[nodiscard]] std::optional<VideoFrameStoreItem> try_pop() override;

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] bool full() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    void notify_not_empty() noexcept;
    void notify_not_full() noexcept;

    std::shared_ptr<infra::Notifier> notifier_;
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<VideoFrameStoreItem> items_;
};

} // namespace semi::domain
