#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace semi::domain {

struct BackendStreamId {
    std::uint32_t value = 0;
};

struct Rational {
    std::int32_t numerator = 0;
    std::int32_t denominator = 1;
};

struct StreamTiming {
    Rational time_base;
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
    BackendStreamId id;
    StreamTiming timing;
    StreamConfig config;
};

struct ContainerInfo {
    std::optional<std::int64_t> duration_us;
};

struct BackendProbeResult {
    ContainerInfo container;
    std::vector<StreamDescriptor> streams;
};

enum class DemuxerBackendOperation : std::uint8_t {
    Open,
    Probe,
};

struct DemuxerBackendError {
    DemuxerBackendOperation operation = DemuxerBackendOperation::Open;
    int native_code = 0;
    std::string message;
};

class DemuxerBackend {
public:
    virtual ~DemuxerBackend() = default;

    DemuxerBackend(const DemuxerBackend&) = delete;
    DemuxerBackend& operator=(const DemuxerBackend&) = delete;
    DemuxerBackend(DemuxerBackend&&) = delete;
    DemuxerBackend& operator=(DemuxerBackend&&) = delete;

    [[nodiscard]] virtual std::expected<BackendProbeResult, DemuxerBackendError>
    open(std::string_view source) = 0;

    virtual void close() noexcept = 0;

protected:
    DemuxerBackend() = default;
};

} // namespace semi::domain
