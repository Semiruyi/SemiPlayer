#pragma once

#include "contracts/demuxer/packet/encoded_packet.hpp"
#include "contracts/media/media_types.hpp"

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace semi::contracts::video_decoder {

// The operation that failed in a video decoder implementation. Native error
// codes are backend-specific (for example, an FFmpeg error code).
enum class VideoDecoderBackendOperation : std::uint8_t {
    Configure,
    Decode,
    Drain,
};

struct VideoDecoderBackendError {
    VideoDecoderBackendOperation operation = VideoDecoderBackendOperation::Configure;
    int native_code = 0;
    std::string message;
};

using DecodedVideoBatch = std::vector<media::DecodedVideo>;

// Backend boundary for compressed video decoding. Implementations own all
// native codec state; returned frames own their pixel storage and have no
// native lifetime dependency. Calls are made exclusively by the domain worker.
class VideoDecoderBackend {
public:
    virtual ~VideoDecoderBackend() = default;

    VideoDecoderBackend(const VideoDecoderBackend&) = delete;
    VideoDecoderBackend& operator=(const VideoDecoderBackend&) = delete;
    VideoDecoderBackend(VideoDecoderBackend&&) = delete;
    VideoDecoderBackend& operator=(VideoDecoderBackend&&) = delete;

    [[nodiscard]] virtual std::expected<void, VideoDecoderBackendError>
    configure(const media::VideoCodecConfig& config) = 0;

    // A packet may produce zero, one, or many decoded video frames.
    [[nodiscard]] virtual std::expected<DecodedVideoBatch, VideoDecoderBackendError>
    decode(const demuxer::packet::EncodedPacket& packet) = 0;

    // Drains delayed codec frames after an end-of-input marker.
    [[nodiscard]] virtual std::expected<DecodedVideoBatch, VideoDecoderBackendError>
    drain() = 0;

    // Clears delayed codec data after the worker has observed a new generation.
    virtual void reset() noexcept = 0;
    virtual void unconfigure() noexcept = 0;

protected:
    VideoDecoderBackend() = default;
};

} // namespace semi::contracts::video_decoder
