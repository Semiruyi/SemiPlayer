#pragma once

#include "domain/resource/audio_frame_store/audio_frame.hpp"

#include <variant>

namespace semi::domain {

// Ordered control item indicating that no more decoded PCM will be produced
// for this generation. Consumers must check its generation before draining.
struct AudioFrameEndOfInput {
    Generation::Value generation = 0;
};

using AudioFrameStoreItem = std::variant<AudioFrame, AudioFrameEndOfInput>;

[[nodiscard]] inline Generation::Value
audio_frame_store_item_generation(const AudioFrameStoreItem& item) noexcept {
    if (const auto* frame = std::get_if<AudioFrame>(&item)) {
        return frame->generation();
    }
    return std::get<AudioFrameEndOfInput>(item).generation;
}

[[nodiscard]] inline bool is_current_audio_frame_store_item(
    const AudioFrameStoreItem& item, Generation::Value current_generation) noexcept {
    return audio_frame_store_item_generation(item) == current_generation;
}

} // namespace semi::domain
