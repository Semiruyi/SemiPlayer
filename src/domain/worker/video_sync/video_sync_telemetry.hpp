#pragma once

#include "domain/resource/generation/generation.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace semi::domain {

struct VideoSyncPresentationObservation final {
    std::optional<std::int64_t> frame_pts_us;
    std::optional<std::int64_t> clock_pts_us;
    std::uint64_t callback_duration_us = 0;
};

// Observes facts produced by VideoSync without participating in scheduling.
// Implementations must be non-blocking, allocation-free on the worker path,
// and noexcept.
class VideoSyncTelemetry {
public:
    virtual ~VideoSyncTelemetry() = default;

    VideoSyncTelemetry(const VideoSyncTelemetry&) = delete;
    VideoSyncTelemetry& operator=(const VideoSyncTelemetry&) = delete;
    VideoSyncTelemetry(VideoSyncTelemetry&&) = delete;
    VideoSyncTelemetry& operator=(VideoSyncTelemetry&&) = delete;

    virtual void on_session_started(Generation::Value generation) noexcept = 0;
    virtual void on_playback_started() noexcept = 0;
    virtual void on_frame_popped() noexcept = 0;
    virtual void on_frame_dropped_for_catchup() noexcept = 0;
    virtual void on_stale_item_dropped() noexcept = 0;
    virtual void on_empty_pop() noexcept = 0;
    virtual void on_audio_clock_unavailable() noexcept = 0;
    virtual void on_wait_scheduled(std::uint64_t target_us) noexcept = 0;
    virtual void on_wait_overshoot(std::uint64_t overshoot_us) noexcept = 0;
    virtual void on_frame_presented(
        const VideoSyncPresentationObservation& observation) noexcept = 0;
    virtual void on_session_finished(std::string_view reason) noexcept = 0;

protected:
    VideoSyncTelemetry() = default;
};

class NullVideoSyncTelemetry final : public VideoSyncTelemetry {
public:
    void on_session_started(Generation::Value) noexcept override {}
    void on_playback_started() noexcept override {}
    void on_frame_popped() noexcept override {}
    void on_frame_dropped_for_catchup() noexcept override {}
    void on_stale_item_dropped() noexcept override {}
    void on_empty_pop() noexcept override {}
    void on_audio_clock_unavailable() noexcept override {}
    void on_wait_scheduled(std::uint64_t) noexcept override {}
    void on_wait_overshoot(std::uint64_t) noexcept override {}
    void on_frame_presented(
        const VideoSyncPresentationObservation&) noexcept override {}
    void on_session_finished(std::string_view) noexcept override {}
};

} // namespace semi::domain
