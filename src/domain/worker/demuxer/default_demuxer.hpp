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
                   std::shared_ptr<infra::Notifier> notifier,
                   std::shared_ptr<Generation> generation);
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
        Closed,
        Ready,
        Reading,
        Stopping,
        Stopped,
        Exhausted,
        Failed,
    };

    enum class Event : std::uint8_t {
        OpenSucceeded,
        StartRequested,
        WorkerStartFailed,
        StopRequested,
        WorkerStopped,
        InputExhausted,
        ReadFailed,
        CloseRequested,
    };

    enum class WorkAction : std::uint8_t {
        Stop,
        RetryPending,
        ReadBackend,
    };

    enum class DeliveryResult : std::uint8_t {
        Accepted,
        Full,
    };

    enum class WorkerExit : std::uint8_t {
        Stopped,
        Exhausted,
        Failed,
    };

    struct WorkerSession {
        Generation::Value generation = 0;
        std::optional<contracts::media::DemuxerStreamId> audio_stream_id;
        std::optional<AudioPacketQueueItem> pending_item;
    };

    void worker_main() noexcept;
    [[nodiscard]] WorkAction wait_for_work(bool has_pending_item);
    [[nodiscard]] std::optional<WorkerExit> retry_pending_item(WorkerSession& session);
    [[nodiscard]] std::optional<WorkerExit> read_and_route_packet(WorkerSession& session);
    [[nodiscard]] DeliveryResult submit_or_defer(WorkerSession& session,
                                                  AudioPacketQueueItem&& item);
    [[nodiscard]] bool transition_locked(Event event) noexcept;
    void complete_worker_locked(WorkerExit exit) noexcept;
    void notify_read_error(DemuxerBackendError error) noexcept;

    std::shared_ptr<DemuxerBackend> backend_;
    std::shared_ptr<AudioPacketSink> audio_packet_sink_;
    std::shared_ptr<infra::Notifier> notifier_;
    std::shared_ptr<Generation> generation_;
    std::shared_ptr<infra::Notifier::Subscription> audio_queue_not_full_subscription_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    State state_ = State::Closed;
    bool worker_running_ = false;
    std::atomic_bool queue_not_full_hint_{false};

    std::optional<contracts::media::DemuxerStreamId> audio_stream_id_;
    std::optional<std::int64_t> pending_seek_position_us_;
};

} // namespace semi::domain
