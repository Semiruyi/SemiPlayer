#include "domain/worker/demuxer/default_demuxer.hpp"

#include <cassert>
#include <concepts>
#include <exception>
#include <variant>
#include <utility>

namespace semi::domain {
namespace {

template <typename... Functions>
struct Overloaded : Functions... {
    using Functions::operator()...;
};

template <typename... Functions>
Overloaded(Functions...) -> Overloaded<Functions...>;

DemuxerError backend_failure(DemuxerBackendError error) {
    DemuxerError result;
    result.code = DemuxerErrorCode::BackendFailure;
    result.message = error.message;
    result.backend_error = std::move(error);
    return result;
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
                        result.video = SelectedStream<VideoCodecConfig>{stream.id, stream.timing, config};
                    }
                } else if constexpr (std::same_as<Config, AudioCodecConfig>) {
                    if (!result.audio) {
                        result.audio = SelectedStream<AudioCodecConfig>{stream.id, stream.timing, config};
                    }
                } else if constexpr (std::same_as<Config, SubtitleCodecConfig>) {
                    if (!result.subtitle) {
                        result.subtitle = SelectedStream<SubtitleCodecConfig>{stream.id, stream.timing, config};
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
      generation_(std::move(generation)) {
    if (notifier_) {
        audio_queue_not_full_subscription_ = notifier_->subscribe<AudioQueueNotFull>(
            [this](const AudioQueueNotFull&) {
                queue_not_full_hint_.store(true, std::memory_order_release);
                cv_.notify_one();
            });
    }
}

DefaultDemuxer::~DefaultDemuxer() {
    close();
    audio_queue_not_full_subscription_.reset();
}

std::expected<DemuxerOpenResult, DemuxerError> DefaultDemuxer::open(std::string_view source) {
    std::unique_lock lock(mutex_);
    if (state_ != State::Closed) {
        return std::unexpected(DemuxerError{
            .code = DemuxerErrorCode::InvalidState,
            .message = "demuxer is already open",
            .backend_error = std::nullopt,
        });
    }
    if (!backend_) {
        return std::unexpected(DemuxerError{
            .code = DemuxerErrorCode::BackendFailure,
            .message = "demuxer backend is unavailable",
            .backend_error = std::nullopt,
        });
    }
    if (!audio_packet_sink_) {
        return std::unexpected(DemuxerError{
            .code = DemuxerErrorCode::InvalidState,
            .message = "audio packet sink is unavailable",
            .backend_error = std::nullopt,
        });
    }
    if (!generation_) {
        return std::unexpected(DemuxerError{
            .code = DemuxerErrorCode::InvalidState,
            .message = "generation is unavailable",
            .backend_error = std::nullopt,
        });
    }

    auto probe = backend_->open(source);
    if (!probe) {
        backend_->close();
        return std::unexpected(backend_failure(std::move(probe.error())));
    }

    auto result = select_default_streams(std::move(*probe));
    generation_->bump();
    if (result.audio) {
        audio_stream_id_ = result.audio->id;
    } else {
        audio_stream_id_.reset();
    }

    const bool opened = transition_locked(Event::OpenSucceeded);
    assert(opened);
    queue_not_full_hint_.store(false, std::memory_order_release);
    pending_seek_position_us_.reset();
    return result;
}

std::expected<void, DemuxerError> DefaultDemuxer::start() {
    std::unique_lock lock(mutex_);
    if (state_ == State::Closed) {
        return std::unexpected(DemuxerError{
            .code = DemuxerErrorCode::InvalidState,
            .message = "demuxer must be open before starting",
            .backend_error = std::nullopt,
        });
    }

    if (state_ == State::Reading) {
        return {};
    }
    if (state_ == State::Stopping) {
        return std::unexpected(DemuxerError{
            .code = DemuxerErrorCode::InvalidState,
            .message = "demuxer is stopping",
            .backend_error = std::nullopt,
        });
    }
    if (state_ == State::Exhausted) {
        return std::unexpected(DemuxerError{
            .code = DemuxerErrorCode::InvalidState,
            .message = "demuxer input is exhausted; reopen before starting again",
            .backend_error = std::nullopt,
        });
    }
    if (state_ == State::Failed) {
        return std::unexpected(DemuxerError{
            .code = DemuxerErrorCode::InvalidState,
            .message = "demuxer read failed; reopen before starting again",
            .backend_error = std::nullopt,
        });
    }
    if (state_ == State::Stopped) {
        return std::unexpected(DemuxerError{
            .code = DemuxerErrorCode::InvalidState,
            .message = "demuxer was stopped; reopen before starting again",
            .backend_error = std::nullopt,
        });
    }

    const bool started = transition_locked(Event::StartRequested);
    assert(started);
    if (!worker_running_) {
        if (worker_.joinable()) {
            lock.unlock();
            worker_.join();
            lock.lock();
            if (state_ == State::Closed) {
                return std::unexpected(DemuxerError{
                    .code = DemuxerErrorCode::InvalidState,
                    .message = "demuxer was closed while restarting its worker",
                    .backend_error = std::nullopt,
                });
            }
        }

        try {
            worker_running_ = true;
            worker_ = std::thread([this] {
                worker_main();
            });
        } catch (...) {
            worker_running_ = false;
            const bool reset_to_ready = transition_locked(Event::WorkerStartFailed);
            assert(reset_to_ready);
            return std::unexpected(DemuxerError{
                .code = DemuxerErrorCode::BackendFailure,
                .message = "failed to start demuxer worker",
                .backend_error = std::nullopt,
            });
        }
    }
    cv_.notify_one();
    return {};
}

void DefaultDemuxer::stop() noexcept {
    std::thread worker;
    {
        std::lock_guard lock(mutex_);
        if (state_ == State::Closed) {
            return;
        }

        if (state_ == State::Reading) {
            const bool stopping = transition_locked(Event::StopRequested);
            assert(stopping);
        } else if (state_ == State::Ready) {
            const bool stopped = transition_locked(Event::StopRequested);
            assert(stopped);
        }

        if (worker_.joinable()) {
            worker = std::move(worker_);
        }
    }

    cv_.notify_one();
    if (worker.joinable()) {
        worker.join();
    }

    std::lock_guard lock(mutex_);
    queue_not_full_hint_.store(false, std::memory_order_release);
}

std::expected<void, DemuxerError> DefaultDemuxer::seek(std::int64_t position_us) {
    std::lock_guard lock(mutex_);
    if (state_ == State::Closed) {
        return std::unexpected(DemuxerError{
            .code = DemuxerErrorCode::InvalidState,
            .message = "demuxer must be open before seeking",
            .backend_error = std::nullopt,
        });
    }

    pending_seek_position_us_ = position_us;
    return {};
}

void DefaultDemuxer::close() noexcept {
    stop();

    std::lock_guard lock(mutex_);
    if (backend_) {
        backend_->close();
    }
    pending_seek_position_us_.reset();
    audio_stream_id_.reset();
    if (state_ != State::Closed) {
        const bool closed = transition_locked(Event::CloseRequested);
        assert(closed);
    }
}

void DefaultDemuxer::worker_main() noexcept {
    try {
        WorkerSession session;
        {
            std::lock_guard lock(mutex_);
            session.generation = generation_->current();
            session.audio_stream_id = audio_stream_id_;
        }

        std::optional<WorkerExit> exit;
        while (!exit) {
            switch (wait_for_work(session.pending_item.has_value())) {
            case WorkAction::Stop:
                exit = WorkerExit::Stopped;
                break;
            case WorkAction::RetryPending:
                exit = retry_pending_item(session);
                break;
            case WorkAction::ReadBackend:
                exit = read_and_route_packet(session);
                break;
            }
        }

        std::lock_guard lock(mutex_);
        complete_worker_locked(*exit);
    } catch (...) {
        notify_read_error(DemuxerBackendError{
            .operation = DemuxerBackendOperation::Read,
            .native_code = 0,
            .message = "demuxer worker failed",
        });
        std::lock_guard lock(mutex_);
        complete_worker_locked(WorkerExit::Failed);
    }
}

DefaultDemuxer::WorkAction DefaultDemuxer::wait_for_work(bool has_pending_item) {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [this, has_pending_item] {
        return state_ == State::Stopping ||
               (state_ == State::Reading &&
                (!has_pending_item || queue_not_full_hint_.load(std::memory_order_acquire)));
    });

    if (state_ == State::Stopping) {
        return WorkAction::Stop;
    }
    if (has_pending_item) {
        queue_not_full_hint_.store(false, std::memory_order_release);
        return WorkAction::RetryPending;
    }
    return WorkAction::ReadBackend;
}

std::optional<DefaultDemuxer::WorkerExit>
DefaultDemuxer::retry_pending_item(WorkerSession& session) {
    auto item = std::move(*session.pending_item);
    session.pending_item.reset();

    const bool is_end_of_input = std::holds_alternative<AudioPacketEndOfInput>(item);
    if (submit_or_defer(session, std::move(item)) == DeliveryResult::Accepted && is_end_of_input) {
        return WorkerExit::Exhausted;
    }
    return std::nullopt;
}

std::optional<DefaultDemuxer::WorkerExit>
DefaultDemuxer::read_and_route_packet(WorkerSession& session) {
    auto read_result = backend_->read_packet();
    {
        std::lock_guard lock(mutex_);
        if (state_ == State::Stopping) {
            return WorkerExit::Stopped;
        }
    }

    if (!read_result) {
        notify_read_error(std::move(read_result.error()));
        return WorkerExit::Failed;
    }

    return std::visit(
        Overloaded{
            [this, &session](BackendEndOfStream) -> std::optional<WorkerExit> {
                if (!session.audio_stream_id) {
                    return WorkerExit::Exhausted;
                }

                AudioPacketQueueItem end_of_input = AudioPacketEndOfInput{
                    .generation = session.generation,
                };
                if (submit_or_defer(session, std::move(end_of_input)) == DeliveryResult::Accepted) {
                    return WorkerExit::Exhausted;
                }
                return std::nullopt;
            },
            [this, &session](BackendPacket&& backend_packet) -> std::optional<WorkerExit> {
                if (!session.audio_stream_id ||
                    backend_packet.stream_id.value != session.audio_stream_id->value) {
                    return std::nullopt;
                }

                AudioPacketQueueItem audio_item{
                    std::in_place_type<AudioPacket>,
                    std::move(backend_packet.packet),
                    session.generation,
                };
                (void)submit_or_defer(session, std::move(audio_item));
                return std::nullopt;
            },
        },
        std::move(*read_result));
}

DefaultDemuxer::DeliveryResult DefaultDemuxer::submit_or_defer(
    WorkerSession& session, AudioPacketQueueItem&& item) {
    if (audio_packet_sink_->try_push(std::move(item)) == AudioPacketPushResult::Accepted) {
        return DeliveryResult::Accepted;
    }

    session.pending_item.emplace(std::move(item));
    return DeliveryResult::Full;
}

bool DefaultDemuxer::transition_locked(Event event) noexcept {
    switch (event) {
    case Event::OpenSucceeded:
        if (state_ == State::Closed) {
            state_ = State::Ready;
            return true;
        }
        return false;
    case Event::StartRequested:
        if (state_ == State::Ready) {
            state_ = State::Reading;
            return true;
        }
        return false;
    case Event::WorkerStartFailed:
        if (state_ == State::Reading) {
            state_ = State::Ready;
            return true;
        }
        return false;
    case Event::StopRequested:
        if (state_ == State::Reading) {
            state_ = State::Stopping;
            return true;
        }
        if (state_ == State::Ready) {
            state_ = State::Stopped;
            return true;
        }
        return false;
    case Event::WorkerStopped:
        if (state_ == State::Stopping) {
            state_ = State::Stopped;
            return true;
        }
        return false;
    case Event::InputExhausted:
        if (state_ == State::Reading) {
            state_ = State::Exhausted;
            return true;
        }
        return false;
    case Event::ReadFailed:
        if (state_ == State::Reading) {
            state_ = State::Failed;
            return true;
        }
        return false;
    case Event::CloseRequested:
        if (state_ != State::Closed) {
            state_ = State::Closed;
            return true;
        }
        return false;
    }
    return false;
}

void DefaultDemuxer::complete_worker_locked(WorkerExit exit) noexcept {
    worker_running_ = false;
    if (state_ == State::Stopping) {
        const bool stopped = transition_locked(Event::WorkerStopped);
        assert(stopped);
        return;
    }

    switch (exit) {
    case WorkerExit::Stopped:
        return;
    case WorkerExit::Exhausted:
        {
            const bool exhausted = transition_locked(Event::InputExhausted);
            assert(exhausted);
        }
        return;
    case WorkerExit::Failed:
        {
            const bool failed = transition_locked(Event::ReadFailed);
            assert(failed);
        }
        return;
    }
}

void DefaultDemuxer::notify_read_error(DemuxerBackendError error) noexcept {
    if (!notifier_) {
        return;
    }

    try {
        (void)notifier_->send(DemuxerReadError{.error = std::move(error)});
    } catch (...) {
    }
}

} // namespace semi::domain
