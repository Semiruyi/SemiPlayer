#include "domain/worker/video_decoder/default_video_decoder.hpp"

#include "domain/resource/generation/generation_events.hpp"
#include "domain/worker/video_decoder/video_decoder_events.hpp"

#include <cassert>
#include <algorithm>
#include <chrono>
#include "infrastructure/log/log.hpp"

#define SEMI_LOG_TAG "video_decoder"
#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace semi::domain {
namespace {

std::int64_t steady_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::int64_t unix_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void require_state_transition(bool succeeded) noexcept {
    assert(succeeded);
    if (!succeeded) [[unlikely]] {
        std::terminate();
    }
}

VideoDecoderError invalid_state(std::string message) {
    return VideoDecoderError{
        .code = VideoDecoderErrorCode::InvalidState,
        .message = std::move(message),
        .backend_error = std::nullopt,
    };
}

VideoDecoderBackendError backend_exception(VideoDecoderBackendOperation operation,
                                           std::string message) {
    return VideoDecoderBackendError{
        .operation = operation,
        .native_code = 0,
        .message = std::move(message),
    };
}

} // namespace

DefaultVideoDecoder::DefaultVideoDecoder(std::shared_ptr<VideoPacketSource> video_packet_source,
                                         std::shared_ptr<VideoFrameSink> video_frame_sink,
                                         std::shared_ptr<VideoDecoderBackend> backend,
                                         std::shared_ptr<infra::Notifier> notifier,
                                         std::shared_ptr<Generation> generation)
    : video_packet_source_(std::move(video_packet_source)),
      video_frame_sink_(std::move(video_frame_sink)),
      backend_(std::move(backend)),
      notifier_(std::move(notifier)),
      generation_(std::move(generation)),
      worker_([this] { worker_main(); }) {
    if (!notifier_) {
        return;
    }

    video_queue_not_empty_subscription_ =
        notifier_->subscribe<VideoQueueNotEmpty>([this](const VideoQueueNotEmpty&) {
            {
                std::lock_guard lock(mutex_);
                input_not_empty_hint_ = true;
            }
            cv_.notify_one();
        });
    video_frame_store_not_full_subscription_ =
        notifier_->subscribe<VideoFrameStoreNotFull>([this](const VideoFrameStoreNotFull&) {
            {
                std::lock_guard lock(mutex_);
                output_not_full_hint_ = true;
            }
            cv_.notify_one();
        });
    generation_changed_subscription_ = notifier_->subscribe<GenerationChanged>(
        [this](const GenerationChanged&) { cv_.notify_one(); });
}

DefaultVideoDecoder::~DefaultVideoDecoder() {
    shutdown_worker();
    video_queue_not_empty_subscription_.reset();
    video_frame_store_not_full_subscription_.reset();
    generation_changed_subscription_.reset();
}

std::expected<void, VideoDecoderError>
DefaultVideoDecoder::configure(const contracts::media::VideoCodecConfig& config) {
    ConfigureCommand command;
    command.config = config;
    auto completion = command.completion.get_future();
    {
        std::lock_guard lock(mutex_);
        commands_.emplace_back(std::move(command));
    }
    cv_.notify_one();
    return completion.get();
}

void DefaultVideoDecoder::unconfigure() noexcept {
    UnconfigureCommand command;
    auto completion = command.completion.get_future();
    {
        std::lock_guard lock(mutex_);
        commands_.emplace_back(std::move(command));
    }
    cv_.notify_one();
    completion.wait();
}

void DefaultVideoDecoder::worker_main() noexcept {
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
    lock.unlock();
    publish_statistics(VideoDecoderStatisticsReason::Shutdown);
    lock.lock();
    require_state_transition(transition_worker_locked(WorkerEvent::Stopped));
    cv_.notify_all();
}

