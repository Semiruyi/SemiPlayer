#pragma once

#include "domain/resource/audio_packet_queue/audio_packet_sink.hpp"
#include "domain/resource/audio_packet_queue/audio_packet_queue_events.hpp"
#include "domain/resource/generation/generation.hpp"
#include "domain/worker/demuxer/demuxer_events.hpp"
#include "domain/worker/demuxer/demuxer.hpp"
#include "infrastructure/notifier/notifier.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace semi::domain {

class DefaultDemuxer final : public Demuxer {
public:
    DefaultDemuxer(std::shared_ptr<DemuxerBackend> backend,
                   std::shared_ptr<AudioPacketSink> audio_packet_sink,
                   std::shared_ptr<infra::Notifier> notifier);
    ~DefaultDemuxer() override;

    [[nodiscard]] std::expected<DemuxerOpenResult, DemuxerError>
    open(std::string_view source) override;

    [[nodiscard]] std::expected<void, DemuxerError> start() override;

    void stop() noexcept override;

    [[nodiscard]] std::expected<void, DemuxerError>
    seek(std::int64_t position_us) override;

    void close() noexcept override;

private:
    enum class State : std::uint8_t {
        Constructed,
        Idle,
        Reading,
        Seeking,
        Stopping,
        Stopped,
    };

    void worker_main() noexcept;
    void notify_end_of_stream() noexcept;
    void notify_read_error(DemuxerBackendError error) noexcept;

    std::shared_ptr<DemuxerBackend> backend_;
    std::shared_ptr<AudioPacketSink> audio_packet_sink_;
    std::shared_ptr<infra::Notifier> notifier_;
    std::shared_ptr<infra::Notifier::Subscription> audio_queue_not_full_subscription_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    State state_ = State::Constructed;
    bool stop_requested_ = false;
    bool worker_running_ = false;
    std::atomic_bool queue_not_full_hint_{false};
    std::optional<AudioPacket> pending_audio_packet_;

    std::optional<contracts::media::DemuxerStreamId> audio_stream_id_;
    bool opened_ = false;
    std::optional<std::int64_t> pending_seek_position_us_;
    Generation generation_;
};

} // namespace semi::domain
