#include "infrastructure/statistics/performance_statistics.hpp"
#include "infrastructure/notifier/default_notifier.hpp"
#include "domain/worker/video_decoder/video_decoder_statistics.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace semi::infra {
namespace {
class PerformanceStatisticsTest : public ::testing::Test {
protected:
    void SetUp() override {
        directory = std::filesystem::temp_directory_path() /
            ("semi-statistics-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(directory);
        path = directory / "report.jsonl";
    }
    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }
    std::vector<std::string> lines() {
        std::ifstream file(path);
        std::vector<std::string> result;
        for (std::string line; std::getline(file, line);) result.push_back(std::move(line));
        return result;
    }
    std::filesystem::path directory, path;
};

TEST_F(PerformanceStatisticsTest, DrainsOwnedSnapshotsAndEscapesJson) {
    auto notifier = std::make_shared<DefaultNotifier>();
    {
        PerformanceStatistics statistics(notifier, path);
        domain::VideoDecoderStatistics report;
        report.codec = "h264\"\\\n\t";
        report.decode_calls = 2;
        report.drain_calls = 1;
        report.output_frames = 2;
        report.processing_total_ns = 9'000'000;
        report.processing_max_ns = 5'000'000;
        report.output_observed = true;
        report.output_format = contracts::media::VideoPixelFormat::Nv12;
        report.output_width = 1920;
        report.output_height = 1080;
        ASSERT_TRUE(notifier->send(report));
        report.codec = "changed"; // The queue must own its copy.
        report.output_frames = 0;
        report.decode_calls = 0;
        report.drain_calls = 0;
        report.output_observed = false;
        ASSERT_TRUE(notifier->send(report));
    }
    const auto result = lines();
    ASSERT_EQ(result.size(), 2U);
    EXPECT_NE(result[0].find("\"codec\":\"h264\\\"\\\\\\u000a\\u0009\""), std::string::npos);
    EXPECT_NE(result[0].find("\"processing_mean_call_ms\":3.000000"), std::string::npos);
    EXPECT_NE(result[0].find("\"processing_per_output_frame_ms\":4.500000"), std::string::npos);
    EXPECT_NE(result[0].find("\"output_format\":\"nv12\""), std::string::npos);
    EXPECT_NE(result[1].find("\"processing_mean_call_ms\":null"), std::string::npos);
    EXPECT_NE(result[1].find("\"output_format\":null"), std::string::npos);
    EXPECT_FALSE(notifier->send(domain::VideoDecoderStatistics{}));
}

TEST_F(PerformanceStatisticsTest, AcceptsConcurrentProducersAndDrainsBeforeDestruction) {
    auto notifier = std::make_shared<DefaultNotifier>();
    {
        PerformanceStatistics statistics(notifier, path);
        std::vector<std::thread> producers;
        for (int i = 0; i < 4; ++i) {
            producers.emplace_back([&] {
                for (int j = 0; j < 8; ++j)
                    (void)notifier->send(domain::VideoDecoderStatistics{});
            });
        }
        for (auto& producer : producers) producer.join();
    }
    EXPECT_EQ(lines().size(), 32U); // Below the bounded queue capacity.
}

TEST_F(PerformanceStatisticsTest, InvalidDestinationDoesNotFailProducerOrShutdown) {
    auto notifier = std::make_shared<DefaultNotifier>();
    {
        PerformanceStatistics statistics(notifier, directory); // A directory cannot be opened as a file.
        EXPECT_TRUE(notifier->send(domain::VideoDecoderStatistics{}));
        EXPECT_TRUE(notifier->send(domain::VideoDecoderStatistics{}));
    }
    EXPECT_FALSE(notifier->send(domain::VideoDecoderStatistics{}));
}

TEST_F(PerformanceStatisticsTest, DoesNotCreateAnEmptyFileWithoutReports) {
    auto notifier = std::make_shared<DefaultNotifier>();
    { PerformanceStatistics statistics(notifier, path); }
    EXPECT_FALSE(std::filesystem::exists(path));
}
} // namespace
} // namespace semi::infra
