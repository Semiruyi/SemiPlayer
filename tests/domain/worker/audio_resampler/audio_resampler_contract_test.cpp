#include "domain/worker/audio_resampler/audio_resampler_events.hpp"

#include <gtest/gtest.h>

namespace semi::domain {
namespace {

TEST(AudioResamplerEvents, CarriesBackendFailure) {
    const AudioResamplerBackendFailure failure{
        .error = AudioResamplerBackendError{
            .operation = AudioResamplerBackendOperation::Resample,
            .native_code = -1,
            .message = "invalid pcm",
        },
    };

    EXPECT_EQ(failure.error.operation, AudioResamplerBackendOperation::Resample);
    EXPECT_EQ(failure.error.message, "invalid pcm");
}

TEST(AudioResamplerError, CanCarryBackendFailureDetails) {
    const AudioResamplerError error{
        .code = AudioResamplerErrorCode::BackendFailure,
        .message = "audio resampler backend failed",
        .backend_error = AudioResamplerBackendError{
            .operation = AudioResamplerBackendOperation::Configure,
            .native_code = -22,
            .message = "unsupported format",
        },
    };

    ASSERT_TRUE(error.backend_error.has_value());
    EXPECT_EQ(error.code, AudioResamplerErrorCode::BackendFailure);
    EXPECT_EQ(error.backend_error->operation, AudioResamplerBackendOperation::Configure);
    EXPECT_EQ(error.backend_error->message, "unsupported format");
}

} // namespace
} // namespace semi::domain
