#pragma once

#include "contracts/demuxer/demuxer_backend.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace semi::domain {

using contracts::demuxer::BackendProbeResult;
using contracts::demuxer::BackendEndOfStream;
using contracts::demuxer::BackendPacket;
using contracts::demuxer::DemuxerBackend;
using contracts::demuxer::DemuxerBackendError;
using contracts::demuxer::DemuxerBackendOperation;
using contracts::media::AudioCodecConfig;
using contracts::media::CodecCommon;
using contracts::media::ContainerInfo;
using contracts::media::OtherStreamConfig;
using contracts::media::OtherStreamKind;
using contracts::media::StreamConfig;
using contracts::media::StreamDescriptor;
using contracts::media::StreamTiming;
using contracts::media::SubtitleCodecConfig;
using contracts::media::TimeBase;
using contracts::media::VideoCodecConfig;

template <typename Config>
struct SelectedStream {
    contracts::media::DemuxerStreamId id;
    StreamTiming timing;
    Config config;
};

struct DemuxerOpenResult {
    ContainerInfo container;
    std::optional<SelectedStream<VideoCodecConfig>> video;
    std::optional<SelectedStream<AudioCodecConfig>> audio;
    std::optional<SelectedStream<SubtitleCodecConfig>> subtitle;
};

enum class DemuxerErrorCode : std::uint8_t {
    InvalidState,
    BackendFailure,
};

struct DemuxerError {
    DemuxerErrorCode code = DemuxerErrorCode::BackendFailure;
    std::string message;
    std::optional<DemuxerBackendError> backend_error;
};

class Demuxer {
public:
    virtual ~Demuxer() = default;

    Demuxer(const Demuxer&) = delete;
    Demuxer& operator=(const Demuxer&) = delete;
    Demuxer(Demuxer&&) = delete;
    Demuxer& operator=(Demuxer&&) = delete;

    [[nodiscard]] virtual std::expected<DemuxerOpenResult, DemuxerError>
    open(std::string_view source) = 0;

    // Starts packet production after a successful open. The operation is
    // idempotent while the demuxer is already started.
    [[nodiscard]] virtual std::expected<void, DemuxerError> start() = 0;

    // Stops packet production and ends the current media session. Any packet
    // already read but not accepted by the sink may be discarded. Reopen the
    // media before starting another session.
    virtual void stop() noexcept = 0;

    // Records a seek target on the opened media. Actual backend repositioning
    // is not implemented by the current demuxer backend.
    [[nodiscard]] virtual std::expected<void, DemuxerError>
    seek(std::int64_t position_us) = 0;

    virtual void close() noexcept = 0;

protected:
    Demuxer() = default;
};

} // namespace semi::domain
