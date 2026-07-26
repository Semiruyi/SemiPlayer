#pragma once

#include "contracts/demuxer/demuxer_backend.hpp"

#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace semi::domain {

using contracts::demuxer::BackendProbeResult;
using contracts::demuxer::DemuxerBackend;
using contracts::demuxer::DemuxerBackendError;
using contracts::demuxer::DemuxerBackendOperation;
using contracts::media::AudioCodecConfig;
using contracts::media::BackendStreamId;
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
    BackendStreamId id;
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

    virtual void close() noexcept = 0;

protected:
    Demuxer() = default;
};

} // namespace semi::domain
