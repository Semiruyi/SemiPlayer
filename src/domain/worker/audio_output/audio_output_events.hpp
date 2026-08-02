#pragma once

#include "domain/resource/generation/generation.hpp"
#include "domain/worker/audio_output/audio_output.hpp"

namespace semi::domain {

struct AudioPlaybackFinished {
    Generation::Value generation = 0;
};

struct AudioOutputBackendFailure {
    Generation::Value generation = 0;
    AudioOutputBackendError error;
};

} // namespace semi::domain
