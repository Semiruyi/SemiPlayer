#pragma once

#include "infrastructure/notifier/realtime_notifier.hpp"

#include <cstdint>
namespace semi::contracts::audio_output {

// One slot is used by AudioOutput. The second is reserved for AudioClock.
using AudioOutputRealTimeNotifier = infra::RealTimeNotifier<
    infra::RealTimeEventSpec<std::uint32_t, 2>>;

} // namespace semi::contracts::audio_output
