#pragma once

#include "contracts/media/media_types.hpp"
#include "infrastructure/ffmpeg/ffmpeg_raii.hpp"

#include <expected>
#include <memory>
#include <string>
#include <string_view>

namespace semi::infra::gpu { class GpuDeviceManager; }

namespace semi::infra::ffmpeg::hardware_decode {

struct HardwareDecodeError {
    int native_code = 0; // FFmpeg error code; native device errors are described in message.
    std::string message;
};

// One decoder configuration, not a playback identifier. The codec context must
// be destroyed before this object (including any get_format worker callbacks).
class FfmpegHardwareDecodeSession {
public:
    virtual ~FfmpegHardwareDecodeSession() = default;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    // Called on a fresh, unopened context. Does not take ownership of context.
    [[nodiscard]] virtual std::expected<void, HardwareDecodeError>
    configure(AVCodecContext& context) = 0;
    // Called only from get_format. Return a candidate, or NONE to decline;
    // the backend then chooses a supported software candidate. No exceptions
    // may cross FFmpeg's C callback boundary. May be called repeatedly.
    [[nodiscard]] virtual AVPixelFormat select_format(
        AVCodecContext& context, const AVPixelFormat* candidates) noexcept = 0;
    // The returned owner must keep its resources alive independently of session.
    [[nodiscard]] virtual std::expected<std::unique_ptr<const contracts::media::VideoFrameBuffer>,
                                        HardwareDecodeError>
    wrap_frame(AvFramePtr frame) = 0;
};

class FfmpegHardwareDecodeProvider {
public:
    virtual ~FfmpegHardwareDecodeProvider() = default;
    [[nodiscard]] virtual std::expected<std::unique_ptr<FfmpegHardwareDecodeSession>,
                                        HardwareDecodeError>
    prepare(const AVCodec& codec) = 0;
};

// Build-specific assembly only. Null means no implementation in this build.
[[nodiscard]] std::shared_ptr<FfmpegHardwareDecodeProvider>
make_platform_hardware_decode_provider(std::shared_ptr<gpu::GpuDeviceManager> devices);

} // namespace semi::infra::ffmpeg::hardware_decode
