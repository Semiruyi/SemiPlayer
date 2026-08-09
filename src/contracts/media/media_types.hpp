#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
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
    Rgba8,
};

struct VideoPlane {
    std::vector<std::byte> bytes;
    std::uint32_t stride_bytes = 0;
};

// Ownership-transferred decoded video. The MVP currently produces one Rgba8
// plane, but the plane-based representation leaves room for native formats
// such as NV12 and P010 later.
struct DecodedVideo {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    VideoPixelFormat pixel_format = VideoPixelFormat::Unknown;
    std::vector<VideoPlane> planes;
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
