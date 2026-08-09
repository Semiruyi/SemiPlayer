#pragma once

#include "domain/worker/video_decoder/video_decoder.hpp"

namespace semi::domain {

struct VideoDecoderBackendFailure {
    VideoDecoderBackendError error;
};

} // namespace semi::domain
