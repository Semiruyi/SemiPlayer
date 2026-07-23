#pragma once

#include "domain/demuxer/demuxer_backend.hpp"

#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace semi::domain {

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

class DefaultDemuxer final : public Demuxer {
public:
    explicit DefaultDemuxer(std::shared_ptr<DemuxerBackend> backend);
    ~DefaultDemuxer() override;

    [[nodiscard]] std::expected<DemuxerOpenResult, DemuxerError>
    open(std::string_view source) override;

    void close() noexcept override;

private:
    std::shared_ptr<DemuxerBackend> backend_;
    bool opened_ = false;
};

} // namespace semi::domain
