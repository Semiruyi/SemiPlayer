#pragma once

#include "domain/resource/generation/generation.hpp"
#include "domain/worker/audio_output/audio_output.hpp"

namespace semi::domain {

struct AudioPlaybackFinished {
    Generation::Value generation = 0;
};

// Sent from the AudioOutput worker once current_position() becomes available
// for a generation. Consumers such as VideoSync use it to avoid polling while
// the first confirmed audio frame is still being established.
struct AudioPlaybackPositionReady {
    Generation::Value generation = 0;
};

struct AudioOutputBackendFailure {
    Generation::Value generation = 0;
    AudioOutputBackendError error;
};

} // namespace semi::domain
