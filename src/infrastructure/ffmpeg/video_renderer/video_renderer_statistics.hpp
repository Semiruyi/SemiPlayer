#pragma once

#include "contracts/media/media_types.hpp"

#include <cstdint>

namespace semi::infra::ffmpeg::video_renderer {

struct RenderTiming {
    std::uint64_t calls = 0;
    std::uint64_t total_ns = 0;
    std::uint64_t max_ns = 0;
};

// Owned snapshot for one successful configure -> unconfigure interval.
// Reset keeps accumulating. Only configured render calls are counted.
struct VideoRendererStatistics {
    std::int64_t configured_at_unix_ms = 0;
    std::int64_t finished_at_unix_ms = 0;
    std::uint64_t configured_duration_ns = 0;
    std::uint64_t successful_calls = 0;
    std::uint64_t failed_calls = 0;
    std::uint64_t output_bytes = 0;
    bool input_observed = false;
    bool format_changed = false;
    contracts::media::VideoPixelFormat input_format = contracts::media::VideoPixelFormat::Unknown;
    std::uint32_t input_width = 0;
    std::uint32_t input_height = 0;
    std::uint32_t output_width = 0;
    std::uint32_t output_height = 0;
    RenderTiming total;
    RenderTiming readback;
    RenderTiming create_scaler;
    RenderTiming allocate_rgba;
    RenderTiming scale;
};

} // namespace semi::infra::ffmpeg::video_renderer
