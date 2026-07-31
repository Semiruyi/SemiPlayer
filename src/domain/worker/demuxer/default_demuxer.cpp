#include "domain/worker/demuxer/default_demuxer.hpp"

#include <cassert>
#include <concepts>
#include <variant>
#include <utility>

namespace semi::domain {
namespace {

DemuxerError command_handling_not_implemented() {
    return DemuxerError{
        .code = DemuxerErrorCode::InvalidState,
        .message = "default demuxer command handling is not implemented",
        .backend_error = std::nullopt,
    };
}

DemuxerError backend_failure(DemuxerBackendError error) {
    return DemuxerError{
        .code = DemuxerErrorCode::BackendFailure,
        .message = error.message,
        .backend_error = std::move(error),
    };
}

DemuxerOpenResult select_default_streams(BackendProbeResult probe) {
    DemuxerOpenResult result;
    result.container = std::move(probe.container);
    for (const StreamDescriptor& stream : probe.streams) {
        std::visit(
            [&result, &stream](const auto& config) {
                using Config = std::decay_t<decltype(config)>;
                if constexpr (std::same_as<Config, VideoCodecConfig>) {
                    if (!result.video) {
                        result.video = SelectedStream<VideoCodecConfig>{stream.id, stream.timing,
                                                                         config};
                    }
                } else if constexpr (std::same_as<Config, AudioCodecConfig>) {
                    if (!result.audio) {
                        result.audio = SelectedStream<AudioCodecConfig>{stream.id, stream.timing,
                                                                         config};
                    }
                } else if constexpr (std::same_as<Config, SubtitleCodecConfig>) {
                    if (!result.subtitle) {
                        result.subtitle = SelectedStream<SubtitleCodecConfig>{stream.id,
                                                                               stream.timing,
                                                                               config};
                    }
                }
            },
            stream.config);
    }
    return result;
}

} // namespace

DefaultDemuxer::DefaultDemuxer(std::shared_ptr<DemuxerBackend> backend,
                               std::shared_ptr<AudioPacketSink> audio_packet_sink,
                               std::shared_ptr<infra::Notifier> notifier,
                               std::shared_ptr<Generation> generation)
    : backend_(std::move(backend)),
      audio_packet_sink_(std::move(audio_packet_sink)),
      notifier_(std::move(notifier)),
      generation_(std::move(generation)),
      worker_([this] {
          worker_main();
      }) {}

DefaultDemuxer::~DefaultDemuxer() {
    shutdown_worker();
}

std::expected<DemuxerOpenResult, DemuxerError>
DefaultDemuxer::open(std::string_view source) {
    OpenCommand command;
    command.source = source;
    auto completion = command.completion.get_future();
    {
        std::lock_guard lock(mutex_);
        commands_.emplace_back(std::move(command));
    }
    cv_.notify_one();
    return completion.get();
}

std::expected<void, DemuxerError> DefaultDemuxer::seek(std::int64_t position_us) {
    SeekCommand command;
    command.position_us = position_us;
    auto completion = command.completion.get_future();
    {
        std::lock_guard lock(mutex_);
        commands_.emplace_back(std::move(command));
    }
    cv_.notify_one();
    return completion.get();
}

void DefaultDemuxer::close() noexcept {
    CloseCommand command;
    auto completion = command.completion.get_future();
    {
        std::lock_guard lock(mutex_);
        commands_.emplace_back(std::move(command));
    }
    cv_.notify_one();
    completion.wait();
}

void DefaultDemuxer::worker_main() noexcept {
    std::unique_lock lock(mutex_);
    if (worker_state_ == WorkerState::Starting) {
        const bool started = transition_worker_locked(WorkerEvent::Started);
        assert(started);
    }
    cv_.notify_all();
    for (;;) {
        cv_.wait(lock, [this] {
            return worker_state_ == WorkerState::ShuttingDown || !commands_.empty();
        });
        if (worker_state_ == WorkerState::ShuttingDown) {
            break;
        }
        ControlCommand command = std::move(commands_.front());
        commands_.pop_front();
        lock.unlock();
        std::visit([this](auto& value) { process_command(value); }, command);
        lock.lock();
    }
    const bool stopped = transition_worker_locked(WorkerEvent::Stopped);
    assert(stopped);
    cv_.notify_all();
}

void DefaultDemuxer::shutdown_worker() noexcept {
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

void DefaultDemuxer::process_command(OpenCommand& command) noexcept {
    std::shared_ptr<DemuxerBackend> backend;
    {
        std::lock_guard lock(mutex_);
        const bool requested = transition_session_locked(SessionEvent::OpenRequested);
        if (!requested) {
            command.completion.set_value(std::unexpected(DemuxerError{
                .code = DemuxerErrorCode::InvalidState,
                .message = "demuxer session is not closed",
                .backend_error = std::nullopt,
            }));
            return;
        }
        backend = backend_;
    }

    if (!backend || !generation_) {
        std::lock_guard lock(mutex_);
        const bool failed = transition_session_locked(SessionEvent::OpenFailed);
        assert(failed);
        command.completion.set_value(std::unexpected(DemuxerError{
            .code = DemuxerErrorCode::InvalidState,
            .message = "demuxer dependencies are unavailable",
            .backend_error = std::nullopt,
        }));
        return;
    }

    auto probe = backend->open(command.source);
    if (!probe) {
        backend->close();
        std::lock_guard lock(mutex_);
        const bool failed = transition_session_locked(SessionEvent::OpenFailed);
        assert(failed);
        command.completion.set_value(std::unexpected(backend_failure(std::move(probe.error()))));
        return;
    }

    auto result = select_default_streams(std::move(*probe));
    generation_->bump();
    {
        std::lock_guard lock(mutex_);
        audio_stream_id_ = result.audio ? std::optional{result.audio->id} : std::nullopt;
        const bool opened = transition_session_locked(SessionEvent::OpenSucceeded);
        assert(opened);
    }
    command.completion.set_value(std::move(result));
}

void DefaultDemuxer::process_command(SeekCommand& command) noexcept {
    std::lock_guard lock(mutex_);
    if (session_state_ != SessionState::Running &&
        session_state_ != SessionState::Exhausted) {
        command.completion.set_value(std::unexpected(command_handling_not_implemented()));
        return;
    }
    command.completion.set_value(std::unexpected(command_handling_not_implemented()));
}

void DefaultDemuxer::process_command(CloseCommand& command) noexcept {
    std::shared_ptr<DemuxerBackend> backend;
    {
        std::lock_guard lock(mutex_);
        if (session_state_ != SessionState::Closed) {
            const bool requested = transition_session_locked(SessionEvent::CloseRequested);
            assert(requested);
            backend = backend_;
        }
    }
    if (backend) {
        backend->close();
    }
    {
        std::lock_guard lock(mutex_);
        audio_stream_id_.reset();
        if (session_state_ == SessionState::Closing) {
            const bool closed = transition_session_locked(SessionEvent::Closed);
            assert(closed);
        }
    }
    command.completion.set_value();
}

bool DefaultDemuxer::transition_worker_locked(WorkerEvent event) noexcept {
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

bool DefaultDemuxer::transition_session_locked(SessionEvent event) noexcept {
    switch (event) {
    case SessionEvent::OpenRequested:
        if (session_state_ == SessionState::Closed) {
            session_state_ = SessionState::Opening;
            return true;
        }
        return false;
    case SessionEvent::OpenSucceeded:
        if (session_state_ == SessionState::Opening) {
            session_state_ = SessionState::Running;
            return true;
        }
        return false;
    case SessionEvent::OpenFailed:
        if (session_state_ == SessionState::Opening) {
            session_state_ = SessionState::Closed;
            return true;
        }
        return false;
    case SessionEvent::SeekSucceeded:
        if (session_state_ == SessionState::Running ||
            session_state_ == SessionState::Exhausted) {
            session_state_ = SessionState::Running;
            return true;
        }
        return false;
    case SessionEvent::SeekFailed:
        return session_state_ == SessionState::Running ||
               session_state_ == SessionState::Exhausted;
    case SessionEvent::CloseRequested:
        if (session_state_ != SessionState::Closed &&
            session_state_ != SessionState::Closing) {
            session_state_ = SessionState::Closing;
            return true;
        }
        return session_state_ == SessionState::Closing;
    case SessionEvent::Closed:
        if (session_state_ == SessionState::Closing) {
            session_state_ = SessionState::Closed;
            return true;
        }
        return false;
    case SessionEvent::InputExhausted:
        if (session_state_ == SessionState::Running) {
            session_state_ = SessionState::Exhausted;
            return true;
        }
        return false;
    case SessionEvent::BackendFailed:
        if (session_state_ == SessionState::Running) {
            session_state_ = SessionState::Failed;
            return true;
        }
        return false;
    }
    return false;
}

} // namespace semi::domain
