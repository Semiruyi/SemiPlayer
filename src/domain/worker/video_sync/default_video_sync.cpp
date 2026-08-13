#include "domain/worker/video_sync/default_video_sync.hpp"

#include "domain/resource/generation/generation_events.hpp"
#include "infrastructure/log/log.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <string>
#include <utility>
#include <variant>

#define SEMI_LOG_TAG "video_sync"

namespace semi::domain {
namespace {

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
    std::shared_ptr<Generation> generation)
    : video_rendered_source_(std::move(video_rendered_source)),
      audio_output_(std::move(audio_output)),
      notifier_(std::move(notifier)),
      generation_(std::move(generation)),
      worker_([this] {
          worker_main();
      }) {
    if (!notifier_) {
        return;
    }

    video_rendered_store_not_empty_subscription_ =
        notifier_->subscribe<VideoRenderedStoreNotEmpty>(
            [this](const VideoRenderedStoreNotEmpty&) {
                {
                    std::lock_guard lock(mutex_);
                    input_not_empty_hint_ = true;
                }
                cv_.notify_one();
            });
    generation_changed_subscription_ = notifier_->subscribe<GenerationChanged>(
        [this](const GenerationChanged&) {
            {
                std::lock_guard lock(mutex_);
                generation_changed_hint_ = true;
                audio_position_ready_hint_ = false;
            }
            cv_.notify_one();
        });
    audio_position_ready_subscription_ = notifier_->subscribe<AudioPlaybackPositionReady>(
        [this](const AudioPlaybackPositionReady& event) {
            {
                std::lock_guard lock(mutex_);
                if (event.generation == active_generation_ || active_generation_ == 0) {
                    audio_position_ready_hint_ = true;
                    waiting_for_audio_position_ = false;
                }
            }
            cv_.notify_one();
        });
}

DefaultVideoSync::~DefaultVideoSync() {
    shutdown_worker();
    video_rendered_store_not_empty_subscription_.reset();
    generation_changed_subscription_.reset();
    audio_position_ready_subscription_.reset();
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
        const bool started = transition_worker_locked(WorkerEvent::Started);
        assert(started);
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
        if (next_wake_deadline_) {
            cv_.wait_until(lock, *next_wake_deadline_, predicate);
        } else {
            cv_.wait(lock, predicate);
        }
    }

    const bool stopped = transition_worker_locked(WorkerEvent::Stopped);
    assert(stopped);
    cv_.notify_all();
}

