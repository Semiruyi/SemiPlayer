#pragma once

#include "contracts/media/media_types.hpp"

#include <expected>

namespace semi::contracts::video_readback {

enum class ReadbackOperation : std::uint8_t {
    Validate,
    Allocate,
    CreateStaging,
    Map,
};

struct ReadbackError {
    ReadbackOperation operation = ReadbackOperation::Validate;
    std::int32_t native_code = 0; // Backend-specific; zero if no native call failed.
    std::string message;
};

// Synchronous, sequential-use resource transfer. The caller retains input for
// the entire call. Success owns independent CPU storage, exposes
// CpuVideoFrameResource, and preserves format and visible dimensions.
// No conversion, timestamps, generation or playback policy belongs here.
class VideoReadback {
public:
    virtual ~VideoReadback() = default;

    [[nodiscard]] virtual std::expected<std::unique_ptr<const media::VideoFrameBuffer>, ReadbackError>
    readback(const media::VideoFrameBuffer& input) = 0;
};

} // namespace semi::contracts::video_readback
