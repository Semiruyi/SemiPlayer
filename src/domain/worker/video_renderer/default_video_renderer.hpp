#pragma once

#include "contracts/video_renderer/video_renderer_backend.hpp"
#include "domain/resource/generation/generation.hpp"
#include "domain/resource/video_frame_store/video_frame_store_events.hpp"
#include "domain/resource/video_frame_store/video_frame_source.hpp"
#include "domain/resource/video_rendered_store/video_rendered_store_events.hpp"
#include "domain/resource/video_rendered_store/video_rendered_store_sink.hpp"
#include "domain/worker/video_renderer/video_renderer.hpp"
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

class DefaultVideoRenderer final : public VideoRenderer {
public:
    DefaultVideoRenderer(std::shared_ptr<VideoFrameSource> video_frame_source,
                         std::shared_ptr<VideoRenderedSink> video_rendered_sink,
                         std::shared_ptr<VideoRendererBackend> backend,
                         std::shared_ptr<infra::Notifier> notifier,
                         std::shared_ptr<Generation> generation);
    ~DefaultVideoRenderer() override;

    DefaultVideoRenderer(const DefaultVideoRenderer&) = delete;
    DefaultVideoRenderer& operator=(const DefaultVideoRenderer&) = delete;
    DefaultVideoRenderer(DefaultVideoRenderer&&) = delete;
    DefaultVideoRenderer& operator=(DefaultVideoRenderer&&) = delete;

    [[nodiscard]] std::expected<void, VideoRendererError>
    configure(const VideoRendererOptions& options) override;

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
        VideoRendererOptions options;
        std::promise<std::expected<void, VideoRendererError>> completion;
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
    void handle_input_item(VideoFrameStoreItem item) noexcept;
    void handle_video_frame(VideoFrame frame, Generation::Value current_generation) noexcept;
    void handle_end_of_input(Generation::Value generation) noexcept;
    void store_rendered_output(contracts::media::RenderedVideo rendered,
                               Generation::Value generation) noexcept;
    void handle_backend_failure(VideoRendererBackendError error) noexcept;
    void notify_backend_failure(VideoRendererBackendError error) noexcept;

    [[nodiscard]] bool transition_worker_locked(WorkerEvent event) noexcept;
    [[nodiscard]] bool transition_session_locked(SessionEvent event) noexcept;

    std::shared_ptr<VideoFrameSource> video_frame_source_;
    std::shared_ptr<VideoRenderedSink> video_rendered_sink_;
    std::shared_ptr<VideoRendererBackend> backend_;
    std::shared_ptr<infra::Notifier> notifier_;
    std::shared_ptr<Generation> generation_;

    std::shared_ptr<infra::Notifier::Subscription> video_frame_store_not_empty_subscription_;
    std::shared_ptr<infra::Notifier::Subscription> video_rendered_store_not_full_subscription_;
    std::shared_ptr<infra::Notifier::Subscription> generation_changed_subscription_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    WorkerState worker_state_ = WorkerState::Starting;
    SessionState session_state_ = SessionState::Constructed;
    std::deque<ControlCommand> commands_;

    std::deque<VideoRenderedStoreItem> pending_outputs_;
    Generation::Value active_generation_ = 0;
    bool input_exhausted_ = false;
    bool input_not_empty_hint_ = false;
    bool output_not_full_hint_ = false;
};

} // namespace semi::domain