void DefaultVideoSync::shutdown_worker() noexcept {
    {
        std::lock_guard lock(mutex_);
        if (worker_state_ == WorkerState::Stopped) {
            return;
        }
        const bool requested = transition_worker_locked(WorkerEvent::ShutdownRequested);
        assert(requested);
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

    if (!video_rendered_source_ || !notifier_ || !generation_ ||
        (command.options.audio_master && !audio_output_)) {
        std::lock_guard lock(mutex_);
        const bool failed = transition_session_locked(SessionEvent::ConfigureFailed);
        assert(failed);
        command.completion.set_value(
            std::unexpected(internal_error("video sync dependencies are unavailable")));
        return;
    }

    {
        std::lock_guard lock(mutex_);
        options_ = command.options;
        active_generation_ = generation_->current();
        playback_enabled_ = false;
        paused_generation_pending_ = false;
        input_not_empty_hint_ = true;
        generation_changed_hint_ = false;
        audio_position_ready_hint_ = false;
        waiting_for_audio_position_ = false;
        waiting_for_resume_ = false;
        end_of_input_observed_ = false;
        pending_frame_.reset();
        next_wake_deadline_.reset();
        reset_local_clock();
        const bool succeeded = transition_session_locked(SessionEvent::ConfigureSucceeded);
        assert(succeeded);
        command.completion.set_value(std::expected<void, VideoSyncError>{});
    }
}

void DefaultVideoSync::process_command(StartPlaybackCommand& command) noexcept {
    std::lock_guard lock(mutex_);
    if (session_state_ != SessionState::Configured) {
        command.completion.set_value(
            std::unexpected(invalid_state("video sync is not configured")));
        return;
    }

    playback_enabled_ = true;
    paused_generation_pending_ = false;
    input_not_empty_hint_ = true;
    waiting_for_audio_position_ = false;
    waiting_for_resume_ = false;
    next_wake_deadline_.reset();
    resume_local_clock();
    command.completion.set_value(std::expected<void, VideoSyncError>{});
}

void DefaultVideoSync::process_command(PausePlaybackCommand& command) noexcept {
    std::lock_guard lock(mutex_);
    if (session_state_ != SessionState::Configured) {
        command.completion.set_value(
            std::unexpected(invalid_state("video sync is not configured")));
        return;
    }

    if (playback_enabled_) {
        pause_local_clock();
    }
    playback_enabled_ = false;
    waiting_for_audio_position_ = false;
    waiting_for_resume_ = false;
    next_wake_deadline_.reset();
    command.completion.set_value(std::expected<void, VideoSyncError>{});
}

void DefaultVideoSync::process_command(UnconfigureCommand& command) noexcept {
    std::lock_guard lock(mutex_);
    if (session_state_ == SessionState::Constructed) {
        command.completion.set_value();
        return;
    }

    const bool requested = transition_session_locked(SessionEvent::UnconfigureRequested);
    assert(requested);
    playback_enabled_ = false;
    paused_generation_pending_ = false;
    input_not_empty_hint_ = false;
    generation_changed_hint_ = false;
    audio_position_ready_hint_ = false;
    waiting_for_audio_position_ = false;
    waiting_for_resume_ = false;
    end_of_input_observed_ = false;
    pending_frame_.reset();
    next_wake_deadline_.reset();
    active_generation_ = 0;
    reset_local_clock();
    const bool succeeded = transition_session_locked(SessionEvent::UnconfigureSucceeded);
    assert(succeeded);
    command.completion.set_value();
}

bool DefaultVideoSync::should_process_data_locked() const noexcept {
    if (session_state_ != SessionState::Configured) {
        return false;
    }

    if (generation_changed_hint_ || (generation_ && generation_->current() != active_generation_)) {
        return true;
    }

    if (pending_frame_) {
        if (audio_position_ready_hint_) {
            return true;
        }
        if (waiting_for_audio_position_) {
            return audio_position_ready_hint_;
        }
        if (waiting_for_resume_) {
            return playback_enabled_;
        }
        if (next_wake_deadline_) {
            return Clock::now() >= *next_wake_deadline_;
        }
        return true;
    }

    if (end_of_input_observed_) {
        return false;
    }

    if (!playback_enabled_ && !paused_generation_pending_) {
        return false;
    }

    return input_not_empty_hint_;
}

void DefaultVideoSync::process_data_step() noexcept {
    adopt_generation_if_needed();

    if (!begin_data_step()) {
        return;
    }

    auto clock_pts = current_clock_pts();
    if (wait_for_audio_clock_if_needed(clock_pts)) {
        return;
    }

    auto presentation = collect_due_presentation(clock_pts);
    if (presentation.frame) {
        present_frame(std::move(*presentation.frame));
        if (!playback_enabled_) {
            paused_generation_pending_ = false;
        }
    }
}

bool DefaultVideoSync::begin_data_step() noexcept {
    std::lock_guard lock(mutex_);
    if (session_state_ != SessionState::Configured ||
        (!playback_enabled_ && !paused_generation_pending_)) {
        return false;
    }
    if (audio_position_ready_hint_) {
        audio_position_ready_hint_ = false;
        waiting_for_audio_position_ = false;
    }
    return true;
}

bool DefaultVideoSync::wait_for_audio_clock_if_needed(
    const std::optional<std::int64_t>& clock_pts) noexcept {
    if (!options_.audio_master || clock_pts) {
        return false;
    }

    if (!pending_frame_) {
        VideoRenderedStoreItem item = RenderedVideoEndOfInput{};
        if (!pop_next_item(item)) {
            return true;
        }
        if (video_rendered_store_item_generation(item) !=
            (generation_ ? generation_->current() : 0)) {
            adopt_generation_if_needed();
            return true;
        }
        if (auto* frame = std::get_if<RenderedVideoFrame>(&item)) {
            pending_frame_.emplace(std::move(*frame));
        } else {
            end_of_input_observed_ = true;
            return true;
        }
    }

    std::lock_guard lock(mutex_);
    waiting_for_audio_position_ = true;
    next_wake_deadline_.reset();
    return true;
}

DefaultVideoSync::DataStepPresentation DefaultVideoSync::collect_due_presentation(
    std::optional<std::int64_t>& clock_pts) noexcept {
    std::optional<RenderedVideoFrame> candidate;
    DataStepPresentation presentation;
    if (!take_pending_frame_if_due(clock_pts, candidate)) {
        return presentation;
    }

    const bool can_drain_multiple = playback_enabled_;
    if (!candidate || can_drain_multiple) {
        for (;;) {
            VideoRenderedStoreItem item = RenderedVideoEndOfInput{};
            if (!pop_next_item(item)) {
                break;
            }

            const auto current_generation = generation_ ? generation_->current() : 0;
            if (video_rendered_store_item_generation(item) != current_generation) {
                adopt_generation_if_needed();
                continue;
            }

            if (auto* frame = std::get_if<RenderedVideoFrame>(&item)) {
                if (!frame_is_due(*frame, clock_pts)) {
                    pending_frame_.emplace(std::move(*frame));
                    break;
                }

                candidate.emplace(std::move(*frame));
                if (!can_drain_multiple) {
                    break;
                }
                continue;
            }

            end_of_input_observed_ = true;
            break;
        }
    }

    presentation.frame = std::move(candidate);
    return presentation;
}

bool DefaultVideoSync::take_pending_frame_if_due(
    std::optional<std::int64_t>& clock_pts,
    std::optional<RenderedVideoFrame>& candidate) noexcept {
    if (!pending_frame_) {
        return true;
    }

    if (!frame_is_due(*pending_frame_, clock_pts)) {
        return false;
    }

    candidate.emplace(std::move(*pending_frame_));
    pending_frame_.reset();
    next_wake_deadline_.reset();
    return true;
}

bool DefaultVideoSync::frame_is_due(const RenderedVideoFrame& frame,
                                    std::optional<std::int64_t>& clock_pts) noexcept {
    // A paused clock cannot advance to a post-seek frame whose PTS is slightly
    // later than the first prepared audio PTS. Once the new generation has a
    // valid clock, present its first video frame immediately and pause again.
    if (!playback_enabled_ && paused_generation_pending_) {
        return true;
    }

    const auto frame_pts = frame.rendered().pts_us;
    if (!clock_pts && !options_.audio_master && frame_pts) {
        anchor_local_clock_if_needed(*frame_pts);
        clock_pts = current_clock_pts();
    }
    if (!frame_pts || !clock_pts || *frame_pts <= *clock_pts) {
        return true;
    }

    schedule_frame_wait(*frame_pts, *clock_pts);
    return false;
}

void DefaultVideoSync::schedule_frame_wait(std::int64_t frame_pts,
                                           std::int64_t clock_pts) noexcept {
    std::lock_guard lock(mutex_);
    if (!playback_enabled_) {
        waiting_for_resume_ = true;
        next_wake_deadline_.reset();
        return;
    }

    waiting_for_resume_ = false;
    waiting_for_audio_position_ = false;
    next_wake_deadline_ =
        Clock::now() + std::chrono::microseconds(frame_pts - clock_pts);
}

void DefaultVideoSync::adopt_generation_if_needed() noexcept {
    const auto current_generation = generation_ ? generation_->current() : 0;
    std::lock_guard lock(mutex_);
    if (session_state_ != SessionState::Configured ||
        (!generation_changed_hint_ && current_generation == active_generation_)) {
        return;
    }

    pending_frame_.reset();
    next_wake_deadline_.reset();
    waiting_for_audio_position_ = false;
    waiting_for_resume_ = false;
    audio_position_ready_hint_ = false;
    end_of_input_observed_ = false;
    active_generation_ = current_generation;
    generation_changed_hint_ = false;
    input_not_empty_hint_ = true;
    paused_generation_pending_ = !playback_enabled_;
    reset_local_clock();
}

bool DefaultVideoSync::pop_next_item(VideoRenderedStoreItem& item) noexcept {
    std::shared_ptr<VideoRenderedSource> source;
    {
        std::lock_guard lock(mutex_);
        if (session_state_ != SessionState::Configured ||
            (!playback_enabled_ && !paused_generation_pending_) || !input_not_empty_hint_) {
            return false;
        }
        input_not_empty_hint_ = false;
        source = video_rendered_source_;
    }

    assert(source);
    auto popped = source->try_pop();
    if (!popped) {
        return false;
    }

    {
        std::lock_guard lock(mutex_);
        if (session_state_ == SessionState::Configured && !end_of_input_observed_) {
            input_not_empty_hint_ = true;
        }
    }
    item = std::move(*popped);
    return true;
}

std::optional<std::int64_t> DefaultVideoSync::current_clock_pts() const noexcept {
    if (options_.audio_master) {
        if (!audio_output_) {
            return std::nullopt;
        }
        const auto position = audio_output_->current_position();
        if (!position || position->generation != active_generation_) {
            return std::nullopt;
        }
        return position->pts_us;
    }

    if (local_clock_paused_) {
        return local_clock_frozen_pts_us_;
    }
    if (!local_clock_started_at_) {
        return std::nullopt;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now() - *local_clock_started_at_);
    return local_clock_start_pts_us_ + elapsed.count();
}

void DefaultVideoSync::anchor_local_clock_if_needed(std::int64_t pts_us) noexcept {
    if (local_clock_started_at_ || local_clock_frozen_pts_us_) {
        return;
    }
    local_clock_start_pts_us_ = pts_us;
    local_clock_started_at_ = Clock::now();
    if (!playback_enabled_) {
        local_clock_frozen_pts_us_ = pts_us;
    }
}

void DefaultVideoSync::pause_local_clock() noexcept {
    if (const auto current = current_clock_pts(); current && !options_.audio_master) {
        local_clock_frozen_pts_us_ = *current;
    }
    local_clock_paused_ = true;
}

void DefaultVideoSync::resume_local_clock() noexcept {
    if (options_.audio_master) {
        return;
    }
    if (local_clock_frozen_pts_us_) {
        local_clock_start_pts_us_ = *local_clock_frozen_pts_us_;
        local_clock_started_at_ = Clock::now();
        local_clock_frozen_pts_us_.reset();
    }
    local_clock_paused_ = false;
}

void DefaultVideoSync::reset_local_clock() noexcept {
    local_clock_started_at_.reset();
    local_clock_start_pts_us_ = 0;
    local_clock_frozen_pts_us_.reset();
    local_clock_paused_ = true;
}

void DefaultVideoSync::present_frame(RenderedVideoFrame&& frame) noexcept {
    if (!options_.on_frame) {
        return;
    }
    try {
        options_.on_frame(frame);
    } catch (...) {
        SEMI_LOG_ERROR("video frame callback threw an exception");
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
