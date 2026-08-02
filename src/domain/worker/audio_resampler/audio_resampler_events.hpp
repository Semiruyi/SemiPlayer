#pragma once

#include "domain/worker/audio_resampler/audio_resampler.hpp"

namespace semi::domain {

struct AudioResamplerBackendFailure {
    AudioResamplerBackendError error;
};

} // namespace semi::domain
