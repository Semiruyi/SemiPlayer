#pragma once

#include "contracts/audio_output/audio_output_realtime_events.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>

namespace semi::domain {

// Realtime-safe playback position state owned by DefaultAudioOutput.
class AudioPlaybackClockState final {
public:
    void configure(std::uint32_t sample_rate) noexcept;
    void reset() noexcept;
    void pause() noexcept;
    void resume() noexcept;
    void finish() noexcept;
    void prepare_pcm(std::uint64_t generation, std::int64_t pts_us) noexcept;
    void on_audio_frames_consumed(
        const contracts::audio_output::AudioFramesConsumed& event) noexcept;

    [[nodiscard]] std::optional<std::int64_t> current_pts() const noexcept;

private:
    using Clock = std::chrono::steady_clock;

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
