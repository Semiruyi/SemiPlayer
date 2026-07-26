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

    [[nodiscard]] std::expected<void, DemuxerError> start() override;

    void stop() noexcept override;

    [[nodiscard]] std::expected<void, DemuxerError>
    seek(std::int64_t position_us) override;

    void close() noexcept override;

private:
    std::shared_ptr<DemuxerBackend> backend_;
    bool opened_ = false;
    bool started_ = false;
    std::optional<std::int64_t> pending_seek_position_us_;
};

} // namespace semi::domain
