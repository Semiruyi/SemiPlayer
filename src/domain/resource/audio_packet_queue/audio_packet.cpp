#include "domain/resource/audio_packet_queue/audio_packet.hpp"

#include <cassert>
#include <utility>

namespace semi::domain {

AudioPacket::AudioPacket(
    std::unique_ptr<contracts::demuxer::packet::EncodedAudioPacket> encoded_packet,
    Generation::Value generation) noexcept
    : encoded_packet_(std::move(encoded_packet)), generation_(generation) {
    assert(encoded_packet_ != nullptr);
}

Generation::Value AudioPacket::generation() const noexcept {
    return generation_;
}

const contracts::demuxer::packet::EncodedAudioPacket& AudioPacket::encoded() const noexcept {
    assert(encoded_packet_ != nullptr);
    return *encoded_packet_;
}

} // namespace semi::domain
