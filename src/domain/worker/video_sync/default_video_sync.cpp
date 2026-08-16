#include "domain/worker/video_sync/default_video_sync.hpp"

#include "domain/resource/generation/generation_events.hpp"
#include "infrastructure/log/log.hpp"

#include <cassert>
#include <chrono>
#include <exception>
#include <string>
#include <utility>
#include <variant>

#define SEMI_LOG_TAG "video_sync"

namespace semi::domain {
namespace {

void require_state_transition(bool succeeded) noexcept {
    assert(succeeded);
    if (!succeeded) [[unlikely]] {
        std::terminate();
    }
}

VideoSyncError invalid_state(std::string message) {
    return VideoSyncError{
        .code = VideoSyncErrorCode::InvalidState,
        .message = std::move(message),
    };
}

VideoSyncError internal_error(std::string message) {
    return VideoSyncError{
        .code = VideoSyncErrorCode::Internal,
        .message = std::move(message),
    };
}

} // namespace

DefaultVideoSync::DefaultVideoSync(
    std::shared_ptr<VideoRenderedSource> video_rendered_source,
    std::shared_ptr<AudioOutput> audio_output,
    std::shared_ptr<infra::Notifier> notifier,
    std::shared_ptr<Generation> generation,
    std::shared_ptr<VideoSyncTelemetry> telemetry)
    : notifier_(std::move(notifier)),
      generation_(std::move(generation)),
      telemetry_(telemetry ? std::move(telemetry)
                           : std::make_shared<NullVideoSyncTelemetry>()),
      input_(std::move(video_rendered_source)),
      clock_(std::move(audio_output)),
      worker_([this] {
          worker_main();
      }) {
    if (!notifier_) {
        return;
    }

    video_rendered_store_not_empty_subscription_ =
        notifier_->subscribe<VideoRenderedStoreNotEmpty>(
            [this](const VideoRenderedStoreNotEmpty&) {
                input_.mark_available();
                cv_.notify_one();
            });
    generation_changed_subscription_ = notifier_->subscribe<GenerationChanged>(
        [this](const GenerationChanged&) {
            {
                std::lock_guard lock(mutex_);
                generation_changed_hint_ = true;
                audio_position_ready_hint_ = false;
            }
            input_.mark_available();
            cv_.notify_one();
        });
    audio_position_ready_subscription_ = notifier_->subscribe<AudioPlaybackPositionReady>(
        [this](const AudioPlaybackPositionReady& event) {
            {
                std::lock_guard lock(mutex_);
                if (event.generation == active_generation_ || active_generation_ == 0) {
                    audio_position_ready_hint_ = true;
                }
            }
            cv_.notify_one();
        });
    audio_playback_finished_subscription_ = notifier_->subscribe<AudioPlaybackFinished>(
        [this](const AudioPlaybackFinished& event) {
            {
                std::lock_guard lock(mutex_);
                if (event.generation != active_generation_) {
                    return;
                }
                audio_playback_finished_hint_ = true;
            }
            cv_.notify_one();
        });
}

DefaultVideoSync::~DefaultVideoSync() {
    shutdown_worker();
    video_rendered_store_not_empty_subscription_.reset();
    generation_changed_subscription_.reset();
    audio_position_ready_subscription_.reset();
    audio_playback_finished_subscription_.reset();
}

std::expected<void, VideoSyncError>
DefaultVideoSync::configure(const VideoSyncOptions& options) {
    ConfigureCommand command;
    command.options = options;
    auto completion = command.completion.get_future();
    {
        std::lock_guard lock(mutex_);
        commands_.emplace_back(std::move(command));
    }
    cv_.notify_one();
    return completion.get();
}

std::expected<void, VideoSyncError> DefaultVideoSync::start_playback() {
    StartPlaybackCommand command;
    auto completion = command.completion.get_future();
    {
        std::lock_guard lock(mutex_);
        commands_.emplace_back(std::move(command));
    }
    cv_.notify_one();
    return completion.get();
}

std::expected<void, VideoSyncError> DefaultVideoSync::pause_playback() {
    PausePlaybackCommand command;
    auto completion = command.completion.get_future();
    {
        std::lock_guard lock(mutex_);
        commands_.emplace_back(std::move(command));
    }
    cv_.notify_one();
    return completion.get();
}

void DefaultVideoSync::unconfigure() noexcept {
    UnconfigureCommand command;
    auto completion = command.completion.get_future();
    {
        std::lock_guard lock(mutex_);
        commands_.emplace_back(std::move(command));
    }
    cv_.notify_one();
    completion.wait();
}

void DefaultVideoSync::worker_main() noexcept {
    std::unique_lock lock(mutex_);
    if (worker_state_ == WorkerState::Starting) {
        require_state_transition(transition_worker_locked(WorkerEvent::Started));
    }
    cv_.notify_all();

    for (;;) {
        if (worker_state_ == WorkerState::ShuttingDown) {
            break;
        }

        if (!commands_.empty()) {
            ControlCommand command = std::move(commands_.front());
            commands_.pop_front();
            lock.unlock();
            std::visit([this](auto& value) { process_command(value); }, command);
            lock.lock();
            continue;
        }

        if (should_process_data_locked()) {
            lock.unlock();
            process_data_step();
            lock.lock();
            continue;
        }

        const auto predicate = [this] {
            return worker_state_ == WorkerState::ShuttingDown || !commands_.empty() ||
                   should_process_data_locked();
        };
        if (const auto next_wake_deadline = scheduler_.next_wake_deadline()) {
            cv_.wait_until(lock, *next_wake_deadline, predicate);
        } else {
            cv_.wait(lock, predicate);
        }
    }

    require_state_transition(transition_worker_locked(WorkerEvent::Stopped));
    cv_.notify_all();
}

void DefaultVideoSync::shutdown_worker() noexcept {
    {
        std::lock_guard lock(mutex_);
        if (worker_state_ == WorkerState::Stopped) {
            return;
        }
        require_state_transition(transition_worker_locked(WorkerEvent::ShutdownRequested));
    }
    cv_.notify_one();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void DefaultVideoSync::process_command(ConfigureCommand& command) noexcept {
    {
        std::lock_guard lock(mutex_);
        const bool requested = transition_session_locked(SessionEvent::ConfigureRequested);
        if (!requested) {
            command.completion.set_value(
                std::unexpected(invalid_state("video sync is already configured")));
            return;
        }
    }

    if (!input_.has_source() || !notifier_ || !generation_ ||
        (command.options.audio_master && !clock_.has_audio_output())) {
        std::lock_guard lock(mutex_);
        require_state_transition(transition_session_locked(SessionEvent::ConfigureFailed));
        command.completion.set_value(
            std::unexpected(internal_error("video sync dependencies are unavailable")));
        return;
    }

    Generation::Value configured_generation = 0;
    {
        std::lock_guard lock(mutex_);
        options_ = command.options;
        active_generation_ = generation_->current();
        configured_generation = active_generation_;
        playback_enabled_ = false;
        generation_changed_hint_ = false;
        audio_position_ready_hint_ = false;
        audio_playback_finished_hint_ = false;
        playback_finished_notified_ = false;
        input_.reset();
        input_.mark_available();
        clock_.configure(options_.audio_master, active_generation_);
        scheduler_.reset();
        require_state_transition(transition_session_locked(SessionEvent::ConfigureSucceeded));
        command.completion.set_value(std::expected<void, VideoSyncError>{});
    }
    telemetry_->on_session_started(configured_generation);
}

void DefaultVideoSync::process_command(StartPlaybackCommand& command) noexcept {
    {
        std::lock_guard lock(mutex_);
        if (session_state_ != SessionState::Configured) {
            command.completion.set_value(
                std::unexpected(invalid_state("video sync is not configured")));
            return;
        }

        playback_enabled_ = true;
        input_.mark_available();
        scheduler_.on_playback_started();
        clock_.resume();
        command.completion.set_value(std::expected<void, VideoSyncError>{});
    }
    telemetry_->on_playback_started();
}

void DefaultVideoSync::process_command(PausePlaybackCommand& command) noexcept {
    std::lock_guard lock(mutex_);
    if (session_state_ != SessionState::Configured) {
        command.completion.set_value(
            std::unexpected(invalid_state("video sync is not configured")));
        return;
    }

    if (playback_enabled_) {
        clock_.pause();
    }
    playback_enabled_ = false;
    scheduler_.on_playback_paused();
    command.completion.set_value(std::expected<void, VideoSyncError>{});
}

void DefaultVideoSync::process_command(UnconfigureCommand& command) noexcept {
    {
        std::lock_guard lock(mutex_);
        if (session_state_ == SessionState::Constructed) {
            command.completion.set_value();
            return;
        }

        require_state_transition(transition_session_locked(SessionEvent::UnconfigureRequested));
        playback_enabled_ = false;
        generation_changed_hint_ = false;
        audio_position_ready_hint_ = false;
        audio_playback_finished_hint_ = false;
        playback_finished_notified_ = false;
        active_generation_ = 0;
        input_.reset();
        scheduler_.reset();
        clock_.reset();
        require_state_transition(transition_session_locked(SessionEvent::UnconfigureSucceeded));
    }
    telemetry_->on_session_finished("unconfigure");
    command.completion.set_value();
}

bool DefaultVideoSync::should_process_data_locked() const noexcept {
    if (session_state_ != SessionState::Configured) {
        return false;
    }

    if (generation_changed_hint_ || (generation_ && generation_->current() != active_generation_)) {
        return true;
    }

    if (audio_playback_finished_hint_) {
        return true;
    }

    if (scheduler_.has_pending_frame()) {
        if (audio_position_ready_hint_) {
            return true;
        }
        if (scheduler_.waiting_for_audio_position()) {
            return audio_position_ready_hint_;
        }
        if (scheduler_.waiting_for_resume()) {
            return playback_enabled_;
        }
        if (const auto next_wake_deadline = scheduler_.next_wake_deadline()) {
            return Clock::now() >= *next_wake_deadline;
        }
        return true;
    }

    if (input_.end_of_input_observed()) {
        return false;
    }

    if (!playback_enabled_ && !scheduler_.paused_generation_pending()) {
        return false;
    }

    return input_.has_available_hint();
}

void DefaultVideoSync::process_data_step() noexcept {
    adopt_generation_if_needed();
    adopt_audio_playback_finished_if_needed();

    if (!begin_data_step()) {
        return;
    }

    auto result = scheduler_.step(input_, clock_, active_generation_, playback_enabled_);
    record_schedule_observations(result);
    if (result.frame) {
        present_frame(std::move(*result.frame), result.presentation_clock_pts_us);
        scheduler_.on_frame_presented(playback_enabled_);
    }
    notify_playback_finished_if_needed();
}

bool DefaultVideoSync::begin_data_step() noexcept {
    std::lock_guard lock(mutex_);
    if (session_state_ != SessionState::Configured ||
        (!playback_enabled_ && !scheduler_.paused_generation_pending())) {
        return false;
    }
    if (audio_position_ready_hint_) {
        audio_position_ready_hint_ = false;
        scheduler_.on_audio_position_ready();
    }
    return true;
}

void DefaultVideoSync::adopt_generation_if_needed() noexcept {
    const auto current_generation = generation_ ? generation_->current() : 0;
    std::lock_guard lock(mutex_);
    if (session_state_ != SessionState::Configured ||
        (!generation_changed_hint_ && current_generation == active_generation_)) {
        return;
    }

    audio_position_ready_hint_ = false;
    audio_playback_finished_hint_ = false;
    playback_finished_notified_ = false;
    active_generation_ = current_generation;
    generation_changed_hint_ = false;
    input_.reset();
    input_.mark_available();
    clock_.on_generation_changed(active_generation_);
    scheduler_.on_generation_changed(playback_enabled_);
}

void DefaultVideoSync::adopt_audio_playback_finished_if_needed() noexcept {
    std::lock_guard lock(mutex_);
    if (session_state_ != SessionState::Configured ||
        !audio_playback_finished_hint_ || clock_.audio_playback_finished()) {
        return;
    }

    audio_playback_finished_hint_ = false;
    clock_.on_audio_playback_finished(playback_enabled_);
    scheduler_.on_audio_playback_finished();
    input_.mark_available();
}

void DefaultVideoSync::record_schedule_observations(
    const VideoSyncScheduleResult& result) noexcept {
    for (std::uint64_t index = 0; index < result.stale_items_dropped; ++index) {
        telemetry_->on_stale_item_dropped();
    }
    for (std::uint64_t index = 0; index < result.frames_popped; ++index) {
        telemetry_->on_frame_popped();
    }
    if (result.empty_pop) {
        telemetry_->on_empty_pop();
    }
    if (result.audio_clock_unavailable) {
        telemetry_->on_audio_clock_unavailable();
    }
    if (result.wait_scheduled) {
        telemetry_->on_wait_scheduled(result.wait_target_us);
    }
    if (result.wait_overshoot_observed) {
        telemetry_->on_wait_overshoot(result.wait_overshoot_us);
    }
    for (std::uint64_t index = 0; index < result.catchup_drops; ++index) {
        telemetry_->on_frame_dropped_for_catchup();
    }
}

void DefaultVideoSync::present_frame(
    RenderedVideoFrame&& frame,
    std::optional<std::int64_t> clock_pts) noexcept {
    if (!options_.on_frame) {
        telemetry_->on_frame_presented(VideoSyncPresentationObservation{
            .frame_pts_us = frame.rendered().pts_us,
            .clock_pts_us = clock_pts,
        });
        return;
    }
    const auto callback_started_at = Clock::now();
    try {
        options_.on_frame(frame);
    } catch (...) {
        SEMI_LOG_ERROR("video frame callback threw an exception");
    }
    const auto callback_duration_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now() - callback_started_at)
            .count());
    telemetry_->on_frame_presented(VideoSyncPresentationObservation{
        .frame_pts_us = frame.rendered().pts_us,
        .clock_pts_us = clock_pts,
        .callback_duration_us = callback_duration_us,
    });
}

