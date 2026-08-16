#pragma once

#include "domain/resource/generation/generation.hpp"
#include "domain/resource/video_rendered_store/video_rendered_store_events.hpp"
#include "domain/resource/video_rendered_store/video_rendered_store_source.hpp"
#include "domain/worker/audio_output/audio_output.hpp"
#include "domain/worker/audio_output/audio_output_events.hpp"
#include "domain/worker/video_sync/video_sync.hpp"
#include "domain/worker/video_sync/video_sync_events.hpp"
#include "domain/worker/video_sync/video_sync_telemetry.hpp"
#include "infrastructure/notifier/notifier.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <expected>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <variant>

namespace semi::domain {

class DefaultVideoSync final : public VideoSync {
public:
    DefaultVideoSync(std::shared_ptr<VideoRenderedSource> video_rendered_source,
                     std::shared_ptr<AudioOutput> audio_output,
                     std::shared_ptr<infra::Notifier> notifier,
                     std::shared_ptr<Generation> generation,
                     std::shared_ptr<VideoSyncTelemetry> telemetry = nullptr);
    ~DefaultVideoSync() override;

    DefaultVideoSync(const DefaultVideoSync&) = delete;
    DefaultVideoSync& operator=(const DefaultVideoSync&) = delete;
    DefaultVideoSync(DefaultVideoSync&&) = delete;
    DefaultVideoSync& operator=(DefaultVideoSync&&) = delete;

    [[nodiscard]] std::expected<void, VideoSyncError>
    configure(const VideoSyncOptions& options) override;

    [[nodiscard]] std::expected<void, VideoSyncError>
    start_playback() override;

    [[nodiscard]] std::expected<void, VideoSyncError>
    pause_playback() override;

    void unconfigure() noexcept override;

private:
    using Clock = std::chrono::steady_clock;

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
    };
    enum class SessionEvent : std::uint8_t {
        ConfigureRequested,
        ConfigureSucceeded,
        ConfigureFailed,
        UnconfigureRequested,
        UnconfigureSucceeded,
    };

    struct ConfigureCommand {
        VideoSyncOptions options;
        std::promise<std::expected<void, VideoSyncError>> completion;
    };

    struct StartPlaybackCommand {
        std::promise<std::expected<void, VideoSyncError>> completion;
    };

    struct PausePlaybackCommand {
        std::promise<std::expected<void, VideoSyncError>> completion;
    };

    struct UnconfigureCommand {
        std::promise<void> completion;
    };

    using ControlCommand = std::variant<ConfigureCommand,
                                        StartPlaybackCommand,
                                        PausePlaybackCommand,
                                        UnconfigureCommand>;

    struct DataStepPresentation {
        std::optional<RenderedVideoFrame> frame;
    };

    void worker_main() noexcept;
    void process_command(ConfigureCommand& command) noexcept;
    void process_command(StartPlaybackCommand& command) noexcept;
    void process_command(PausePlaybackCommand& command) noexcept;
    void process_command(UnconfigureCommand& command) noexcept;
    void shutdown_worker() noexcept;

    [[nodiscard]] bool should_process_data_locked() const noexcept;
    void process_data_step() noexcept;
    [[nodiscard]] bool begin_data_step() noexcept;
    [[nodiscard]] bool wait_for_audio_clock_if_needed(
        const std::optional<std::int64_t>& clock_pts) noexcept;
    [[nodiscard]] DataStepPresentation collect_due_presentation(
        std::optional<std::int64_t>& clock_pts) noexcept;
    [[nodiscard]] bool take_pending_frame_if_due(
        std::optional<std::int64_t>& clock_pts,
        std::optional<RenderedVideoFrame>& candidate) noexcept;
    [[nodiscard]] bool frame_is_due(const RenderedVideoFrame& frame,
                                    std::optional<std::int64_t>& clock_pts) noexcept;
    void schedule_frame_wait(std::int64_t frame_pts, std::int64_t clock_pts) noexcept;
    void adopt_generation_if_needed() noexcept;
    void adopt_audio_playback_finished_if_needed() noexcept;
    [[nodiscard]] bool pop_next_item(VideoRenderedStoreItem& item) noexcept;
    [[nodiscard]] std::optional<std::int64_t> current_clock_pts() const noexcept;
    void anchor_local_clock_if_needed(std::int64_t pts_us) noexcept;
    void pause_local_clock() noexcept;
    void resume_local_clock() noexcept;
    void reset_local_clock() noexcept;
    void present_frame(RenderedVideoFrame&& frame,
                       std::optional<std::int64_t> clock_pts) noexcept;
    void notify_playback_finished_if_needed() noexcept;

    [[nodiscard]] bool transition_worker_locked(WorkerEvent event) noexcept;
    [[nodiscard]] bool transition_session_locked(SessionEvent event) noexcept;

    std::shared_ptr<VideoRenderedSource> video_rendered_source_;
    std::shared_ptr<AudioOutput> audio_output_;
    std::shared_ptr<infra::Notifier> notifier_;
    std::shared_ptr<Generation> generation_;
    std::shared_ptr<VideoSyncTelemetry> telemetry_;

    std::shared_ptr<infra::Notifier::Subscription>
        video_rendered_store_not_empty_subscription_;
    std::shared_ptr<infra::Notifier::Subscription> generation_changed_subscription_;
    std::shared_ptr<infra::Notifier::Subscription> audio_position_ready_subscription_;
    std::shared_ptr<infra::Notifier::Subscription> audio_playback_finished_subscription_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    WorkerState worker_state_ = WorkerState::Starting;
    SessionState session_state_ = SessionState::Constructed;
    std::deque<ControlCommand> commands_;

    VideoSyncOptions options_{};
    Generation::Value active_generation_ = 0;
    bool playback_enabled_ = false;
    bool paused_generation_pending_ = false;
    bool input_not_empty_hint_ = false;
    bool generation_changed_hint_ = false;
    bool audio_position_ready_hint_ = false;
    bool audio_playback_finished_hint_ = false;
    bool waiting_for_audio_position_ = false;
    bool waiting_for_resume_ = false;
    bool end_of_input_observed_ = false;
    bool playback_finished_notified_ = false;
    bool audio_playback_finished_ = false;

    std::optional<RenderedVideoFrame> pending_frame_;
    std::optional<Clock::time_point> next_wake_deadline_;

    std::optional<Clock::time_point> local_clock_started_at_;
    std::int64_t local_clock_start_pts_us_ = 0;
    std::optional<std::int64_t> local_clock_frozen_pts_us_;
    bool local_clock_paused_ = true;
};

} // namespace semi::domain
