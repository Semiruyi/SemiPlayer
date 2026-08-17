#pragma once

#include "domain/resource/video_rendered_store/video_rendered_store_events.hpp"
#include "domain/resource/video_rendered_store/video_rendered_store_sink.hpp"
#include "domain/resource/video_rendered_store/video_rendered_store_source.hpp"
#include "infrastructure/notifier/notifier.hpp"

#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>

namespace semi::domain {

class VideoRenderedStore final : public VideoRenderedSink, public VideoRenderedSource {
public:
    static constexpr std::size_t kDefaultCapacity = 3;

    explicit VideoRenderedStore(std::shared_ptr<infra::Notifier> notifier,
                                std::size_t capacity = kDefaultCapacity);
    ~VideoRenderedStore() override = default;

    VideoRenderedStore(const VideoRenderedStore&) = delete;
    VideoRenderedStore& operator=(const VideoRenderedStore&) = delete;
    VideoRenderedStore(VideoRenderedStore&&) = delete;
    VideoRenderedStore& operator=(VideoRenderedStore&&) = delete;

    [[nodiscard]] VideoRenderedPushResult
    try_push(VideoRenderedStoreItem&& item) override;
    [[nodiscard]] std::optional<VideoRenderedStoreItem> try_pop() override;

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] bool full() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    void notify_not_empty() noexcept;
    void notify_not_full() noexcept;

    std::shared_ptr<infra::Notifier> notifier_;
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<VideoRenderedStoreItem> items_;
};

} // namespace semi::domain
