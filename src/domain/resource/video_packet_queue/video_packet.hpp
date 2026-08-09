#pragma once

#include "contracts/demuxer/packet/encoded_packet.hpp"
#include "domain/resource/generation/generation.hpp"

namespace semi::domain {

// One encoded video packet in the playback pipeline. Generation is the
// domain-side invalidation marker; media data remains backend-neutral.
class VideoPacket final {
public:
    VideoPacket(contracts::demuxer::packet::EncodedPacket encoded_packet,
                Generation::Value generation) noexcept;

    ~VideoPacket() = default;

    VideoPacket(const VideoPacket&) = delete;
    VideoPacket& operator=(const VideoPacket&) = delete;
    VideoPacket(VideoPacket&&) noexcept = default;
    VideoPacket& operator=(VideoPacket&&) noexcept = default;

    [[nodiscard]] Generation::Value generation() const noexcept;
    [[nodiscard]] const contracts::demuxer::packet::EncodedPacket& encoded() const noexcept;

private:
    contracts::demuxer::packet::EncodedPacket encoded_packet_;
    Generation::Value generation_;
};

} // namespace semi::domain
