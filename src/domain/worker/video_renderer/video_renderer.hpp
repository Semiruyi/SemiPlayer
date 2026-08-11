#pragma once

#include "contracts/video_renderer/video_renderer_backend.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>

namespace semi::domain {

using contracts::video_renderer::VideoRendererBackend;
using contracts::video_renderer::VideoRendererBackendError;
using contracts::video_renderer::VideoRendererBackendOperation;
using contracts::video_renderer::VideoRendererOptions;

enum class VideoRendererErrorCode : std::uint8_t {
    InvalidState,
    BackendFailure,
};

struct VideoRendererError {
    VideoRendererErrorCode code = VideoRendererErrorCode::BackendFailure;
    std::string message;
    std::optional<VideoRendererBackendError> backend_error;
};

// Control-plane interface for the CPU video renderer. The worker owns its
// thread for the module lifetime and converts decoded frames into host-format
// frames for an injected VideoRenderedSink.
class VideoRenderer {
public:
    virtual ~VideoRenderer() = default;

    VideoRenderer(const VideoRenderer&) = delete;
    VideoRenderer& operator=(const VideoRenderer&) = delete;
    VideoRenderer(VideoRenderer&&) = delete;
    VideoRenderer& operator=(VideoRenderer&&) = delete;

    [[nodiscard]] virtual std::expected<void, VideoRendererError>
    configure(const VideoRendererOptions& options) = 0;

    virtual void unconfigure() noexcept = 0;

protected:
    VideoRenderer() = default;
};

} // namespace semi::domain
