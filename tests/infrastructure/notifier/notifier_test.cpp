#include "infrastructure/notifier/default_notifier.hpp"

#include "infrastructure/log/log.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#define SEMI_LOG_TAG "NotifierTest"

namespace semi::infra {
namespace {

struct EventA {
    int value;
};

struct EventB {
    int value;
};

std::filesystem::path make_log_path(const char* suffix) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("semi_player_notifier_" + std::to_string(stamp) + "_" + suffix + ".log");
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input.is_open()) {
        return {};
    }
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

class NotifierTest : public ::testing::Test {
protected:
    void TearDown() override {
        semi::log::shutdown();
        for (const auto& path : paths_) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
            std::filesystem::remove(path.string() + ".1", ec);
        }
    }

    semi::log::Config make_log_config(const std::filesystem::path& path) {
        paths_.push_back(path);

        semi::log::Config config;
        config.file_path = path.string();
        config.level = semi::log::Level::Info;
        config.console_level = semi::log::Level::Off;
        config.queue_size = 256;
        config.worker_threads = 1;
        config.rotate_bytes = 1024 * 1024;
        config.rotate_files = 2;
        return config;
    }

private:
    std::vector<std::filesystem::path> paths_;
};

TEST_F(NotifierTest, SendsOnlyMatchingEventType) {
    DefaultNotifier notifier;
    int sum = 0;
    int other = 0;

    auto sub_a = notifier.subscribe<EventA>([&sum](const EventA& event) {
        sum += event.value;
    });
    auto sub_b = notifier.subscribe<EventB>([&other](const EventB& event) {
        other += event.value;
    });

    EXPECT_TRUE(notifier.send(EventA{3}));

    EXPECT_TRUE(sub_a->active());
    EXPECT_TRUE(sub_b->active());
    EXPECT_EQ(sum, 3);
    EXPECT_EQ(other, 0);
}

TEST_F(NotifierTest, UnsubscribeIsIdempotentAndPreventsFutureCallbacks) {
    DefaultNotifier notifier;
    int calls = 0;

    auto sub = notifier.subscribe<EventA>([&calls](const EventA&) {
        ++calls;
    });

    EXPECT_TRUE(notifier.send(EventA{1}));
    EXPECT_TRUE(sub->unsubscribe());
    EXPECT_FALSE(sub->unsubscribe());
    EXPECT_FALSE(notifier.send(EventA{1}));

    EXPECT_FALSE(sub->active());
    EXPECT_EQ(calls, 1);
}

TEST_F(NotifierTest, SubscriptionDestructorUnsubscribes) {
    DefaultNotifier notifier;
    int calls = 0;

    {
        auto sub = notifier.subscribe<EventA>([&calls](const EventA&) {
            ++calls;
        });
        ASSERT_TRUE(sub->active());
    }

    EXPECT_FALSE(notifier.send(EventA{1}));

    EXPECT_EQ(calls, 0);
}

TEST_F(NotifierTest, UnsubscribingOneSubscriptionKeepsSameTypeCallbacks) {
    DefaultNotifier notifier;
    int first_calls = 0;
    int second_calls = 0;

    auto first = notifier.subscribe<EventA>([&first_calls](const EventA&) {
        ++first_calls;
    });
    auto second = notifier.subscribe<EventA>([&second_calls](const EventA&) {
        ++second_calls;
    });

    EXPECT_TRUE(first->unsubscribe());
    EXPECT_TRUE(notifier.send(EventA{1}));

    EXPECT_FALSE(first->active());
    EXPECT_TRUE(second->active());
    EXPECT_EQ(first_calls, 0);
    EXPECT_EQ(second_calls, 1);
}

