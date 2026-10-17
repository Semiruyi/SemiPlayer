#pragma once

#include "infrastructure/notifier/notifier.hpp"

#include <filesystem>
#include <memory>
#include <thread>

namespace semi::infra {

// Subscribes at construction. Destruction stops accepting reports, drains the
// queue and joins the writer. Producers must be stopped before destruction.
class PerformanceStatistics final {
public:
    explicit PerformanceStatistics(std::shared_ptr<Notifier> notifier,
                                   std::filesystem::path output_path = {});
    ~PerformanceStatistics();
    PerformanceStatistics(const PerformanceStatistics&) = delete;
    PerformanceStatistics& operator=(const PerformanceStatistics&) = delete;
    PerformanceStatistics(PerformanceStatistics&&) = delete;
    PerformanceStatistics& operator=(PerformanceStatistics&&) = delete;

private:
    struct State;
    static void write_reports(const std::shared_ptr<State>& state,
                              const std::filesystem::path& output_path) noexcept;
    std::shared_ptr<State> state_;
    std::shared_ptr<Notifier::Subscription> decoder_subscription_;
    std::thread writer_;
};

} // namespace semi::infra
