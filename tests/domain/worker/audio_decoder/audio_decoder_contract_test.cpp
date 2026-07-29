#include "domain/resource/audio_frame_store/audio_frame_store_item.hpp"
#include "domain/worker/audio_decoder/audio_decoder_events.hpp"

#include <gtest/gtest.h>

namespace semi::domain {
namespace {

TEST(AudioDecoderEvents, CarriesBackendFailure) {
    const AudioDecoderBackendFailure failure{
        .error = AudioDecoderBackendError{
            .operation = AudioDecoderBackendOperation::Decode,
            .native_code = -1,
            .message = "invalid packet",
        },
    };

    EXPECT_EQ(failure.error.operation, AudioDecoderBackendOperation::Decode);
    EXPECT_EQ(failure.error.message, "invalid packet");
}

TEST(AudioFrameEndOfInput, CarriesItsGeneration) {
    const AudioFrameStoreItem end_of_input = AudioFrameEndOfInput{.generation = 7};

    EXPECT_EQ(audio_frame_store_item_generation(end_of_input), 7U);
    EXPECT_TRUE(is_current_audio_frame_store_item(end_of_input, 7));
    EXPECT_FALSE(is_current_audio_frame_store_item(end_of_input, 8));
}

} // namespace
} // namespace semi::domain