TEST_F(NotifierTest, SendUsesSnapshotAndAllowsMutationInsideCallback) {
    DefaultNotifier notifier;
    int first_calls = 0;
    int second_calls = 0;
    int late_calls = 0;
    std::shared_ptr<Notifier::Subscription> second;
    std::vector<std::shared_ptr<Notifier::Subscription>> late_subs;

    auto first = notifier.subscribe<EventA>([&](const EventA&) {
        ++first_calls;
        (void)second->unsubscribe();
        late_subs.push_back(notifier.subscribe<EventA>([&late_calls](const EventA&) {
            ++late_calls;
        }));
    });
    second = notifier.subscribe<EventA>([&](const EventA&) {
        ++second_calls;
    });

    EXPECT_TRUE(notifier.send(EventA{1}));
    EXPECT_TRUE(notifier.send(EventA{1}));

    EXPECT_TRUE(first->active());
    EXPECT_FALSE(second->active());
    EXPECT_EQ(first_calls, 2);
    EXPECT_EQ(second_calls, 0);
    EXPECT_EQ(late_calls, 1);
}

TEST_F(NotifierTest, ClearDisablesOneEventType) {
    DefaultNotifier notifier;
    int a_calls = 0;
    int b_calls = 0;

    auto sub_a = notifier.subscribe<EventA>([&a_calls](const EventA&) {
        ++a_calls;
    });
    auto sub_b = notifier.subscribe<EventB>([&b_calls](const EventB&) {
        ++b_calls;
    });

    EXPECT_TRUE(notifier.clear<EventA>());
    EXPECT_FALSE(notifier.clear<EventA>());
    EXPECT_FALSE(notifier.send(EventA{1}));
    EXPECT_TRUE(notifier.send(EventB{1}));

    EXPECT_FALSE(sub_a->active());
    EXPECT_TRUE(sub_b->active());
    EXPECT_EQ(a_calls, 0);
    EXPECT_EQ(b_calls, 1);
}

TEST_F(NotifierTest, ClearAllDisablesAllSubscriptions) {
    DefaultNotifier notifier;
    auto sub_a = notifier.subscribe<EventA>([](const EventA&) {});
    auto sub_b = notifier.subscribe<EventB>([](const EventB&) {});

    EXPECT_TRUE(notifier.clear_all());
    EXPECT_FALSE(notifier.clear_all());

    EXPECT_FALSE(sub_a->active());
    EXPECT_FALSE(sub_b->active());
}

TEST_F(NotifierTest, InterfacesAreThreadSafeUnderConcurrentUse) {
    DefaultNotifier notifier;
    std::atomic<int> calls{0};
    constexpr int kThreads = 8;
    constexpr int kIterations = 500;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&] {
            for (int j = 0; j < kIterations; ++j) {
                auto sub = notifier.subscribe<EventA>([&calls](const EventA&) {
                    calls.fetch_add(1, std::memory_order_relaxed);
                });
                (void)notifier.send(EventA{j});
                if (j % 2 == 0) {
                    (void)sub->unsubscribe();
                }
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_GT(calls.load(std::memory_order_relaxed), 0);
}

TEST_F(NotifierTest, LogsWarningForCallbackOver20ms) {
    const auto path = make_log_path("warn");
    ASSERT_EQ(semi::log::init(make_log_config(path)), semi::log::InitResult::Ready);

    DefaultNotifier notifier;
    auto sub = notifier.subscribe<EventA>([](const EventA&) {
        std::this_thread::sleep_for(std::chrono::milliseconds{25});
    });

    EXPECT_TRUE(notifier.send(EventA{1}));
    semi::log::flush();
    semi::log::shutdown();

    const auto content = read_file(path);
    EXPECT_NE(content.find("[warning]"), std::string::npos);
    EXPECT_NE(content.find("slow callback"), std::string::npos);
}

TEST_F(NotifierTest, LogsErrorForCallbackOver100ms) {
    const auto path = make_log_path("error");
    ASSERT_EQ(semi::log::init(make_log_config(path)), semi::log::InitResult::Ready);

    DefaultNotifier notifier;
    auto sub = notifier.subscribe<EventA>([](const EventA&) {
        std::this_thread::sleep_for(std::chrono::milliseconds{105});
    });

    EXPECT_TRUE(notifier.send(EventA{1}));
    semi::log::flush();
    semi::log::shutdown();

    const auto content = read_file(path);
    EXPECT_NE(content.find("[error]"), std::string::npos);
    EXPECT_NE(content.find("slow callback"), std::string::npos);
}

} // namespace
} // namespace semi::infra
