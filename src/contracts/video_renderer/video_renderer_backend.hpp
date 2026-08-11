#pragma once

#include "contracts/media/media_types.hpp"

#include <cstdint>
#include <expected>
#include <string>

namespace semi::contracts::video_renderer {

enum class VideoRendererBackendOperation : std::uint8_t {
    Configure,
    Render,
};

struct VideoRendererBackendError {
    VideoRendererBackendOperation operation = VideoRendererBackendOperation::Render;
    int native_code = 0;
    std::string message;
};

// The first renderer milestone exposes one portable CPU format. The source
// format remains part of the decoded frame and is discovered at render time;
// the output format is deliberately fixed so the host contract stays stable.
struct VideoRendererOptions {
    media::VideoPixelFormat output_pixel_format = media::VideoPixelFormat::Rgba8;
    std::uint32_t output_width = 0;
    std::uint32_t output_height = 0;
};

class VideoRendererBackend {
public:
    virtual ~VideoRendererBackend() = default;

    VideoRendererBackend(const VideoRendererBackend&) = delete;
    VideoRendererBackend& operator=(const VideoRendererBackend&) = delete;
    VideoRendererBackend(VideoRendererBackend&&) = delete;
    VideoRendererBackend& operator=(VideoRendererBackend&&) = delete;

    // Configure the output side of the conversion. A zero output dimension
    // preserves the decoded frame dimensions.
    [[nodiscard]] virtual std::expected<void, VideoRendererBackendError>
    configure(const VideoRendererOptions& options) = 0;

    // Converts one decoded frame. The returned frame owns its pixel storage
    // and is independent from the input VideoFrameBuffer lifetime.
    [[nodiscard]] virtual std::expected<media::RenderedVideo, VideoRendererBackendError>
    render(const media::DecodedVideo& input) = 0;

    // Clears cached conversion state after a generation change.
    virtual void reset() noexcept = 0;
    virtual void unconfigure() noexcept = 0;

protected:
    VideoRendererBackend() = default;
};

} // namespace semi::contracts::video_renderer
