#include "domain/resource/audio_packet_queue/audio_packet.hpp"

#include <cassert>
#include <utility>

namespace semi::domain {

AudioPacket::AudioPacket(
    std::unique_ptr<contracts::demuxer::packet::EncodedPacket> encoded_packet,
    Generation::Value generation) noexcept
    : encoded_packet_(std::move(encoded_packet)), generation_(generation) {
    assert(encoded_packet_ != nullptr);
}

Generation::Value AudioPacket::generation() const noexcept {
    return generation_;
}

const contracts::demuxer::packet::EncodedPacket& AudioPacket::encoded() const noexcept {
    assert(encoded_packet_ != nullptr);
    return *encoded_packet_;
}

} // namespace semi::domain
