#pragma once

#include "contracts/demuxer/demuxer_backend.hpp"

#include <memory>

namespace semi::infra::ffmpeg::demuxer {

class FfmpegDemuxerBackend final : public contracts::demuxer::DemuxerBackend {
public:
    FfmpegDemuxerBackend();
    ~FfmpegDemuxerBackend() override;

    [[nodiscard]] std::expected<contracts::demuxer::BackendProbeResult,
                                 contracts::demuxer::DemuxerBackendError>
    open(std::string_view source) override;

    [[nodiscard]] std::expected<contracts::demuxer::BackendReadResult,
                                 contracts::demuxer::DemuxerBackendError>
    read_packet() override;

    [[nodiscard]] std::expected<void, contracts::demuxer::DemuxerBackendError>
    seek(std::int64_t position_us) override;

    void close() noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace semi::infra::ffmpeg::demuxer
