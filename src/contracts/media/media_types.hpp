#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace semi::contracts::media {

struct DemuxerStreamId {
    std::uint32_t value = 0;
};

struct TimeBase {
    std::int32_t numerator = 0;
    std::int32_t denominator = 1;
};

struct StreamTiming {
    TimeBase time_base;
    std::optional<std::int64_t> start_pts;
    std::optional<std::int64_t> duration_pts;
};

struct CodecCommon {
    std::string codec_name;
    std::vector<std::byte> extradata;
};

struct VideoCodecConfig {
    CodecCommon common;
    std::uint32_t coded_width = 0;
    std::uint32_t coded_height = 0;
    std::optional<std::int32_t> profile;
    std::optional<std::int32_t> level;
};

enum class VideoPixelFormat : std::uint8_t {
    Unknown,
    Yuv420p,
    Yuv422p,
    Yuv444p,
    Nv12,
    P010,
    Rgba8,
};

// A read-only view into one decoded video plane. The view does not own the
// memory; the VideoFrameBuffer that returned it must outlive the view.
struct VideoPlaneView {
    const std::byte* data = nullptr;
    std::size_t size_bytes = 0;
    std::uint32_t stride_bytes = 0;

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return std::span<const std::byte>(data, size_bytes);
    }
};

// Backend-neutral ownership and access boundary for one decoded video frame.
// Implementations may own copied memory or retain a reference-counted native
// buffer. The domain only observes immutable frame metadata and plane views.
class VideoFrameBuffer {
public:
    virtual ~VideoFrameBuffer() = default;

    [[nodiscard]] virtual VideoPixelFormat pixel_format() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t width() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t height() const noexcept = 0;
    [[nodiscard]] virtual std::size_t plane_count() const noexcept = 0;
    [[nodiscard]] virtual VideoPlaneView plane(std::size_t index) const noexcept = 0;

protected:
    VideoFrameBuffer() = default;
};

// Ownership-transferred decoded video. The unique buffer owner keeps all plane
// data alive while the consumer uses the immutable views exposed by the buffer.
struct DecodedVideo {
    std::unique_ptr<const VideoFrameBuffer> buffer;
    std::optional<std::int64_t> pts_us;
};

// Host-neutral rendered video. The renderer owns a tightly described CPU
// pixel buffer so downstream presentation code does not depend on FFmpeg
// native frame lifetime or pixel-plane layout.
struct RenderedVideo {
    VideoPixelFormat pixel_format = VideoPixelFormat::Rgba8;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride_bytes = 0;
    std::vector<std::byte> pixels;
    std::optional<std::int64_t> pts_us;
};

struct AudioCodecConfig {
    CodecCommon common;
    std::uint32_t sample_rate = 0;
    std::uint32_t channels = 0;
};

// PCM sample storage type. Planarity belongs to AudioPcmFormat because every
// sample type can be represented as either one interleaved plane or multiple
// per-channel planes.
enum class AudioSampleFormat : std::uint8_t {
    Unknown,
    U8,
    S16,
    S32,
    S64,
    F32,
    F64,
};

// Raw decoded PCM format. This is a media contract, not an output-device
// contract: AudioResampler is responsible for adapting it to AudioSink.
struct AudioPcmFormat {
    std::uint32_t sample_rate = 0;
    std::uint32_t channels = 0;
    AudioSampleFormat sample_format = AudioSampleFormat::Unknown;
    bool planar = false;
};

// Ownership-transferred decoded PCM. An interleaved frame has one plane;
// a planar frame has one plane per channel. Each plane contains complete
// samples only. Timestamps use the media timeline in microseconds.
struct DecodedAudio {
    AudioPcmFormat format;
    std::uint32_t samples_per_channel = 0;
    std::vector<std::vector<std::byte>> planes;
    std::optional<std::int64_t> pts_us;
};

struct SubtitleCodecConfig {
    CodecCommon common;
};

enum class OtherStreamKind : std::uint8_t {
    Data,
    Attachment,
    Unknown,
};

struct OtherStreamConfig {
    CodecCommon common;
    OtherStreamKind kind = OtherStreamKind::Unknown;
};

using StreamConfig = std::variant<VideoCodecConfig, AudioCodecConfig, SubtitleCodecConfig,
                                  OtherStreamConfig>;

struct StreamDescriptor {
    DemuxerStreamId id;
    StreamTiming timing;
    StreamConfig config;
};

struct ContainerInfo {
    std::optional<std::int64_t> duration_us;
};

} // namespace semi::contracts::media