void DefaultVideoSync::notify_playback_finished_if_needed() noexcept {
    Generation::Value generation = 0;
    {
        std::lock_guard lock(mutex_);
        if (session_state_ != SessionState::Configured ||
            !input_.end_of_input_observed() ||
            playback_finished_notified_) {
            return;
        }
        playback_finished_notified_ = true;
        generation = active_generation_;
    }

    try {
        if (notifier_) {
            (void)notifier_->send(VideoPlaybackFinished{.generation = generation});
        }
    } catch (...) {
        SEMI_LOG_ERROR("failed to publish video playback finished");
    }
}

bool DefaultVideoSync::transition_worker_locked(WorkerEvent event) noexcept {
    switch (event) {
    case WorkerEvent::Started:
        if (worker_state_ == WorkerState::Starting) {
            worker_state_ = WorkerState::Alive;
            return true;
        }
        return false;
    case WorkerEvent::ShutdownRequested:
        if (worker_state_ == WorkerState::Starting || worker_state_ == WorkerState::Alive) {
            worker_state_ = WorkerState::ShuttingDown;
            return true;
        }
        return false;
    case WorkerEvent::Stopped:
        if (worker_state_ == WorkerState::ShuttingDown) {
            worker_state_ = WorkerState::Stopped;
            return true;
        }
        return false;
    }
    return false;
}

bool DefaultVideoSync::transition_session_locked(SessionEvent event) noexcept {
    switch (event) {
    case SessionEvent::ConfigureRequested:
        if (session_state_ == SessionState::Constructed) {
            session_state_ = SessionState::Configuring;
            return true;
        }
        return false;
    case SessionEvent::ConfigureSucceeded:
        if (session_state_ == SessionState::Configuring) {
            session_state_ = SessionState::Configured;
            return true;
        }
        return false;
    case SessionEvent::ConfigureFailed:
        if (session_state_ == SessionState::Configuring) {
            session_state_ = SessionState::Constructed;
            return true;
        }
        return false;
    case SessionEvent::UnconfigureRequested:
        if (session_state_ == SessionState::Configured) {
            session_state_ = SessionState::Unconfiguring;
            return true;
        }
        return false;
    case SessionEvent::UnconfigureSucceeded:
        if (session_state_ == SessionState::Unconfiguring) {
            session_state_ = SessionState::Constructed;
            return true;
        }
        return false;
    }
    return false;
}

} // namespace semi::domain
