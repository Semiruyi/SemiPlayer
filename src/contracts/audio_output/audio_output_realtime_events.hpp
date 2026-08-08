#pragma once

#include "infrastructure/notifier/realtime_notifier.hpp"

#include <cstdint>
#include <optional>

namespace semi::contracts::audio_output {

// Produced only for PCM frames actually read by the audio device callback.
// Synthesized silence is not media and must not produce this event.
struct AudioFramesConsumed {
    std::uint64_t generation = 0;
    std::optional<std::int64_t> first_pts_us;
    std::uint32_t frames = 0;
    std::uint32_t sample_rate = 0;
};

// One slot is used by AudioOutput. The second is reserved for AudioClock.
using AudioOutputRealTimeNotifier = infra::RealTimeNotifier<
    infra::RealTimeEventSpec<AudioFramesConsumed, 2>>;

} // namespace semi::contracts::audio_output
