#pragma once

#include "domain/worker/demuxer/demuxer.hpp"

#include <memory>

namespace semi::domain {

class DefaultDemuxer final : public Demuxer {
public:
    explicit DefaultDemuxer(std::shared_ptr<DemuxerBackend> backend);
    ~DefaultDemuxer() override;

    [[nodiscard]] std::expected<DemuxerOpenResult, DemuxerError>
    open(std::string_view source) override;

    void close() noexcept override;

private:
    std::shared_ptr<DemuxerBackend> backend_;
    bool opened_ = false;
};

} // namespace semi::domain
