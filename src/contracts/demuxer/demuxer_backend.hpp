#pragma once

#include "contracts/demuxer/packet/packet_read_result.hpp"
#include "contracts/media/media_types.hpp"

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace semi::contracts::demuxer {

using media::AudioCodecConfig;
using media::ContainerInfo;
using media::OtherStreamConfig;
using media::TimeBase;
using media::StreamConfig;
using media::StreamDescriptor;
using media::StreamTiming;
using media::SubtitleCodecConfig;
using media::VideoCodecConfig;
using packet::BackendEndOfStream;
using packet::BackendPacket;
using packet::BackendReadResult;

struct BackendProbeResult {
    ContainerInfo container;
    std::vector<StreamDescriptor> streams;
};

enum class DemuxerBackendOperation : std::uint8_t {
    Open,
    Probe,
    Read,
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

    [[nodiscard]] virtual std::expected<BackendReadResult, DemuxerBackendError>
    read_packet() = 0;

    virtual void close() noexcept = 0;

protected:
    DemuxerBackend() = default;
};

} // namespace semi::contracts::demuxer
