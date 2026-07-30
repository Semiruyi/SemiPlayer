#pragma once

#include "contracts/demuxer/packet/encoded_packet.hpp"
#include "contracts/media/media_types.hpp"

#include <variant>

namespace semi::contracts::demuxer::packet {

struct BackendPacket {
    media::DemuxerStreamId stream_id;
    EncodedPacket packet;
};

struct BackendEndOfStream {};

using BackendReadResult = std::variant<BackendPacket, BackendEndOfStream>;

} // namespace semi::contracts::demuxer::packet
