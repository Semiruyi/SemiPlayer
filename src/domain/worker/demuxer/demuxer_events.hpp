#pragma once

#include "domain/worker/demuxer/demuxer.hpp"

namespace semi::domain {

struct DemuxerReadError {
    DemuxerBackendError error;
};

} // namespace semi::domain
