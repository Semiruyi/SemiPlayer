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
    void reset(std::uint64_t generation, std::uint32_t sample_rate) noexcept;
    void pause(std::uint64_t generation) noexcept;
    void resume(std::uint64_t generation) noexcept;
    void finish(std::uint64_t generation) noexcept;
    [[nodiscard]] bool set_first_pts(std::uint64_t generation,
                                     std::int64_t pts_us) noexcept;
    void on_audio_frames_consumed(std::uint64_t generation,
                                  std::uint32_t frames) noexcept;

    [[nodiscard]] std::optional<PlaybackPosition> current_position() const noexcept;

private:
    using Clock = std::chrono::steady_clock;

    void begin_write() noexcept;
    void end_write() noexcept;

    std::atomic<std::uint64_t> sequence_{0};
    std::atomic<std::uint64_t> generation_{0};
    std::atomic<std::uint64_t> consumed_frames_{0};
    std::atomic<std::int64_t> first_pts_us_{0};
    std::atomic<std::int64_t> last_consumed_ns_{0};
    std::atomic<std::int64_t> frozen_pts_us_{0};
    std::atomic<std::uint32_t> sample_rate_{0};
    std::atomic<bool> has_first_pts_{false};
    std::atomic<bool> has_frozen_position_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> finished_{false};
};

} // namespace semi::domain
