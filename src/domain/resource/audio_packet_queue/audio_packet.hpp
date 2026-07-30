#pragma once

#include "contracts/demuxer/packet/encoded_packet.hpp"
#include "domain/resource/generation/generation.hpp"

namespace semi::domain {

// One encoded audio packet in the playback pipeline. Technical media data is
// represented by a backend-neutral value; generation is its sole domain datum.
class AudioPacket final {
public:
    AudioPacket(contracts::demuxer::packet::EncodedPacket encoded_packet,
                Generation::Value generation) noexcept;

    ~AudioPacket() = default;

    AudioPacket(const AudioPacket&) = delete;
    AudioPacket& operator=(const AudioPacket&) = delete;
    AudioPacket(AudioPacket&&) noexcept = default;
    AudioPacket& operator=(AudioPacket&&) noexcept = default;

    [[nodiscard]] Generation::Value generation() const noexcept;
    [[nodiscard]] const contracts::demuxer::packet::EncodedPacket& encoded() const noexcept;

private:
    contracts::demuxer::packet::EncodedPacket encoded_packet_;
    Generation::Value generation_;
};

} // namespace semi::domain
