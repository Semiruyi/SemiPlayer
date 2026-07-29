#pragma once

#include "domain/worker/audio_decoder/audio_decoder.hpp"

namespace semi::domain {

struct AudioDecoderBackendFailure {
    AudioDecoderBackendError error;
};

} // namespace semi::domain
