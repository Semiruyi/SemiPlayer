#include "domain/worker/video_renderer/default_video_renderer.hpp"

#include "domain/resource/generation/generation_events.hpp"
#include "domain/resource/video_frame_store/video_frame_store_item.hpp"
#include "domain/worker/video_renderer/video_renderer_events.hpp"

#include <cassert>
#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace semi::domain {
namespace {

void require_state_transition(bool succeeded) noexcept {
    assert(succeeded);
    if (!succeeded) [[unlikely]] {
        std::terminate();
    }
}

VideoRendererError invalid_state(std::string message) {
    return VideoRendererError{
        .code = VideoRendererErrorCode::InvalidState,
        .message = std::move(message),
        .backend_error = std::nullopt,
    };
}

VideoRendererBackendError backend_exception(VideoRendererBackendOperation operation,
                                            std::string message) {
    return VideoRendererBackendError{
        .operation = operation,
        .native_code = 0,
        .message = std::move(message),
    };
}

} // namespace

DefaultVideoRenderer::DefaultVideoRenderer(
    std::shared_ptr<VideoFrameSource> video_frame_source,
    std::shared_ptr<VideoRenderedSink> video_rendered_sink,
    std::shared_ptr<VideoRendererBackend> backend,
    std::shared_ptr<infra::Notifier> notifier,
    std::shared_ptr<Generation> generation)
    : video_frame_source_(std::move(video_frame_source)),
      video_rendered_sink_(std::move(video_rendered_sink)),
      backend_(std::move(backend)),
      notifier_(std::move(notifier)),
      generation_(std::move(generation)),
      worker_([this] {
          worker_main();
      }) {
    if (!notifier_) {
        return;
    }

    video_frame_store_not_empty_subscription_ = notifier_->subscribe<VideoFrameStoreNotEmpty>(
        [this](const VideoFrameStoreNotEmpty&) {
            {
                std::lock_guard lock(mutex_);
                input_not_empty_hint_ = true;
            }
            cv_.notify_one();
        });
    video_rendered_store_not_full_subscription_ =
        notifier_->subscribe<VideoRenderedStoreNotFull>(
            [this](const VideoRenderedStoreNotFull&) {
                {
                    std::lock_guard lock(mutex_);
                    output_not_full_hint_ = true;
                }
                cv_.notify_one();
            });
    generation_changed_subscription_ = notifier_->subscribe<GenerationChanged>(
        [this](const GenerationChanged&) {
            cv_.notify_one();
        });
}

DefaultVideoRenderer::~DefaultVideoRenderer() {
    shutdown_worker();
    video_frame_store_not_empty_subscription_.reset();
    video_rendered_store_not_full_subscription_.reset();
    generation_changed_subscription_.reset();
}

std::expected<void, VideoRendererError>
DefaultVideoRenderer::configure(const VideoRendererOptions& options) {
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

void DefaultVideoRenderer::unconfigure() noexcept {
    UnconfigureCommand command;
    auto completion = command.completion.get_future();
    {
        std::lock_guard lock(mutex_);
        commands_.emplace_back(std::move(command));
    }
    cv_.notify_one();
    completion.wait();
}

void DefaultVideoRenderer::worker_main() noexcept {
    std::unique_lock lock(mutex_);
    if (worker_state_ == WorkerState::Starting) {
        require_state_transition(transition_worker_locked(WorkerEvent::Started));
    }
    cv_.notify_all();
    for (;;) {
        cv_.wait(lock, [this] {
            return worker_state_ == WorkerState::ShuttingDown || !commands_.empty() ||
                   should_process_data_locked();
        });
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
            adopt_generation_if_needed(generation_ ? generation_->current() : 0);
            if (try_push_pending_output() == PendingOutputPushResult::NoPending) {
                read_next_input_to_pending();
            }
            lock.lock();
        }
    }
    require_state_transition(transition_worker_locked(WorkerEvent::Stopped));
    cv_.notify_all();
}

