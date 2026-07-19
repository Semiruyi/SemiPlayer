#pragma once

#include "infrastructure/notifier/notifier.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace semi::infra {

// 基于 std::type_index 的 Notifier 默认实现。
//
// 线程安全；send() 只在锁内复制订阅快照，回调在无锁状态下同步执行。
class DefaultNotifier final : public Notifier {
public:
    DefaultNotifier();
    ~DefaultNotifier() override;

    DefaultNotifier(const DefaultNotifier&) = delete;
    DefaultNotifier& operator=(const DefaultNotifier&) = delete;
    DefaultNotifier(DefaultNotifier&&) = delete;
    DefaultNotifier& operator=(DefaultNotifier&&) = delete;

    [[nodiscard]] bool clear_all() noexcept override;

protected:
    std::shared_ptr<Subscription> subscribe_erased(
        std::type_index type,
        std::function<void(const void*)> cb) override;

    bool send_erased(std::type_index type, const void* event) override;
    bool clear_erased(std::type_index type) noexcept override;

private:
    struct Slot;
    struct State;

    class SubscriptionImpl final : public Subscription {
    public:
        SubscriptionImpl(std::weak_ptr<State> state,
                         std::type_index type,
                         std::shared_ptr<Slot> slot);
        ~SubscriptionImpl() override;

        [[nodiscard]] bool unsubscribe() noexcept override;
        [[nodiscard]] bool active() const noexcept override;

    private:
        std::weak_ptr<State> state_;
        std::type_index type_;
        std::shared_ptr<Slot> slot_;
    };

    static void log_slow_callback(std::type_index type, std::chrono::microseconds elapsed) noexcept;

    std::shared_ptr<State> state_;
};

} // namespace semi::infra
