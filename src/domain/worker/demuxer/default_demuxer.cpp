#include "domain/worker/demuxer/default_demuxer.hpp"

#include "domain/resource/audio_packet_queue/audio_packet_queue_events.hpp"
#include "domain/resource/video_packet_queue/video_packet_queue_events.hpp"
#include "domain/worker/demuxer/demuxer_events.hpp"

#include <cassert>
#include <concepts>
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

DemuxerError invalid_command_state() {
    return DemuxerError{
        .code = DemuxerErrorCode::InvalidState,
        .message = "demuxer session does not allow this command",
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
                               std::shared_ptr<Generation> generation,
                               std::shared_ptr<VideoPacketSink> video_packet_sink)
    : backend_(std::move(backend)),
      audio_packet_sink_(std::move(audio_packet_sink)),
      video_packet_sink_(std::move(video_packet_sink)),
      notifier_(std::move(notifier)),
      generation_(std::move(generation)),
      worker_([this] {
          worker_main();
      }) {
    if (!notifier_) {
        return;
    }

    audio_queue_not_full_subscription_ = notifier_->subscribe<AudioQueueNotFull>(
        [this](const AudioQueueNotFull&) {
            audio_queue_not_full_hint_.store(true, std::memory_order_release);
            cv_.notify_one();
        });
    video_queue_not_full_subscription_ = notifier_->subscribe<VideoQueueNotFull>(
        [this](const VideoQueueNotFull&) {
            video_queue_not_full_hint_.store(true, std::memory_order_release);
            cv_.notify_one();
        });
}

DefaultDemuxer::~DefaultDemuxer() {
    shutdown_worker();
    audio_queue_not_full_subscription_.reset();
    video_queue_not_full_subscription_.reset();
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
            if (try_push_pending_output() == PendingOutputPushResult::NoPending) {
                read_next_output_to_pending();
            }
            lock.lock();
        }
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
    if (result.audio && !audio_packet_sink_) {
        backend->close();
        std::lock_guard lock(mutex_);
        const bool failed = transition_session_locked(SessionEvent::OpenFailed);
        assert(failed);
        command.completion.set_value(std::unexpected(DemuxerError{
            .code = DemuxerErrorCode::InvalidState,
            .message = "audio packet sink is unavailable",
            .backend_error = std::nullopt,
        }));
        return;
    }
    generation_->bump();
    const auto session_generation = generation_->current();
    {
        std::lock_guard lock(mutex_);
        audio_stream_id_ = result.audio ? std::optional{result.audio->id} : std::nullopt;
        video_stream_id_ = result.video && video_packet_sink_
                               ? std::optional{result.video->id}
                               : std::nullopt;
        pending_output_.reset();
        session_generation_ = session_generation;
        pending_output_generation_ = session_generation;
        end_of_input_observed_ = false;
        audio_end_of_input_accepted_ = false;
        video_end_of_input_accepted_ = false;
        audio_queue_not_full_hint_.store(false, std::memory_order_release);
        video_queue_not_full_hint_.store(false, std::memory_order_release);
        const bool opened = transition_session_locked(SessionEvent::OpenSucceeded);
        assert(opened);
    }
    command.completion.set_value(std::move(result));
}

void DefaultDemuxer::process_command(SeekCommand& command) noexcept {
    std::shared_ptr<DemuxerBackend> backend;
    {
        std::lock_guard lock(mutex_);
        if (session_state_ != SessionState::Running &&
            session_state_ != SessionState::Exhausted) {
            command.completion.set_value(std::unexpected(invalid_command_state()));
            return;
        }
        backend = backend_;
    }

    if (!backend || !generation_ || command.position_us < 0) {
        command.completion.set_value(std::unexpected(DemuxerError{
            .code = DemuxerErrorCode::InvalidState,
            .message = command.position_us < 0 ? "seek position must not be negative"
                                               : "demuxer dependencies are unavailable",
            .backend_error = std::nullopt,
        }));
        return;
    }

    auto seek_result = backend->seek(command.position_us);
    if (!seek_result) {
        command.completion.set_value(std::unexpected(backend_failure(std::move(seek_result.error()))));
        return;
    }

    generation_->bump();
    const auto session_generation = generation_->current();
    {
        std::lock_guard lock(mutex_);
        pending_output_.reset();
        session_generation_ = session_generation;
        pending_output_generation_ = session_generation;
        end_of_input_observed_ = false;
        audio_end_of_input_accepted_ = false;
        video_end_of_input_accepted_ = false;
        audio_queue_not_full_hint_.store(false, std::memory_order_release);
        video_queue_not_full_hint_.store(false, std::memory_order_release);
        const bool resumed = transition_session_locked(SessionEvent::SeekSucceeded);
        assert(resumed);
    }
    command.completion.set_value({});
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
        video_stream_id_.reset();
        pending_output_.reset();
        session_generation_ = 0;
        pending_output_generation_ = 0;
        end_of_input_observed_ = false;
        audio_end_of_input_accepted_ = false;
        video_end_of_input_accepted_ = false;
        audio_queue_not_full_hint_.store(false, std::memory_order_release);
        video_queue_not_full_hint_.store(false, std::memory_order_release);
        if (session_state_ == SessionState::Closing) {
            const bool closed = transition_session_locked(SessionEvent::Closed);
            assert(closed);
        }
    }
    command.completion.set_value();
}

