#include "domain/worker/audio_output/audio_output_events.hpp"

#include <gtest/gtest.h>

namespace semi::domain {
namespace {

TEST(AudioOutputEvents, PlaybackFinishedCarriesGeneration) {
    const AudioPlaybackFinished event{.generation = 42};

    EXPECT_EQ(event.generation, 42U);
}

TEST(AudioOutputEvents, BackendFailureCarriesGenerationAndError) {
    const AudioOutputBackendFailure failure{
        .generation = 7,
        .error = AudioOutputBackendError{
            .operation = AudioOutputBackendOperation::Drain,
            .native_code = -1,
            .message = "device drain failed",
        },
    };

    EXPECT_EQ(failure.generation, 7U);
    EXPECT_EQ(failure.error.operation, AudioOutputBackendOperation::Drain);
    EXPECT_EQ(failure.error.message, "device drain failed");
}

TEST(AudioOutputError, CanCarryBackendFailureDetails) {
    const AudioOutputError error{
        .code = AudioOutputErrorCode::BackendFailure,
        .message = "audio output backend failed",
        .backend_error = AudioOutputBackendError{
            .operation = AudioOutputBackendOperation::Configure,
            .native_code = -22,
            .message = "unsupported device",
        },
    };

    ASSERT_TRUE(error.backend_error.has_value());
    EXPECT_EQ(error.code, AudioOutputErrorCode::BackendFailure);
    EXPECT_EQ(error.backend_error->operation, AudioOutputBackendOperation::Configure);
    EXPECT_EQ(error.backend_error->message, "unsupported device");
}

} // namespace
} // namespace semi::domain
