#pragma once

#include "domain/resource/video_rendered_store/video_rendered_store_source.hpp"

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace semi::domain {

enum class VideoSyncErrorCode : std::uint8_t {
    InvalidState,
    Internal,
};

struct VideoSyncError {
    VideoSyncErrorCode code = VideoSyncErrorCode::Internal;
    std::string message;
};

struct VideoSyncOptions {
    // Audio is the master clock when the opened media has an audio stream.
    // Video-only media uses the local monotonic clock fallback.
    bool audio_master = true;
    std::function<void(const RenderedVideoFrame&)> on_frame;
};

// Final video consumer. The worker lives for the module lifetime; configure /
// unconfigure delimit a media session, while start / pause control playback.
class VideoSync {
public:
    virtual ~VideoSync() = default;

    VideoSync(const VideoSync&) = delete;
    VideoSync& operator=(const VideoSync&) = delete;
    VideoSync(VideoSync&&) = delete;
    VideoSync& operator=(VideoSync&&) = delete;

    [[nodiscard]] virtual std::expected<void, VideoSyncError>
    configure(const VideoSyncOptions& options) = 0;

    [[nodiscard]] virtual std::expected<void, VideoSyncError>
    start_playback() = 0;

    [[nodiscard]] virtual std::expected<void, VideoSyncError>
    pause_playback() = 0;

    virtual void unconfigure() noexcept = 0;

protected:
    VideoSync() = default;
};

} // namespace semi::domain
