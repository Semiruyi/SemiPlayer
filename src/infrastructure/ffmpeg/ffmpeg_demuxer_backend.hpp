#pragma once

#include "domain/demuxer/demuxer_backend.hpp"

#include <memory>

namespace semi::infra::ffmpeg {

class FfmpegDemuxerBackend final : public domain::DemuxerBackend {
public:
    FfmpegDemuxerBackend();
    ~FfmpegDemuxerBackend() override;

    [[nodiscard]] std::expected<domain::BackendProbeResult, domain::DemuxerBackendError>
    open(std::string_view source) override;

    void close() noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace semi::infra::ffmpeg
