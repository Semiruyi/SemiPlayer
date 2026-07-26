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

struct AudioCodecConfig {
    CodecCommon common;
    std::uint32_t sample_rate = 0;
    std::uint32_t channels = 0;
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
