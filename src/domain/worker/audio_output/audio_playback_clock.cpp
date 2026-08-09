#include "domain/worker/audio_output/audio_playback_clock.hpp"

#include <algorithm>

namespace semi::domain {
namespace {
std::int64_t now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::int64_t frames_to_duration_us(std::uint64_t frames,
                                   std::uint32_t sample_rate) noexcept {
    const auto whole_seconds = frames / sample_rate;
    const auto remainder = frames % sample_rate;
    return static_cast<std::int64_t>(whole_seconds) * 1'000'000 +
           static_cast<std::int64_t>(remainder) * 1'000'000 /
               static_cast<std::int64_t>(sample_rate);
}
} // namespace

void AudioPlaybackClockState::begin_write() noexcept {
    sequence_.fetch_add(1, std::memory_order_acq_rel);
}

void AudioPlaybackClockState::end_write() noexcept {
    sequence_.fetch_add(1, std::memory_order_release);
}

void AudioPlaybackClockState::reset(std::uint64_t generation,
                                    std::uint32_t sample_rate) noexcept {
    begin_write();
    sample_rate_.store(sample_rate, std::memory_order_relaxed);
    generation_.store(generation, std::memory_order_relaxed);
    consumed_frames_.store(0, std::memory_order_relaxed);
    first_pts_us_.store(0, std::memory_order_relaxed);
    last_consumed_ns_.store(0, std::memory_order_relaxed);
    frozen_pts_us_.store(0, std::memory_order_relaxed);
    has_first_pts_.store(false, std::memory_order_release);
    has_frozen_position_.store(false, std::memory_order_release);
    finished_.store(false, std::memory_order_release);
    paused_.store(false, std::memory_order_release);
    end_write();
}

void AudioPlaybackClockState::pause(std::uint64_t generation) noexcept {
    if (generation_.load(std::memory_order_acquire) != generation) {
        return;
    }
    if (paused_.load(std::memory_order_acquire)) {
        return;
    }

    const auto current = current_position();
    begin_write();
    if (generation_.load(std::memory_order_relaxed) != generation) {
        end_write();
        return;
    }
    if (current) {
        frozen_pts_us_.store(current->pts_us, std::memory_order_relaxed);
        has_frozen_position_.store(true, std::memory_order_release);
    }
    paused_.store(true, std::memory_order_release);
    end_write();
}

void AudioPlaybackClockState::resume(std::uint64_t generation) noexcept {
    if (generation_.load(std::memory_order_acquire) != generation) {
        return;
    }
    begin_write();
    if (generation_.load(std::memory_order_relaxed) != generation) {
        end_write();
        return;
    }
    paused_.store(false, std::memory_order_release);
    has_frozen_position_.store(false, std::memory_order_release);
    if (consumed_frames_.load(std::memory_order_relaxed) > 0) {
        last_consumed_ns_.store(now_ns(), std::memory_order_relaxed);
    }
    end_write();
}

void AudioPlaybackClockState::finish(std::uint64_t generation) noexcept {
    if (generation_.load(std::memory_order_acquire) != generation) {
        return;
    }
    if (const auto current = current_position()) {
        begin_write();
        if (generation_.load(std::memory_order_relaxed) != generation) {
            end_write();
            return;
        }
        frozen_pts_us_.store(current->pts_us, std::memory_order_relaxed);
        has_frozen_position_.store(true, std::memory_order_release);
        finished_.store(true, std::memory_order_release);
        end_write();
        return;
    }
    begin_write();
    if (generation_.load(std::memory_order_relaxed) != generation) {
        end_write();
        return;
    }
    finished_.store(true, std::memory_order_release);
    end_write();
}

bool AudioPlaybackClockState::set_first_pts(std::uint64_t generation,
                                            std::int64_t pts_us) noexcept {
    // The first PTS is a one-shot anchor for a fresh time line. A later PCM
    // frame must not replace a running clock's callback-derived position.
    if (generation_.load(std::memory_order_acquire) != generation ||
        has_first_pts_.load(std::memory_order_acquire) ||
        finished_.load(std::memory_order_acquire)) {
        return false;
    }
    begin_write();
    if (generation_.load(std::memory_order_relaxed) != generation ||
        has_first_pts_.load(std::memory_order_relaxed) ||
        finished_.load(std::memory_order_relaxed)) {
        end_write();
        return false;
    }
    first_pts_us_.store(pts_us, std::memory_order_relaxed);
    has_first_pts_.store(true, std::memory_order_release);
    has_frozen_position_.store(false, std::memory_order_release);
    finished_.store(false, std::memory_order_release);
    end_write();
    return true;
}

void AudioPlaybackClockState::on_audio_frames_consumed(
    std::uint64_t generation, std::uint32_t frames) noexcept {
    const auto sample_rate = sample_rate_.load(std::memory_order_acquire);
    if (frames == 0 || sample_rate == 0 ||
        generation_.load(std::memory_order_acquire) != generation) {
        return;
    }
    begin_write();
    if (generation_.load(std::memory_order_relaxed) != generation ||
        !has_first_pts_.load(std::memory_order_relaxed) ||
        sample_rate_.load(std::memory_order_relaxed) == 0) {
        end_write();
        return;
    }

    const auto consumed_frames =
        consumed_frames_.fetch_add(frames, std::memory_order_relaxed) + frames;
    last_consumed_ns_.store(now_ns(), std::memory_order_relaxed);
    if (paused_.load(std::memory_order_relaxed)) {
        frozen_pts_us_.store(
            first_pts_us_.load(std::memory_order_relaxed) +
                frames_to_duration_us(consumed_frames, sample_rate),
            std::memory_order_relaxed);
        has_frozen_position_.store(true, std::memory_order_release);
    } else {
        has_frozen_position_.store(false, std::memory_order_release);
    }
    finished_.store(false, std::memory_order_release);
    end_write();
}

std::optional<PlaybackPosition> AudioPlaybackClockState::current_position() const noexcept {
    for (;;) {
        const auto sequence = sequence_.load(std::memory_order_acquire);
        if (sequence & 1U) continue;
        const bool paused = paused_.load(std::memory_order_acquire);
        const bool finished = finished_.load(std::memory_order_acquire);
        const bool has_first_pts = has_first_pts_.load(std::memory_order_acquire);
        const auto sample_rate = sample_rate_.load(std::memory_order_relaxed);
        const auto consumed_frames = consumed_frames_.load(std::memory_order_relaxed);
        std::optional<PlaybackPosition> result;
        if (has_first_pts && sample_rate != 0 && (paused || finished)) {
            if (has_frozen_position_.load(std::memory_order_acquire)) {
                result = PlaybackPosition{
                    .generation = generation_.load(std::memory_order_relaxed),
                    .pts_us = frozen_pts_us_.load(std::memory_order_relaxed),
                };
            } else if (paused || consumed_frames > 0) {
                result = PlaybackPosition{
                    .generation = generation_.load(std::memory_order_relaxed),
                    .pts_us = first_pts_us_.load(std::memory_order_relaxed) +
                              frames_to_duration_us(consumed_frames, sample_rate),
                };
            }
        } else if (has_first_pts && sample_rate != 0 && consumed_frames > 0) {
            const auto elapsed = std::max<std::int64_t>(
                0, now_ns() - last_consumed_ns_.load(std::memory_order_relaxed));
            result = PlaybackPosition{
                .generation = generation_.load(std::memory_order_relaxed),
                .pts_us = first_pts_us_.load(std::memory_order_relaxed) +
                          frames_to_duration_us(consumed_frames, sample_rate) +
                          elapsed / 1'000,
            };
        }
        if (sequence_.load(std::memory_order_acquire) == sequence) return result;
    }
}

} // namespace semi::domain
