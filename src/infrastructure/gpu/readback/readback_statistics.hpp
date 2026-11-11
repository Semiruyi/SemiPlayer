#pragma once

#include "contracts/media/media_types.hpp"

#include <cstdint>
#include <string>

namespace semi::infra::gpu {

struct ReadbackTiming {
    std::uint64_t calls = 0;
    std::uint64_t total_ns = 0;
    std::uint64_t max_ns = 0;
};

// Owned snapshot for one readback instance, from first call to destruction.
// No domain/native resources and no session identifier. Calls are sequential,
// just like VideoReadback itself. Phase counters include failed attempts.
struct ReadbackStatistics {
    std::string backend;
    std::int64_t started_at_unix_ms = 0;
    std::int64_t finished_at_unix_ms = 0;
    std::uint64_t observed_duration_ns = 0;
    std::uint64_t successful_calls = 0;
    std::uint64_t failed_calls = 0;
    std::uint64_t copied_bytes = 0;
    contracts::media::VideoPixelFormat input_format = contracts::media::VideoPixelFormat::Unknown;
    std::uint32_t input_width = 0;
    std::uint32_t input_height = 0;
    bool input_changed = false;
    ReadbackTiming total;
    ReadbackTiming allocate_cpu;
    ReadbackTiming create_staging;
    ReadbackTiming wait_context;
    ReadbackTiming submit_copy;
    ReadbackTiming map;
    ReadbackTiming copy_cpu;
    ReadbackTiming unmap;
};

} // namespace semi::infra::gpu
