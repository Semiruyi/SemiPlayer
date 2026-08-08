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

void AudioPlaybackClockState::begin_write() noexcept {
    sequence_.fetch_add(1, std::memory_order_acq_rel);
}

void AudioPlaybackClockState::end_write() noexcept {
    sequence_.fetch_add(1, std::memory_order_release);
}

void AudioPlaybackClockState::configure(std::uint32_t sample_rate) noexcept {
    sample_rate_.store(sample_rate, std::memory_order_relaxed);
    reset();
}

void AudioPlaybackClockState::reset() noexcept {
    begin_write();
    valid_.store(false, std::memory_order_release);
    prepared_.store(false, std::memory_order_release);
    finished_.store(false, std::memory_order_release);
    paused_.store(false, std::memory_order_release);
    anchor_pts_us_.store(0, std::memory_order_relaxed);
    anchor_ns_.store(0, std::memory_order_relaxed);
    end_write();
}

void AudioPlaybackClockState::pause() noexcept {
    if (!paused_.exchange(true, std::memory_order_acq_rel)) {
        const auto current = current_position();
        begin_write();
        if (current) {
            anchor_pts_us_.store(current->pts_us, std::memory_order_relaxed);
            anchor_ns_.store(now_ns(), std::memory_order_relaxed);
        }
        end_write();
    }
}

void AudioPlaybackClockState::resume() noexcept {
    begin_write();
    paused_.store(false, std::memory_order_release);
    if (valid_.load(std::memory_order_acquire)) {
        anchor_ns_.store(now_ns(), std::memory_order_relaxed);
    }
    end_write();
}

void AudioPlaybackClockState::finish() noexcept {
    if (const auto current = current_position()) {
        begin_write();
        anchor_pts_us_.store(current->pts_us, std::memory_order_relaxed);
        finished_.store(true, std::memory_order_release);
        end_write();
        return;
    }
    begin_write();
    finished_.store(true, std::memory_order_release);
    end_write();
}

bool AudioPlaybackClockState::prepare_pcm(std::uint64_t generation,
                                           std::int64_t pts_us) noexcept {
    // Prepared is a one-shot anchor for a fresh time line. A later PCM frame
    // must not replace a running clock's callback-derived position.
    if (valid_.load(std::memory_order_acquire) ||
        prepared_.load(std::memory_order_acquire) ||
        finished_.load(std::memory_order_acquire)) {
        return false;
    }
    begin_write();
    generation_.store(generation, std::memory_order_relaxed);
    prepared_pts_us_.store(pts_us, std::memory_order_relaxed);
    prepared_.store(true, std::memory_order_release);
    valid_.store(false, std::memory_order_release);
    finished_.store(false, std::memory_order_release);
    end_write();
    return true;
}

void AudioPlaybackClockState::on_audio_frames_consumed(
    std::uint32_t frames) noexcept {
    const auto sample_rate = sample_rate_.load(std::memory_order_acquire);
    if (frames == 0 || sample_rate == 0) {
        return;
    }
    begin_write();
    const auto duration_us = static_cast<std::int64_t>(frames) * 1'000'000 /
                             static_cast<std::int64_t>(sample_rate);
    if (valid_.load(std::memory_order_relaxed)) {
        anchor_pts_us_.store(anchor_pts_us_.load(std::memory_order_relaxed) + duration_us,
                             std::memory_order_relaxed);
    } else if (prepared_.load(std::memory_order_relaxed)) {
        anchor_pts_us_.store(prepared_pts_us_.load(std::memory_order_relaxed) + duration_us,
                             std::memory_order_relaxed);
    } else {
        end_write();
        return;
    }
    anchor_ns_.store(now_ns(), std::memory_order_relaxed);
    prepared_.store(false, std::memory_order_release);
    valid_.store(true, std::memory_order_release);
    finished_.store(false, std::memory_order_release);
    end_write();
}

std::optional<PlaybackPosition> AudioPlaybackClockState::current_position() const noexcept {
    for (;;) {
        const auto sequence = sequence_.load(std::memory_order_acquire);
        if (sequence & 1U) continue;
        const bool paused = paused_.load(std::memory_order_acquire);
        const bool finished = finished_.load(std::memory_order_acquire);
        std::optional<PlaybackPosition> result;
        if (paused || finished) {
        if (valid_.load(std::memory_order_acquire)) {
            result = PlaybackPosition{
                .generation = generation_.load(std::memory_order_relaxed),
                .pts_us = anchor_pts_us_.load(std::memory_order_relaxed),
            };
        } else if (paused && prepared_.load(std::memory_order_relaxed)) {
            result = PlaybackPosition{
                .generation = generation_.load(std::memory_order_relaxed),
                .pts_us = prepared_pts_us_.load(std::memory_order_relaxed),
            };
        }
        } else if (valid_.load(std::memory_order_acquire)) {
            const auto rate = sample_rate_.load(std::memory_order_relaxed);
            if (rate != 0) {
                const auto elapsed = std::max<std::int64_t>(
                    0, now_ns() - anchor_ns_.load(std::memory_order_relaxed));
                result = PlaybackPosition{
                    .generation = generation_.load(std::memory_order_relaxed),
                    .pts_us = anchor_pts_us_.load(std::memory_order_relaxed) + elapsed / 1'000,
                };
            }
        }
        if (sequence_.load(std::memory_order_acquire) == sequence) return result;
    }
}

} // namespace semi::domain
