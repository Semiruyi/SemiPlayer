#pragma once

#include "domain/resource/video_rendered_store/rendered_video_frame.hpp"

namespace semi::domain {

// The presentation boundary consumes one fully rendered frame synchronously.
// A host adapter may copy the pixels into a texture or another host-owned
// buffer; the core does not retain the frame after this call returns.
class VideoPresentationSink {
public:
    virtual ~VideoPresentationSink() = default;

    VideoPresentationSink(const VideoPresentationSink&) = delete;
    VideoPresentationSink& operator=(const VideoPresentationSink&) = delete;
    VideoPresentationSink(VideoPresentationSink&&) = delete;
    VideoPresentationSink& operator=(VideoPresentationSink&&) = delete;

    virtual void present(RenderedVideoFrame&& frame) noexcept = 0;
    virtual void end_of_input(Generation::Value generation) noexcept = 0;

protected:
    VideoPresentationSink() = default;
};

} // namespace semi::domain
