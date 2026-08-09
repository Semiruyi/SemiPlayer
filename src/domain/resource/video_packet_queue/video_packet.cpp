#include "domain/resource/video_packet_queue/video_packet.hpp"

#include <utility>

namespace semi::domain {

VideoPacket::VideoPacket(contracts::demuxer::packet::EncodedPacket encoded_packet,
                         Generation::Value generation) noexcept
    : encoded_packet_(std::move(encoded_packet)), generation_(generation) {}

Generation::Value VideoPacket::generation() const noexcept {
    return generation_;
}

const contracts::demuxer::packet::EncodedPacket& VideoPacket::encoded() const noexcept {
    return encoded_packet_;
}

} // namespace semi::domain
