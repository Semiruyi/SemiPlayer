#include "ioc/ioc_container.hpp"

#include "application/api_layer.hpp"

#include <gtest/gtest.h>

namespace semi::application {
namespace {

TEST(IoCPipelineTest, AssemblesAndOpensSampleThroughNullAudioOutput) {
    auto& container = ioc::IoCContainer::instance();
    ASSERT_TRUE(container.dispose());
    ASSERT_TRUE(container.assemble());

    auto api_layer = container.api_layer();
    ASSERT_NE(api_layer, nullptr);

    const CommandHandle open = api_layer->open(SEMI_PLAYER_TEST_MEDIA_PATH);
    ASSERT_NE(open, 0U);

    CommandResult result;
    EXPECT_EQ(api_layer->await(open, result), SEMI_OK);
    EXPECT_TRUE(result.has_media_info);
    EXPECT_TRUE(result.media_info.has_audio);

    const CommandHandle close = api_layer->close();
    ASSERT_NE(close, 0U);
    EXPECT_EQ(api_layer->await(close, result), SEMI_OK);

    EXPECT_TRUE(container.dispose());
}

} // namespace
} // namespace semi::application
