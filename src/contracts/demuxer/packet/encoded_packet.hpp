#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace semi::contracts::demuxer::packet {

// Backend-neutral, owning encoded media packet. Payload is copied at the
// demuxer boundary, so queued packets have no native backend lifetime.
//
// Timestamp contract:
// - pts_us is the presentation timestamp on the media timeline, in
//   microseconds. It may be absent when the backend has no valid PTS.
// - dts_us is the decode timestamp on the media timeline, in microseconds.
//   It may be absent when the backend has no valid DTS.
// - duration_us is the packet duration in microseconds. It may be absent
//   when the backend has no valid duration.
// - Timestamps may be negative and must not be clamped by implementations.
// - PTS and DTS are independent values; callers must not assume pts >= dts.
// The backend is responsible for converting its native timestamp/time base to
// this contract and mapping an invalid native timestamp to std::nullopt.
struct EncodedPacket {
    std::vector<std::byte> payload;
    std::optional<std::int64_t> pts_us;
    std::optional<std::int64_t> dts_us;
    std::optional<std::int64_t> duration_us;
};

} // namespace semi::contracts::demuxer::packet