void DefaultVideoDecoder::shutdown_worker() noexcept {
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

void DefaultVideoDecoder::process_command(ConfigureCommand& command) noexcept {
    {
        std::lock_guard lock(mutex_);
        const bool requested = transition_session_locked(SessionEvent::ConfigureRequested);
        if (!requested) {
            command.completion.set_value(
                std::unexpected(invalid_state("video decoder is already configured")));
            return;
        }
    }

    if (!backend_ || !video_packet_source_ || !video_frame_sink_ || !notifier_ || !generation_) {
        std::lock_guard lock(mutex_);
        require_state_transition(transition_session_locked(SessionEvent::ConfigureFailed));
        command.completion.set_value(
            std::unexpected(invalid_state("video decoder dependencies are unavailable")));
        return;
    }

    std::expected<void, VideoDecoderBackendError> configured;
    try {
        configured = backend_->configure(command.config);
    } catch (...) {
        configured = std::unexpected(
            backend_exception(VideoDecoderBackendOperation::Configure,
                              "video decoder backend configuration threw an exception"));
    }
    if (!configured) {
        backend_->unconfigure();
        std::lock_guard lock(mutex_);
        require_state_transition(transition_session_locked(SessionEvent::ConfigureFailed));
        command.completion.set_value(std::unexpected(VideoDecoderError{
            .code = VideoDecoderErrorCode::BackendFailure,
            .message = configured.error().message,
            .backend_error = std::move(configured.error()),
        }));
        return;
    }

    begin_statistics(command.config);
    std::lock_guard lock(mutex_);
    active_generation_ = generation_->current();
    seek_target_pts_us_ = generation_->seek_target_for(active_generation_);
    seek_gate_open_ = !seek_target_pts_us_.has_value();
    pending_outputs_.clear();
    input_exhausted_ = false;
    input_not_empty_hint_ = true;
    output_not_full_hint_ = true;
    require_state_transition(transition_session_locked(SessionEvent::ConfigureSucceeded));
    command.completion.set_value(std::expected<void, VideoDecoderError>{});
}

void DefaultVideoDecoder::process_command(UnconfigureCommand& command) noexcept {
    {
        std::lock_guard lock(mutex_);
        if (session_state_ == SessionState::Constructed) {
            command.completion.set_value();
            return;
        }
        require_state_transition(transition_session_locked(SessionEvent::UnconfigureRequested));
    }

    publish_statistics(VideoDecoderStatisticsReason::Unconfigure);
    if (backend_) {
        backend_->unconfigure();
    }

    std::lock_guard lock(mutex_);
    pending_outputs_.clear();
    active_generation_ = 0;
    seek_target_pts_us_.reset();
    seek_gate_open_ = true;
    input_exhausted_ = false;
    input_not_empty_hint_ = false;
    output_not_full_hint_ = false;
    require_state_transition(transition_session_locked(SessionEvent::UnconfigureSucceeded));
    command.completion.set_value();
}

bool DefaultVideoDecoder::should_process_data_locked() const noexcept {
    if (session_state_ != SessionState::Configured) {
        return false;
    }

    if (generation_ && active_generation_ != generation_->current()) {
        return true;
    }

    if (!pending_outputs_.empty()) {
        return output_not_full_hint_;
    }

    if (input_exhausted_ && (!generation_ || active_generation_ == generation_->current())) {
        return false;
    }

    return input_not_empty_hint_;
}

void DefaultVideoDecoder::adopt_generation_if_needed(
    Generation::Value current_generation) noexcept {
    const auto seek_target =
        generation_ ? generation_->seek_target_for(current_generation) : std::nullopt;
    bool generation_changed = false;
    {
        std::lock_guard lock(mutex_);
        if (session_state_ != SessionState::Configured ||
            current_generation == active_generation_) {
            return;
        }

        pending_outputs_.clear();
        input_exhausted_ = false;
        active_generation_ = current_generation;
        seek_target_pts_us_ = seek_target;
        seek_gate_open_ = !seek_target.has_value();
        input_not_empty_hint_ = true;
        output_not_full_hint_ = false;
        generation_changed = true;
    }

    if (generation_changed && backend_) {
        backend_->reset();
    }
}

DefaultVideoDecoder::PendingOutputPushResult
DefaultVideoDecoder::try_push_pending_output() noexcept {
    std::shared_ptr<VideoFrameSink> video_frame_sink;
    std::optional<VideoFrameStoreItem> pending_output;

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
        video_frame_sink = video_frame_sink_;
    }

    assert(video_frame_sink);
    const auto pushed = video_frame_sink->try_push(std::move(*pending_output));

    std::lock_guard lock(mutex_);
    if (worker_state_ == WorkerState::ShuttingDown || session_state_ != SessionState::Configured) {
        return PendingOutputPushResult::Handled;
    }

    if (pushed == VideoFramePushResult::Full) {
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

void DefaultVideoDecoder::read_next_input_to_pending() noexcept {
    std::shared_ptr<VideoPacketSource> video_packet_source;
    {
        std::lock_guard lock(mutex_);
        if (session_state_ != SessionState::Configured ||
            (input_exhausted_ && (!generation_ || active_generation_ == generation_->current())) ||
            !pending_outputs_.empty()) {
            return;
        }
        if (!input_not_empty_hint_) {
            return;
        }

        input_not_empty_hint_ = false;
        video_packet_source = video_packet_source_;
    }

    assert(video_packet_source);
    auto item = video_packet_source->try_pop();
    if (!item) {
        return;
    }

    handle_input_item(std::move(*item));
}

void DefaultVideoDecoder::handle_input_item(VideoPacketQueueItem item) noexcept {
    const Generation::Value current_generation = generation_ ? generation_->current() : 0;
    adopt_generation_if_needed(current_generation);

    {
        std::lock_guard lock(mutex_);
        if (session_state_ != SessionState::Configured) {
            return;
        }
    }

    if (video_packet_queue_item_generation(item) != current_generation) {
        std::lock_guard lock(mutex_);
        if (session_state_ == SessionState::Configured && !input_exhausted_) {
            input_not_empty_hint_ = true;
        }
        return;
    }

    if (auto* packet = std::get_if<VideoPacket>(&item)) {
        handle_video_packet(std::move(*packet), current_generation);
        return;
    }

    handle_end_of_input(current_generation);
}

void DefaultVideoDecoder::handle_video_packet(VideoPacket packet,
                                              Generation::Value current_generation) noexcept {
    std::shared_ptr<VideoDecoderBackend> backend;
    {
        std::lock_guard lock(mutex_);
        if (session_state_ != SessionState::Configured) {
            return;
        }
        backend = backend_;
    }

    assert(backend);
    std::expected<contracts::video_decoder::DecodedVideoBatch, VideoDecoderBackendError> decoded;
    const auto started_ns = steady_ns();
    if (statistics_active_) ++statistics_.decode_calls;
    try {
        decoded = backend->decode(packet.encoded());
    } catch (...) {
        decoded =
            std::unexpected(backend_exception(VideoDecoderBackendOperation::Decode,
                                              "video decoder backend decode threw an exception"));
    }

    record_statistics(steady_ns() - started_ns, decoded ? &*decoded : nullptr);
    if (!decoded) {
        handle_backend_failure(std::move(decoded.error()));
        return;
    }

    store_decoded_outputs(std::move(*decoded), current_generation, false);
}

void DefaultVideoDecoder::handle_end_of_input(Generation::Value generation) noexcept {
    std::shared_ptr<VideoDecoderBackend> backend;
    {
        std::lock_guard lock(mutex_);
        if (session_state_ != SessionState::Configured) {
            return;
        }
        input_exhausted_ = true;
        backend = backend_;
    }

    assert(backend);
    std::expected<contracts::video_decoder::DecodedVideoBatch, VideoDecoderBackendError> drained;
    const auto started_ns = steady_ns();
    if (statistics_active_) ++statistics_.drain_calls;
    try {
        drained = backend->drain();
    } catch (...) {
        drained = std::unexpected(backend_exception(
            VideoDecoderBackendOperation::Drain, "video decoder backend drain threw an exception"));
    }

    record_statistics(steady_ns() - started_ns, drained ? &*drained : nullptr);
    if (!drained) {
        handle_backend_failure(std::move(drained.error()));
        return;
    }

    store_decoded_outputs(std::move(*drained), generation, true);
}

void DefaultVideoDecoder::store_decoded_outputs(contracts::video_decoder::DecodedVideoBatch decoded,
                                                Generation::Value generation,
                                                bool append_end_of_input) noexcept {
    std::lock_guard lock(mutex_);
    if (worker_state_ == WorkerState::ShuttingDown || session_state_ != SessionState::Configured ||
        active_generation_ != generation || (generation_ && generation_->current() != generation)) {
        return;
    }

    for (auto& frame : decoded) {
        if (!seek_gate_open_) {
            if (!frame.pts_us || *frame.pts_us < *seek_target_pts_us_) {
                continue;
            }
            seek_gate_open_ = true;
        }
        pending_outputs_.emplace_back(std::in_place_type<VideoFrame>, std::move(frame), generation);
    }
    if (append_end_of_input) {
        pending_outputs_.emplace_back(VideoFrameEndOfInput{.generation = generation});
    }

    if (!pending_outputs_.empty()) {
        output_not_full_hint_ = true;
    } else if (!input_exhausted_) {
        input_not_empty_hint_ = true;
    }
}

void DefaultVideoDecoder::handle_backend_failure(VideoDecoderBackendError error) noexcept {
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
        publish_statistics(VideoDecoderStatisticsReason::BackendFailure);
        notify_backend_failure(std::move(error));
    }
}

void DefaultVideoDecoder::notify_backend_failure(VideoDecoderBackendError error) noexcept {
    if (!notifier_) {
        return;
    }

    try {
        VideoDecoderBackendFailure event{.error = std::move(error)};
        (void)notifier_->send(event);
    } catch (...) {
        // Backend failure is already reflected in the session state.
    }
}

void DefaultVideoDecoder::begin_statistics(const contracts::media::VideoCodecConfig& config) noexcept {
    try {
        statistics_ = VideoDecoderStatistics{};
        statistics_.codec = config.common.codec_name;
        statistics_.coded_width = config.coded_width;
        statistics_.coded_height = config.coded_height;
        statistics_.configured_at_unix_ms = unix_ms();
        statistics_started_ns_ = steady_ns();
        statistics_active_ = true;
    } catch (...) {
        statistics_active_ = false;
        SEMI_LOG_WARN("cannot initialize decoder statistics");
    }
}

void DefaultVideoDecoder::record_statistics(std::uint64_t elapsed_ns,
    const contracts::video_decoder::DecodedVideoBatch* output) noexcept {
    if (!statistics_active_) return;
    statistics_.processing_total_ns += elapsed_ns;
    statistics_.processing_max_ns = std::max(statistics_.processing_max_ns, elapsed_ns);
    if (!output) { ++statistics_.failed_calls; return; }
    statistics_.output_frames += output->size();
    // Observe all decoded output, including frames discarded by accurate Seek
    // or a concurrent generation change. They still consumed decoder work.
    for (const auto& frame : *output) {
        if (!frame.buffer) continue;
        const auto format = frame.buffer->pixel_format();
        const auto width = frame.buffer->width();
        const auto height = frame.buffer->height();
        if (!statistics_.output_observed) {
            statistics_.output_observed = true;
            statistics_.output_format = format;
            statistics_.output_width = width;
            statistics_.output_height = height;
        } else if (format != statistics_.output_format || width != statistics_.output_width ||
                   height != statistics_.output_height) {
            statistics_.format_changed = true;
        }
    }
}

void DefaultVideoDecoder::publish_statistics(VideoDecoderStatisticsReason reason) noexcept {
    if (!statistics_active_) return;
    statistics_active_ = false; // Exactly once, including failure then unconfigure.
    statistics_.reason = reason;
    statistics_.finished_at_unix_ms = unix_ms();
    statistics_.configured_duration_ns = steady_ns() - statistics_started_ns_;
    try {
        if (notifier_) (void)notifier_->send(statistics_);
    } catch (...) {
        SEMI_LOG_WARN("cannot submit decoder statistics");
    }
}

bool DefaultVideoDecoder::transition_worker_locked(WorkerEvent event) noexcept {
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

bool DefaultVideoDecoder::transition_session_locked(SessionEvent event) noexcept {
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
        if (session_state_ == SessionState::Configured || session_state_ == SessionState::Failed) {
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