bool DefaultDemuxer::should_process_data_locked() const noexcept {
    if (session_state_ != SessionState::Running ||
        (!audio_stream_id_ && !video_stream_id_)) {
        return false;
    }

    if (!end_of_input_observed_ && !pending_output_) {
        return true;
    }

    return pending_output_can_be_pushed_locked();
}

bool DefaultDemuxer::pending_output_can_be_pushed_locked() const noexcept {
    if (!pending_output_) {
        return false;
    }
    if (std::holds_alternative<AudioPacketQueueItem>(*pending_output_)) {
        return audio_queue_not_full_hint_.load(std::memory_order_acquire);
    }
    return video_queue_not_full_hint_.load(std::memory_order_acquire);
}

DefaultDemuxer::PendingOutputPushResult
DefaultDemuxer::take_pending_output_for_push(
    std::optional<PendingOutput>& output) noexcept {
    std::lock_guard lock(mutex_);
    if (session_state_ != SessionState::Running) {
        return PendingOutputPushResult::Handled;
    }
    if (!pending_output_) {
        return PendingOutputPushResult::NoPending;
    }

    const bool ready = std::visit(
        Overloaded{
            [this](AudioPacketQueueItem&) {
                return audio_queue_not_full_hint_.exchange(false, std::memory_order_acq_rel);
            },
            [this](VideoPacketQueueItem&) {
                return video_queue_not_full_hint_.exchange(false, std::memory_order_acq_rel);
            },
        },
        *pending_output_);
    if (!ready) {
        return PendingOutputPushResult::Handled;
    }

    output.emplace(std::move(*pending_output_));
    pending_output_.reset();
    return PendingOutputPushResult::Handled;
}

bool DefaultDemuxer::push_pending_output(PendingOutput& output) noexcept {
    return std::visit(
        Overloaded{
            [this](AudioPacketQueueItem& item) {
                assert(audio_packet_sink_);
                return audio_packet_sink_->try_push(std::move(item)) ==
                       AudioPacketPushResult::Full;
            },
            [this](VideoPacketQueueItem& item) {
                assert(video_packet_sink_);
                return video_packet_sink_->try_push(std::move(item)) ==
                       VideoPacketPushResult::Full;
            },
        },
        output);
}

void DefaultDemuxer::complete_pending_output_push(PendingOutput& output,
                                                  bool was_full) noexcept {
    std::lock_guard lock(mutex_);
    if (worker_state_ == WorkerState::ShuttingDown ||
        session_state_ != SessionState::Running) {
        return;
    }

    if (was_full) {
        pending_output_.emplace(std::move(output));
        return;
    }

    std::visit(
        Overloaded{
            [this](AudioPacketQueueItem& item) {
                if (std::holds_alternative<AudioPacketEndOfInput>(item)) {
                    audio_end_of_input_accepted_ = true;
                }
            },
            [this](VideoPacketQueueItem& item) {
                if (std::holds_alternative<VideoPacketEndOfInput>(item)) {
                    video_end_of_input_accepted_ = true;
                }
            },
        },
        output);
    prepare_next_end_of_input_locked();
    maybe_transition_to_exhausted_locked();
}

DefaultDemuxer::PendingOutputPushResult
DefaultDemuxer::try_push_pending_output() noexcept {
    std::optional<PendingOutput> output;
    const auto take_result = take_pending_output_for_push(output);
    if (take_result == PendingOutputPushResult::NoPending || !output) {
        return take_result;
    }

    const bool was_full = push_pending_output(*output);
    complete_pending_output_push(*output, was_full);
    return PendingOutputPushResult::Handled;
}

