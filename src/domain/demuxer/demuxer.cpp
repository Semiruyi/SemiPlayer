#include "domain/demuxer/demuxer.hpp"

#include <concepts>
#include <utility>

namespace semi::domain {
namespace {

DemuxerError backend_failure(DemuxerBackendError error) {
    DemuxerError result;
    result.code = DemuxerErrorCode::BackendFailure;
    result.message = error.message;
    result.backend_error = std::move(error);
    return result;
}

DemuxerOpenResult select_default_streams(BackendProbeResult probe) {
    DemuxerOpenResult result;
    result.container = std::move(probe.container);

    for (const StreamDescriptor& stream : probe.streams) {
        std::visit(
            [&result, &stream](const auto& config) {
                using Config = std::decay_t<decltype(config)>;
                if constexpr (std::same_as<Config, VideoCodecConfig>) {
                    if (!result.video) {
                        result.video = SelectedStream<VideoCodecConfig>{stream.id, stream.timing, config};
                    }
                } else if constexpr (std::same_as<Config, AudioCodecConfig>) {
                    if (!result.audio) {
                        result.audio = SelectedStream<AudioCodecConfig>{stream.id, stream.timing, config};
                    }
                } else if constexpr (std::same_as<Config, SubtitleCodecConfig>) {
                    if (!result.subtitle) {
                        result.subtitle = SelectedStream<SubtitleCodecConfig>{stream.id, stream.timing, config};
                    }
                }
            },
            stream.config);
    }
    return result;
}

} // namespace

DefaultDemuxer::DefaultDemuxer(std::shared_ptr<DemuxerBackend> backend) : backend_(std::move(backend)) {}

DefaultDemuxer::~DefaultDemuxer() {
    close();
}

std::expected<DemuxerOpenResult, DemuxerError> DefaultDemuxer::open(std::string_view source) {
    if (opened_) {
        return std::unexpected(DemuxerError{
            .code = DemuxerErrorCode::InvalidState,
            .message = "demuxer is already open",
            .backend_error = std::nullopt,
        });
    }
    if (!backend_) {
        return std::unexpected(DemuxerError{
            .code = DemuxerErrorCode::BackendFailure,
            .message = "demuxer backend is unavailable",
            .backend_error = std::nullopt,
        });
    }

    auto probe = backend_->open(source);
    if (!probe) {
        backend_->close();
        return std::unexpected(backend_failure(std::move(probe.error())));
    }

    opened_ = true;
    return select_default_streams(std::move(*probe));
}

void DefaultDemuxer::close() noexcept {
    if (backend_) {
        backend_->close();
    }
    opened_ = false;
}

} // namespace semi::domain
