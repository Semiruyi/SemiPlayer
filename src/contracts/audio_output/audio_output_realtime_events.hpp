#pragma once

#include "infrastructure/notifier/realtime_notifier.hpp"

#include <cstdint>
namespace semi::contracts::audio_output {

// AudioOutput owns the clock, so backend progress has one sink: AudioOutput.
using AudioOutputRealTimeNotifier = infra::RealTimeNotifier<
    infra::RealTimeEventSpec<std::uint32_t, 1>>;

} // namespace semi::contracts::audio_output
