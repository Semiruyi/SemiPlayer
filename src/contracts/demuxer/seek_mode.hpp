#pragma once

#include <cstdint>

namespace semi::contracts::demuxer {

enum class SeekMode : std::uint8_t {
    Unknown,
    PreviousKeyframe,
    NextKeyframe,
};

} // namespace semi::contracts::demuxer
