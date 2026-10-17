#pragma once

#include "contracts/media/media_types.hpp"

#include <cstdint>
#include <string>

namespace semi::domain {

enum class VideoDecoderStatisticsReason : std::uint8_t {
    Unconfigure, BackendFailure, Shutdown,
};

// One successful configure-to-unconfigure interval. Owned values only: this
// event never retains frames, codec contexts, or references into the worker.
struct VideoDecoderStatistics {
    VideoDecoderStatisticsReason reason = VideoDecoderStatisticsReason::Unconfigure;
    std::string codec;
    std::uint32_t coded_width = 0;
    std::uint32_t coded_height = 0;
    std::int64_t configured_at_unix_ms = 0;
    std::int64_t finished_at_unix_ms = 0;
    std::uint64_t configured_duration_ns = 0;
    std::uint64_t decode_calls = 0;
    std::uint64_t drain_calls = 0;
    std::uint64_t failed_calls = 0;
    std::uint64_t output_frames = 0;
    std::uint64_t processing_total_ns = 0;
    std::uint64_t processing_max_ns = 0;
    bool output_observed = false;
    contracts::media::VideoPixelFormat output_format = contracts::media::VideoPixelFormat::Unknown;
    std::uint32_t output_width = 0;
    std::uint32_t output_height = 0;
    bool format_changed = false;
};

} // namespace semi::domain
