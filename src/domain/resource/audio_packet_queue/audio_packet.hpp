#pragma once

#include "contracts/audio/encoded_audio_packet.hpp"
#include "domain/resource/generation/generation.hpp"

#include <memory>

namespace semi::domain {

// One encoded audio packet in the playback pipeline. Technical media data is
// owned by the backend-neutral contract; generation is its sole domain datum.
class AudioPacket final {
public:
    AudioPacket(
        std::unique_ptr<contracts::audio::EncodedAudioPacket> encoded_packet,
        Generation::Value generation) noexcept;

    ~AudioPacket() = default;

    AudioPacket(const AudioPacket&) = delete;
    AudioPacket& operator=(const AudioPacket&) = delete;
    AudioPacket(AudioPacket&&) noexcept = default;
    AudioPacket& operator=(AudioPacket&&) noexcept = default;

    [[nodiscard]] Generation::Value generation() const noexcept;
    [[nodiscard]] const contracts::audio::EncodedAudioPacket& encoded() const noexcept;

private:
    std::unique_ptr<contracts::audio::EncodedAudioPacket> encoded_packet_;
    Generation::Value generation_;
};

} // namespace semi::domain
