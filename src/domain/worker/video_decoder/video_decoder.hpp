#pragma once

#include "contracts/video_decoder/video_decoder_backend.hpp"
#include "contracts/media/media_types.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>

namespace semi::domain {

using contracts::video_decoder::VideoDecoderBackend;
using contracts::video_decoder::VideoDecoderBackendError;
using contracts::video_decoder::VideoDecoderBackendOperation;

enum class VideoDecoderErrorCode : std::uint8_t {
    InvalidState,
    BackendFailure,
};

struct VideoDecoderError {
    VideoDecoderErrorCode code = VideoDecoderErrorCode::BackendFailure;
    std::string message;
    std::optional<VideoDecoderBackendError> backend_error;
};

// Control-plane interface for the video decoding worker. The worker belongs
// to the module lifecycle (constructed with the module, joined on destruction),
// so there is no start()/stop().
class VideoDecoder {
public:
    virtual ~VideoDecoder() = default;

    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;
    VideoDecoder(VideoDecoder&&) = delete;
    VideoDecoder& operator=(VideoDecoder&&) = delete;

    [[nodiscard]] virtual std::expected<void, VideoDecoderError>
    configure(const contracts::media::VideoCodecConfig& config) = 0;

    virtual void unconfigure() noexcept = 0;

protected:
    VideoDecoder() = default;
};

} // namespace semi::domain