void DefaultDemuxer::read_next_output_to_pending() noexcept {
    std::shared_ptr<DemuxerBackend> backend;
    std::optional<contracts::media::DemuxerStreamId> audio_stream_id;
    std::optional<contracts::media::DemuxerStreamId> video_stream_id;
    Generation::Value session_generation = 0;
    {
        std::lock_guard lock(mutex_);
        if (session_state_ != SessionState::Running || end_of_input_observed_ ||
            pending_output_ ||
            (!audio_stream_id_ && !video_stream_id_)) {
            return;
        }
        backend = backend_;
        audio_stream_id = audio_stream_id_;
        video_stream_id = video_stream_id_;
        session_generation = session_generation_;
    }

    assert(backend);
    auto read = backend->read_packet();
    if (!read) {
        handle_read_error(std::move(read.error()));
        return;
    }

    handle_backend_read_result(*read, audio_stream_id, video_stream_id, session_generation);
}

void DefaultDemuxer::handle_backend_read_result(
    contracts::demuxer::BackendReadResult& result,
    std::optional<contracts::media::DemuxerStreamId> audio_stream_id,
    std::optional<contracts::media::DemuxerStreamId> video_stream_id,
    Generation::Value session_generation) noexcept {
    std::visit(
        Overloaded{
            [this, audio_stream_id, video_stream_id, session_generation](BackendPacket& packet) {
                if (audio_stream_id && packet.stream_id.value == audio_stream_id->value) {
                    store_pending_output(AudioPacketQueueItem{
                        std::in_place_type<AudioPacket>,
                        std::move(packet.packet),
                        session_generation,
                    });
                } else if (video_stream_id && packet.stream_id.value == video_stream_id->value) {
                    store_pending_output(VideoPacketQueueItem{
                        std::in_place_type<VideoPacket>,
                        std::move(packet.packet),
                        session_generation,
                    });
                }
            },
            [this, session_generation](BackendEndOfStream) {
                store_pending_end_of_input(session_generation);
            },
        },
        result);
}

void DefaultDemuxer::store_pending_output(PendingOutput output) noexcept {
    std::lock_guard lock(mutex_);
    if (worker_state_ == WorkerState::ShuttingDown ||
        session_state_ != SessionState::Running) {
        return;
    }
    assert(!pending_output_);
    const bool audio = std::holds_alternative<AudioPacketQueueItem>(output);
    pending_output_.emplace(std::move(output));
    if (audio) {
        audio_queue_not_full_hint_.store(true, std::memory_order_release);
    } else {
        video_queue_not_full_hint_.store(true, std::memory_order_release);
    }
}

void DefaultDemuxer::store_pending_end_of_input(Generation::Value generation) noexcept {
    std::lock_guard lock(mutex_);
    if (worker_state_ == WorkerState::ShuttingDown ||
        session_state_ != SessionState::Running) {
        return;
    }

    end_of_input_observed_ = true;
    if (audio_stream_id_) {
        audio_end_of_input_accepted_ = false;
    }
    if (video_stream_id_) {
        video_end_of_input_accepted_ = false;
    }
    pending_output_generation_ = generation;
    prepare_next_end_of_input_locked();
    maybe_transition_to_exhausted_locked();
}

void DefaultDemuxer::prepare_next_end_of_input_locked() noexcept {
    if (!end_of_input_observed_ || pending_output_) {
        return;
    }

    if (audio_stream_id_ && !audio_end_of_input_accepted_) {
        pending_output_.emplace(AudioPacketEndOfInput{
            .generation = pending_output_generation_,
        });
        audio_queue_not_full_hint_.store(true, std::memory_order_release);
        return;
    }

    if (video_stream_id_ && !video_end_of_input_accepted_) {
        pending_output_.emplace(VideoPacketEndOfInput{
            .generation = pending_output_generation_,
        });
        video_queue_not_full_hint_.store(true, std::memory_order_release);
    }
}

void DefaultDemuxer::maybe_transition_to_exhausted_locked() noexcept {
    if (!end_of_input_observed_ || pending_output_ ||
        (audio_stream_id_ && !audio_end_of_input_accepted_) ||
        (video_stream_id_ && !video_end_of_input_accepted_)) {
        return;
    }

    const bool exhausted = transition_session_locked(SessionEvent::InputExhausted);
    assert(exhausted);
}

void DefaultDemuxer::handle_read_error(DemuxerBackendError error) noexcept {
    bool should_notify = false;
    {
        std::lock_guard lock(mutex_);
        if (worker_state_ != WorkerState::ShuttingDown &&
            session_state_ == SessionState::Running) {
            pending_output_.reset();
            const bool failed = transition_session_locked(SessionEvent::BackendFailed);
            assert(failed);
            should_notify = true;
        }
    }

    if (should_notify) {
        notify_read_error(std::move(error));
    }
}

void DefaultDemuxer::notify_read_error(DemuxerBackendError error) noexcept {
    if (!notifier_) {
        return;
    }

    try {
        DemuxerReadError event{.error = std::move(error)};
        (void)notifier_->send(event);
    } catch (...) {
        // Read failure is already reflected in the session state.
    }
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
