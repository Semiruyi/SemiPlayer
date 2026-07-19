#include "application/api_layer.hpp"

#include <gtest/gtest.h>

namespace semi::application {
namespace {

TEST(ApiLayerTest, CommandsCompleteOnWorkerWithInternalUntilMediaIsConnected) {
    ApiLayer layer;
    ASSERT_TRUE(layer.start());

    const CommandHandle handle = layer.open("movie.mp4");
    ASSERT_NE(handle, 0U);

    CommandResult result;
    EXPECT_EQ(layer.await(handle, result), SEMI_ERR_INTERNAL);
    EXPECT_FALSE(result.has_media_info);
    EXPECT_TRUE(layer.stop());
}

TEST(ApiLayerTest, AwaitConsumesHandle) {
    ApiLayer layer;
    ASSERT_TRUE(layer.start());

    const CommandHandle handle = layer.play();
    ASSERT_NE(handle, 0U);

    CommandResult result;
    EXPECT_EQ(layer.await(handle, result), SEMI_ERR_INTERNAL);
    EXPECT_EQ(layer.await(handle, result), SEMI_ERR_INVALID_HANDLE);
    EXPECT_TRUE(layer.stop());
}

TEST(ApiLayerTest, StartAndStopAreIdempotent) {
    ApiLayer layer;
    EXPECT_TRUE(layer.start());
    EXPECT_TRUE(layer.start());
    EXPECT_TRUE(layer.stop());
    EXPECT_TRUE(layer.stop());
}

} // namespace
} // namespace semi::application
