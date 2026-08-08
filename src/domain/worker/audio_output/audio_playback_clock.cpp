#include "domain/worker/audio_output/audio_playback_clock.hpp"

#include <algorithm>

namespace semi::domain {
namespace {
std::int64_t now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace

void AudioPlaybackClockState::configure(std::uint32_t sample_rate) noexcept {
    sample_rate_.store(sample_rate, std::memory_order_relaxed);
    reset();
}

void AudioPlaybackClockState::reset() noexcept {
    valid_.store(false, std::memory_order_release);
    prepared_.store(false, std::memory_order_release);
    finished_.store(false, std::memory_order_release);
    paused_.store(false, std::memory_order_release);
    anchor_pts_us_.store(0, std::memory_order_relaxed);
    anchor_ns_.store(0, std::memory_order_relaxed);
}

void AudioPlaybackClockState::pause() noexcept {
    if (!paused_.exchange(true, std::memory_order_acq_rel)) {
        const auto current = current_pts();
        if (current) {
            anchor_pts_us_.store(*current, std::memory_order_relaxed);
            anchor_ns_.store(now_ns(), std::memory_order_relaxed);
        }
    }
}

void AudioPlaybackClockState::resume() noexcept {
    paused_.store(false, std::memory_order_release);
    if (valid_.load(std::memory_order_acquire)) {
        anchor_ns_.store(now_ns(), std::memory_order_relaxed);
    }
}

void AudioPlaybackClockState::finish() noexcept {
    if (const auto current = current_pts()) {
        anchor_pts_us_.store(*current, std::memory_order_relaxed);
    }
    finished_.store(true, std::memory_order_release);
}

void AudioPlaybackClockState::prepare_pcm(std::uint64_t generation,
                                           std::int64_t pts_us) noexcept {
    generation_.store(generation, std::memory_order_relaxed);
    prepared_pts_us_.store(pts_us, std::memory_order_relaxed);
    prepared_.store(true, std::memory_order_release);
    valid_.store(false, std::memory_order_release);
    finished_.store(false, std::memory_order_release);
}

void AudioPlaybackClockState::on_audio_frames_consumed(
    const contracts::audio_output::AudioFramesConsumed& event) noexcept {
    if (!event.first_pts_us || event.frames == 0 || event.sample_rate == 0) {
        return;
    }
    generation_.store(event.generation, std::memory_order_relaxed);
    sample_rate_.store(event.sample_rate, std::memory_order_relaxed);
    anchor_pts_us_.store(*event.first_pts_us +
                             static_cast<std::int64_t>(event.frames) * 1'000'000 /
                                 static_cast<std::int64_t>(event.sample_rate),
                         std::memory_order_relaxed);
    anchor_ns_.store(now_ns(), std::memory_order_relaxed);
    prepared_.store(false, std::memory_order_release);
    valid_.store(true, std::memory_order_release);
    finished_.store(false, std::memory_order_release);
}

std::optional<std::int64_t> AudioPlaybackClockState::current_pts() const noexcept {
    if (paused_.load(std::memory_order_acquire) || finished_.load(std::memory_order_acquire)) {
        if (valid_.load(std::memory_order_acquire)) {
            return anchor_pts_us_.load(std::memory_order_relaxed);
        }
        if (paused_.load(std::memory_order_relaxed) && prepared_.load(std::memory_order_relaxed)) {
            return prepared_pts_us_.load(std::memory_order_relaxed);
        }
        return std::nullopt;
    }
    if (!valid_.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    const auto rate = sample_rate_.load(std::memory_order_relaxed);
    if (rate == 0) return std::nullopt;
    const auto elapsed = std::max<std::int64_t>(0, now_ns() - anchor_ns_.load(std::memory_order_relaxed));
    return anchor_pts_us_.load(std::memory_order_relaxed) + elapsed / 1'000;
}

} // namespace semi::domain
