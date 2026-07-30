#include "domain/resource/audio_packet_queue/audio_packet.hpp"

#include <utility>

namespace semi::domain {

AudioPacket::AudioPacket(contracts::demuxer::packet::EncodedPacket encoded_packet,
                         Generation::Value generation) noexcept
    : encoded_packet_(std::move(encoded_packet)), generation_(generation) {}

Generation::Value AudioPacket::generation() const noexcept {
    return generation_;
}

const contracts::demuxer::packet::EncodedPacket& AudioPacket::encoded() const noexcept {
    return encoded_packet_;
}

} // namespace semi::domain
