#pragma once

#include "contracts/video_renderer/video_renderer_backend.hpp"

#include <memory>

namespace semi::contracts::video_readback { class VideoReadback; }
namespace semi::infra { class Notifier; }

namespace semi::infra::ffmpeg::video_renderer {

class FfmpegVideoRendererBackend final : public contracts::video_renderer::VideoRendererBackend {
public:
    explicit FfmpegVideoRendererBackend(
        std::shared_ptr<contracts::video_readback::VideoReadback> readback = {},
        std::shared_ptr<Notifier> notifier = {});
    ~FfmpegVideoRendererBackend() override;

    [[nodiscard]] std::expected<void, contracts::video_renderer::VideoRendererBackendError>
    configure(const contracts::video_renderer::VideoRendererOptions& options) override;

    [[nodiscard]] std::expected<contracts::media::RenderedVideo,
                                contracts::video_renderer::VideoRendererBackendError>
    render(const contracts::media::DecodedVideo& input) override;

    void reset() noexcept override;
    void unconfigure() noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace semi::infra::ffmpeg::video_renderer