void DefaultVideoRenderer::shutdown_worker() noexcept {
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

void DefaultVideoRenderer::process_command(ConfigureCommand& command) noexcept {
    {
        std::lock_guard lock(mutex_);
        const bool requested = transition_session_locked(SessionEvent::ConfigureRequested);
        if (!requested) {
            command.completion.set_value(
                std::unexpected(invalid_state("video renderer is already configured")));
            return;
        }
    }

    if (!backend_ || !video_frame_source_ || !video_rendered_sink_ || !notifier_ || !generation_) {
        std::lock_guard lock(mutex_);
        require_state_transition(transition_session_locked(SessionEvent::ConfigureFailed));
        command.completion.set_value(
            std::unexpected(invalid_state("video renderer dependencies are unavailable")));
        return;
    }

    std::expected<void, VideoRendererBackendError> configured;
    try {
        configured = backend_->configure(command.options);
    } catch (...) {
        configured = std::unexpected(backend_exception(
            VideoRendererBackendOperation::Configure,
            "video renderer backend configuration threw an exception"));
    }
    if (!configured) {
        backend_->unconfigure();
        std::lock_guard lock(mutex_);
        require_state_transition(transition_session_locked(SessionEvent::ConfigureFailed));
        command.completion.set_value(std::unexpected(VideoRendererError{
            .code = VideoRendererErrorCode::BackendFailure,
            .message = configured.error().message,
            .backend_error = std::move(configured.error()),
        }));
        return;
    }

    std::lock_guard lock(mutex_);
    active_generation_ = generation_->current();
    pending_outputs_.clear();
    input_exhausted_ = false;
    input_not_empty_hint_ = true;
    output_not_full_hint_ = true;
    require_state_transition(transition_session_locked(SessionEvent::ConfigureSucceeded));
    command.completion.set_value(std::expected<void, VideoRendererError>{});
}

void DefaultVideoRenderer::process_command(UnconfigureCommand& command) noexcept {
    {
        std::lock_guard lock(mutex_);
        if (session_state_ == SessionState::Constructed) {
            command.completion.set_value();
            return;
        }
        require_state_transition(transition_session_locked(SessionEvent::UnconfigureRequested));
    }

    if (backend_) {
        backend_->unconfigure();
    }

    std::lock_guard lock(mutex_);
    pending_outputs_.clear();
    active_generation_ = 0;
    input_exhausted_ = false;
    input_not_empty_hint_ = false;
    output_not_full_hint_ = false;
    require_state_transition(transition_session_locked(SessionEvent::UnconfigureSucceeded));
    command.completion.set_value();
}

bool DefaultVideoRenderer::should_process_data_locked() const noexcept {
    if (session_state_ != SessionState::Configured) {
        return false;
    }

    if (generation_ && active_generation_ != generation_->current()) {
        return true;
    }

    if (!pending_outputs_.empty()) {
        return output_not_full_hint_;
    }

    if (input_exhausted_ &&
        (!generation_ || active_generation_ == generation_->current())) {
        return false;
    }

    return input_not_empty_hint_;
}

void DefaultVideoRenderer::adopt_generation_if_needed(
    Generation::Value current_generation) noexcept {
    bool generation_changed = false;
    {
        std::lock_guard lock(mutex_);
        if (session_state_ != SessionState::Configured || current_generation == active_generation_) {
            return;
        }

        pending_outputs_.clear();
        input_exhausted_ = false;
        active_generation_ = current_generation;
        input_not_empty_hint_ = true;
        output_not_full_hint_ = false;
        generation_changed = true;
    }

    if (generation_changed && backend_) {
        backend_->reset();
    }
}

DefaultVideoRenderer::PendingOutputPushResult
DefaultVideoRenderer::try_push_pending_output() noexcept {
    std::shared_ptr<VideoRenderedSink> video_rendered_sink;
    std::optional<VideoRenderedStoreItem> pending_output;

    {
        std::lock_guard lock(mutex_);
        if (session_state_ != SessionState::Configured) {
            return PendingOutputPushResult::Handled;
        }
        if (pending_outputs_.empty()) {
            return PendingOutputPushResult::NoPending;
        }
        if (!output_not_full_hint_) {
            return PendingOutputPushResult::Handled;
        }

        output_not_full_hint_ = false;
        pending_output.emplace(std::move(pending_outputs_.front()));
        pending_outputs_.pop_front();
        video_rendered_sink = video_rendered_sink_;
    }

    assert(video_rendered_sink);
    const auto pushed = video_rendered_sink->try_push(std::move(*pending_output));

    std::lock_guard lock(mutex_);
    if (worker_state_ == WorkerState::ShuttingDown ||
        session_state_ != SessionState::Configured) {
        return PendingOutputPushResult::Handled;
    }

    if (pushed == VideoRenderedPushResult::Full) {
        pending_outputs_.push_front(std::move(*pending_output));
        return PendingOutputPushResult::Handled;
    }

    if (!pending_outputs_.empty()) {
        output_not_full_hint_ = true;
    } else if (!input_exhausted_) {
        input_not_empty_hint_ = true;
    }
    return PendingOutputPushResult::Handled;
}

void DefaultVideoRenderer::read_next_input_to_pending() noexcept {
    std::shared_ptr<VideoFrameSource> video_frame_source;
    {
        std::lock_guard lock(mutex_);
        if (session_state_ != SessionState::Configured ||
            (input_exhausted_ &&
             (!generation_ || active_generation_ == generation_->current())) ||
            !pending_outputs_.empty()) {
            return;
        }
        if (!input_not_empty_hint_) {
            return;
        }

        input_not_empty_hint_ = false;
        video_frame_source = video_frame_source_;
    }

    assert(video_frame_source);
    auto item = video_frame_source->try_pop();
    if (!item) {
        return;
    }

    handle_input_item(std::move(*item));
}

void DefaultVideoRenderer::handle_input_item(VideoFrameStoreItem item) noexcept {
    const Generation::Value current_generation = generation_ ? generation_->current() : 0;
    adopt_generation_if_needed(current_generation);

    {
        std::lock_guard lock(mutex_);
        if (session_state_ != SessionState::Configured) {
            return;
        }
    }

    if (video_frame_store_item_generation(item) != current_generation) {
        std::lock_guard lock(mutex_);
        if (session_state_ == SessionState::Configured && !input_exhausted_) {
            input_not_empty_hint_ = true;
        }
        return;
    }

    if (auto* frame = std::get_if<VideoFrame>(&item)) {
        handle_video_frame(std::move(*frame), current_generation);
        return;
    }

    handle_end_of_input(current_generation);
}

void DefaultVideoRenderer::handle_video_frame(
    VideoFrame frame, Generation::Value current_generation) noexcept {
    std::shared_ptr<VideoRendererBackend> backend;
    {
        std::lock_guard lock(mutex_);
        if (session_state_ != SessionState::Configured) {
            return;
        }
        backend = backend_;
    }

    assert(backend);
    std::expected<contracts::media::RenderedVideo, VideoRendererBackendError> rendered;
    try {
        rendered = backend->render(frame.decoded());
    } catch (...) {
        rendered = std::unexpected(backend_exception(
            VideoRendererBackendOperation::Render,
            "video renderer backend render threw an exception"));
    }

    if (!rendered) {
        handle_backend_failure(std::move(rendered.error()));
        return;
    }

    store_rendered_output(std::move(*rendered), current_generation);
}

void DefaultVideoRenderer::handle_end_of_input(Generation::Value generation) noexcept {
    std::lock_guard lock(mutex_);
    if (session_state_ != SessionState::Configured || active_generation_ != generation) {
        return;
    }

    input_exhausted_ = true;
    pending_outputs_.emplace_back(RenderedVideoEndOfInput{.generation = generation});
    output_not_full_hint_ = true;
}

void DefaultVideoRenderer::store_rendered_output(
    contracts::media::RenderedVideo rendered,
    Generation::Value generation) noexcept {
    std::lock_guard lock(mutex_);
    if (worker_state_ == WorkerState::ShuttingDown ||
        session_state_ != SessionState::Configured || active_generation_ != generation) {
        return;
    }

    pending_outputs_.emplace_back(
        std::in_place_type<RenderedVideoFrame>, std::move(rendered), generation);
    output_not_full_hint_ = true;
}

void DefaultVideoRenderer::handle_backend_failure(VideoRendererBackendError error) noexcept {
    bool should_notify = false;
    {
        std::lock_guard lock(mutex_);
        if (worker_state_ != WorkerState::ShuttingDown &&
            session_state_ == SessionState::Configured) {
            pending_outputs_.clear();
            input_not_empty_hint_ = false;
            output_not_full_hint_ = false;
            require_state_transition(transition_session_locked(SessionEvent::BackendFailed));
            should_notify = true;
        }
    }

    if (should_notify) {
        notify_backend_failure(std::move(error));
    }
}

void DefaultVideoRenderer::notify_backend_failure(VideoRendererBackendError error) noexcept {
    if (!notifier_) {
        return;
    }

    try {
        VideoRendererBackendFailure event{.error = std::move(error)};
        (void)notifier_->send(event);
    } catch (...) {
        // Backend failure is already reflected in the session state.
    }
}

bool DefaultVideoRenderer::transition_worker_locked(WorkerEvent event) noexcept {
    switch (event) {
    case WorkerEvent::Started:
        if (worker_state_ == WorkerState::Starting) {
            worker_state_ = WorkerState::Alive;
            return true;
        }
        return false;
    case WorkerEvent::ShutdownRequested:
        if (worker_state_ == WorkerState::Starting ||
            worker_state_ == WorkerState::Alive) {
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

bool DefaultVideoRenderer::transition_session_locked(SessionEvent event) noexcept {
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
        if (session_state_ == SessionState::Configured ||
            session_state_ == SessionState::Failed) {
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
    case SessionEvent::BackendFailed:
        if (session_state_ == SessionState::Configured) {
            session_state_ = SessionState::Failed;
            return true;
        }
        return false;
    }
    return false;
}

} // namespace semi::domain
