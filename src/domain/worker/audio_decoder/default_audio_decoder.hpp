#pragma once

#include "contracts/audio_decoder/audio_decoder_backend.hpp"
#include "domain/resource/audio_frame_store/audio_frame_sink.hpp"
#include "domain/resource/audio_frame_store/audio_frame_store_events.hpp"
#include "domain/resource/audio_packet_queue/audio_packet_queue_events.hpp"
#include "domain/resource/audio_packet_queue/audio_packet_source.hpp"
#include "domain/resource/generation/generation.hpp"
#include "domain/worker/audio_decoder/audio_decoder.hpp"
#include "infrastructure/notifier/notifier.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <expected>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <variant>

namespace semi::domain {

// 命令通道模型（docs/modules/audio_decoder/audio_decoder.md「命令通道」）：
// worker 归属模块生命周期（构造时启动、析构时 join），configure/unconfigure 经
// ControlCommand 队列由 worker 执行，调用方同步等待完成。backend 的全部调用
// 始终由 worker 独占。Step 1 实现控制面；数据面（Configured 后自动消费输入）
// 由后续步骤接入。
class DefaultAudioDecoder final : public AudioDecoder {
public:
    DefaultAudioDecoder(std::shared_ptr<AudioPacketSource> audio_packet_source,
                        std::shared_ptr<AudioFrameSink> audio_frame_sink,
                        std::shared_ptr<AudioDecoderBackend> backend,
                        std::shared_ptr<infra::Notifier> notifier,
                        std::shared_ptr<Generation> generation);
    ~DefaultAudioDecoder() override;

    DefaultAudioDecoder(const DefaultAudioDecoder&) = delete;
    DefaultAudioDecoder& operator=(const DefaultAudioDecoder&) = delete;
    DefaultAudioDecoder(DefaultAudioDecoder&&) = delete;
    DefaultAudioDecoder& operator=(DefaultAudioDecoder&&) = delete;

    [[nodiscard]] std::expected<AudioDecoderConfigureResult, AudioDecoderError>
    configure(const contracts::media::AudioCodecConfig& config) override;

    void unconfigure() noexcept override;

private:
    enum class WorkerState : std::uint8_t {
        Starting,
        Alive,
        ShuttingDown,
        Stopped,
    };
    enum class WorkerEvent : std::uint8_t {
        Started,
        ShutdownRequested,
        Stopped,
    };

    enum class SessionState : std::uint8_t {
        Constructed,
        Configuring,
        Configured,
        Unconfiguring,
        Failed,
    };
    enum class SessionEvent : std::uint8_t {
        ConfigureRequested,
        ConfigureSucceeded,
        ConfigureFailed,
        UnconfigureRequested,
        UnconfigureSucceeded,
        BackendFailed,
    };

    struct ConfigureCommand {
        contracts::media::AudioCodecConfig config;
        std::promise<std::expected<AudioDecoderConfigureResult, AudioDecoderError>> completion;
    };

    struct UnconfigureCommand {
        std::promise<void> completion;
    };

    using ControlCommand = std::variant<ConfigureCommand, UnconfigureCommand>;

    enum class PendingOutputPushResult : std::uint8_t {
        NoPending,
        Handled,
    };

    void worker_main() noexcept;
    void process_command(ConfigureCommand& command) noexcept;
    void process_command(UnconfigureCommand& command) noexcept;
    void shutdown_worker() noexcept;

    [[nodiscard]] bool should_process_data_locked() const noexcept;
    void adopt_generation_if_needed(Generation::Value current_generation) noexcept;
    [[nodiscard]] PendingOutputPushResult try_push_pending_output() noexcept;
    void read_next_input_to_pending() noexcept;
    void handle_input_item(AudioPacketQueueItem item) noexcept;
    void handle_audio_packet(AudioPacket packet, Generation::Value current_generation) noexcept;
    void handle_end_of_input(Generation::Value generation) noexcept;
    void store_decoded_outputs(contracts::audio_decoder::DecodedAudioBatch decoded,
                               Generation::Value generation,
                               bool append_end_of_input) noexcept;
    void handle_backend_failure(AudioDecoderBackendError error) noexcept;
    void notify_backend_failure(AudioDecoderBackendError error) noexcept;

    [[nodiscard]] bool transition_worker_locked(WorkerEvent event) noexcept;
    [[nodiscard]] bool transition_session_locked(SessionEvent event) noexcept;

    std::shared_ptr<AudioPacketSource> audio_packet_source_;
    std::shared_ptr<AudioFrameSink> audio_frame_sink_;
    std::shared_ptr<AudioDecoderBackend> backend_;
    std::shared_ptr<infra::Notifier> notifier_;
    std::shared_ptr<Generation> generation_;

    std::shared_ptr<infra::Notifier::Subscription> audio_queue_not_empty_subscription_;
    std::shared_ptr<infra::Notifier::Subscription> audio_frame_store_not_full_subscription_;
    std::shared_ptr<infra::Notifier::Subscription> generation_changed_subscription_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    WorkerState worker_state_ = WorkerState::Starting;
    SessionState session_state_ = SessionState::Constructed;
    std::deque<ControlCommand> commands_;

    // 数据面成员（后续步骤使用）：configure/unconfigure 时复位，worker 独占。
    std::deque<AudioFrameStoreItem> pending_outputs_;
    Generation::Value active_generation_ = 0;
    bool input_exhausted_ = false;
    bool input_not_empty_hint_ = false;
    bool output_not_full_hint_ = false;
};

} // namespace semi::domain
