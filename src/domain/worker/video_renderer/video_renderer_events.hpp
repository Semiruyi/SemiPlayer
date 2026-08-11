#pragma once

#include "domain/worker/video_renderer/video_renderer.hpp"

namespace semi::domain {

struct VideoRendererBackendFailure {
    VideoRendererBackendError error;
};

} // namespace semi::domain
