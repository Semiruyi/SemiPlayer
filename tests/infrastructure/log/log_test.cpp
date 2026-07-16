#include "infrastructure/log/log.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#define SEMI_LOG_TAG "LoggerTest"

namespace semi::log {
namespace {

std::filesystem::path make_log_path(const char* suffix) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("semi_player_logger_" + std::to_string(stamp) + "_" + suffix + ".log");
}

class LoggerTest : public ::testing::Test {
protected:
    void TearDown() override {
        shutdown();
        for (const auto& path : paths_) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
            std::filesystem::remove(path.string() + ".1", ec);
        }
    }

    Config make_config(const std::filesystem::path& path) {
        paths_.push_back(path);

        Config config;
        config.file_path = path.string();
        config.level = Level::Info;
        config.console_level = Level::Off;
        config.queue_size = 256;
        config.worker_threads = 1;
        config.rotate_bytes = 1024 * 1024;
        config.rotate_files = 2;
        return config;
    }

    std::vector<std::filesystem::path> paths_;
};

TEST_F(LoggerTest, WritesMessageToRotatingFile) {
    const auto path = make_log_path("write");
    const auto config = make_config(path);

    EXPECT_EQ(init(config), InitResult::Ready);

    SEMI_LOG_INFO("message {}", 42);
    flush();
    shutdown();

    std::ifstream input(path);
    ASSERT_TRUE(input.is_open());

    const std::string content((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("message 42"), std::string::npos);
    EXPECT_NE(content.find("[LoggerTest]"), std::string::npos);
}

TEST_F(LoggerTest, RejectsRepeatedInitUntilShutdown) {
    const auto first = make_config(make_log_path("first"));
    const auto second = make_config(make_log_path("second"));

    EXPECT_EQ(init(first), InitResult::Ready);
    EXPECT_EQ(init(second), InitResult::AlreadyInitialized);

    shutdown();

    EXPECT_EQ(init(second), InitResult::Ready);
}

} // namespace
} // namespace semi::log
