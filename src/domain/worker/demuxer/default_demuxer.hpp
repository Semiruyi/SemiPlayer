#pragma once

#include "domain/resource/audio_packet_queue/audio_packet_sink.hpp"
#include "domain/resource/generation/generation.hpp"
#include "domain/worker/demuxer/demuxer.hpp"
#include "infrastructure/notifier/notifier.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>

namespace semi::domain {

// The command plane and data plane are added incrementally. This first slice
// establishes that the worker belongs to the module lifetime, not a media session.
class DefaultDemuxer final : public Demuxer {
public:
    DefaultDemuxer(std::shared_ptr<DemuxerBackend> backend,
                   std::shared_ptr<AudioPacketSink> audio_packet_sink,
                   std::shared_ptr<infra::Notifier> notifier,
                   std::shared_ptr<Generation> generation);
    ~DefaultDemuxer() override;

    [[nodiscard]] std::expected<DemuxerOpenResult, DemuxerError>
    open(std::string_view source) override;

    [[nodiscard]] std::expected<void, DemuxerError>
    seek(std::int64_t position_us) override;

    void close() noexcept override;

private:
    enum class WorkerState : std::uint8_t {
        Starting,
        Alive,
        ShuttingDown,
        Stopped,
    };

    enum class SessionState : std::uint8_t {
        Closed,
        Opening,
        Running,
        Exhausted,
        Failed,
        Closing,
    };

    enum class WorkerEvent : std::uint8_t {
        Started,
        ShutdownRequested,
        Stopped,
    };

    enum class SessionEvent : std::uint8_t {
        OpenRequested,
        OpenSucceeded,
        OpenFailed,
        SeekSucceeded,
        SeekFailed,
        CloseRequested,
        Closed,
        InputExhausted,
        BackendFailed,
    };

    enum class PendingAudioOutputPushResult : std::uint8_t {
        NoPending,
        Handled,
    };

    struct OpenCommand {
        std::string source;
        std::promise<std::expected<DemuxerOpenResult, DemuxerError>> completion;
    };

    struct SeekCommand {
        std::int64_t position_us = 0;
        std::promise<std::expected<void, DemuxerError>> completion;
    };

    struct CloseCommand {
        std::promise<void> completion;
    };

    using ControlCommand = std::variant<OpenCommand, SeekCommand, CloseCommand>;

    void worker_main() noexcept;
    void shutdown_worker() noexcept;
    void process_command(OpenCommand& command) noexcept;
    void process_command(SeekCommand& command) noexcept;
    void process_command(CloseCommand& command) noexcept;
    [[nodiscard]] bool should_process_data_locked() const noexcept;
    [[nodiscard]] PendingAudioOutputPushResult try_push_pending_audio_output() noexcept;
    void read_next_audio_output_to_pending() noexcept;
    void handle_backend_read_result(contracts::demuxer::BackendReadResult& result,
                                    contracts::media::DemuxerStreamId audio_stream_id,
                                    Generation::Value session_generation) noexcept;
    void store_pending_audio_output(AudioPacketQueueItem output) noexcept;
    void handle_read_error(DemuxerBackendError error) noexcept;
    void notify_read_error(DemuxerBackendError error) noexcept;
    [[nodiscard]] bool transition_worker_locked(WorkerEvent event) noexcept;
    [[nodiscard]] bool transition_session_locked(SessionEvent event) noexcept;

    std::shared_ptr<DemuxerBackend> backend_;
    std::shared_ptr<AudioPacketSink> audio_packet_sink_;
    std::shared_ptr<infra::Notifier> notifier_;
    std::shared_ptr<Generation> generation_;
    std::shared_ptr<infra::Notifier::Subscription> audio_queue_not_full_subscription_;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<ControlCommand> commands_;
    std::thread worker_;
    WorkerState worker_state_ = WorkerState::Starting;
    SessionState session_state_ = SessionState::Closed;
    std::optional<contracts::media::DemuxerStreamId> audio_stream_id_;
    std::optional<AudioPacketQueueItem> pending_audio_output_;
    Generation::Value session_generation_ = 0;
    std::atomic_bool audio_queue_not_full_hint_{false};
};

} // namespace semi::domain
