#pragma once

#include "contracts/audio_output/audio_output_realtime_events.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>

namespace semi::domain {

struct PlaybackPosition {
    std::uint64_t generation = 0;
    std::int64_t pts_us = 0;
};

// Realtime-safe playback position state owned by DefaultAudioOutput.
class AudioPlaybackClockState final {
public:
    void configure(std::uint32_t sample_rate) noexcept;
    void reset() noexcept;
    void pause() noexcept;
    void resume() noexcept;
    void finish() noexcept;
    [[nodiscard]] bool prepare_pcm(std::uint64_t generation, std::int64_t pts_us) noexcept;
    void on_audio_frames_consumed(std::uint32_t frames) noexcept;

    [[nodiscard]] std::optional<PlaybackPosition> current_position() const noexcept;

private:
    using Clock = std::chrono::steady_clock;

    void begin_write() noexcept;
    void end_write() noexcept;

    std::atomic<std::uint64_t> sequence_{0};
    std::atomic<std::uint64_t> generation_{0};
    std::atomic<std::int64_t> anchor_pts_us_{0};
    std::atomic<std::int64_t> anchor_ns_{0};
    std::atomic<std::uint32_t> sample_rate_{0};
    std::atomic<std::int64_t> prepared_pts_us_{0};
    std::atomic<bool> valid_{false};
    std::atomic<bool> prepared_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> finished_{false};
};

} // namespace semi::domain
