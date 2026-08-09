#include "domain/resource/video_frame_store/video_frame_store_item.hpp"
#include "domain/worker/video_decoder/video_decoder_events.hpp"

#include <gtest/gtest.h>

namespace semi::domain {
namespace {

TEST(VideoDecoderEvents, CarriesBackendFailure) {
    const VideoDecoderBackendFailure failure{
        .error = VideoDecoderBackendError{
            .operation = VideoDecoderBackendOperation::Decode,
            .native_code = -1,
            .message = "invalid packet",
        },
    };

    EXPECT_EQ(failure.error.operation, VideoDecoderBackendOperation::Decode);
    EXPECT_EQ(failure.error.message, "invalid packet");
}

TEST(VideoFrameEndOfInput, CarriesItsGeneration) {
    const VideoFrameStoreItem end_of_input = VideoFrameEndOfInput{.generation = 7};

    EXPECT_EQ(video_frame_store_item_generation(end_of_input), 7U);
    EXPECT_TRUE(is_current_video_frame_store_item(end_of_input, 7));
    EXPECT_FALSE(is_current_video_frame_store_item(end_of_input, 8));
}

} // namespace
} // namespace semi::domain
