#pragma once

#include "domain/resource/generation/generation.hpp"

namespace semi::domain {

// Sent after the final frame for the generation has been presented and the
// synchronous host callback, if any, has returned.
struct VideoPlaybackFinished {
    Generation::Value generation = 0;
};

} // namespace semi::domain
