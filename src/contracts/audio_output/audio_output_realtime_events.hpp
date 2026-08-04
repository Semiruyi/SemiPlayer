#pragma once

#include "infrastructure/notifier/realtime_notifier.hpp"

#include <cstdint>

namespace semi::contracts::audio_output {

// Produced only for PCM frames actually read by the audio device callback.
// Synthesized silence is not media and must not produce this event.
struct AudioFramesConsumed {
    std::uint32_t frames = 0;
};

// One slot is used by AudioOutput. The second is reserved for AudioClock.
using AudioOutputRealTimeNotifier = infra::RealTimeNotifier<
    infra::RealTimeEventSpec<AudioFramesConsumed, 2>>;

} // namespace semi::contracts::audio_output
