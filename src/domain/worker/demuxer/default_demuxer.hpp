#pragma once

#include "domain/resource/audio_packet_queue/audio_packet_sink.hpp"
#include "domain/worker/demuxer/demuxer.hpp"

#include <memory>

namespace semi::domain {

class DefaultDemuxer final : public Demuxer {
public:
    DefaultDemuxer(std::shared_ptr<DemuxerBackend> backend,
                   std::shared_ptr<AudioPacketSink> audio_packet_sink);
    ~DefaultDemuxer() override;

    [[nodiscard]] std::expected<DemuxerOpenResult, DemuxerError>
    open(std::string_view source) override;

    [[nodiscard]] std::expected<void, DemuxerError> start() override;

    void stop() noexcept override;

    [[nodiscard]] std::expected<void, DemuxerError>
    seek(std::int64_t position_us) override;

    void close() noexcept override;

private:
    std::shared_ptr<DemuxerBackend> backend_;
    std::shared_ptr<AudioPacketSink> audio_packet_sink_;
    std::optional<contracts::media::DemuxerStreamId> audio_stream_id_;
    bool opened_ = false;
    bool started_ = false;
    std::optional<std::int64_t> pending_seek_position_us_;
};

} // namespace semi::domain
