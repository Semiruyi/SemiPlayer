#include "infrastructure/notifier/default_notifier.hpp"

#include "infrastructure/log/log.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <mutex>
#include <typeindex>
#include <utility>

#define SEMI_LOG_TAG "notifier"

namespace semi::infra {
namespace {

constexpr auto kWarnCallbackCost = std::chrono::milliseconds{20};
constexpr auto kErrorCallbackCost = std::chrono::milliseconds{100};

} // namespace

struct DefaultNotifier::Slot {
    explicit Slot(std::function<void(const void*)> callback) : cb(std::move(callback)) {}

    std::atomic_bool active{true};
    std::function<void(const void*)> cb;
};

struct DefaultNotifier::State {
    std::mutex mu;
    std::unordered_map<std::type_index, std::vector<std::shared_ptr<Slot>>> slots;
};

DefaultNotifier::SubscriptionImpl::SubscriptionImpl(std::weak_ptr<State> state,
                                                        std::type_index type,
                                                        std::shared_ptr<Slot> slot)
    : state_(std::move(state)), type_(type), slot_(std::move(slot)) {}

DefaultNotifier::SubscriptionImpl::~SubscriptionImpl() {
    (void)unsubscribe();
}

bool DefaultNotifier::SubscriptionImpl::unsubscribe() noexcept {
    if (!slot_ || !slot_->active.exchange(false, std::memory_order_acq_rel)) {
        return false;
    }

    auto state = state_.lock();
    if (!state) {
        return false;
    }

    std::lock_guard<std::mutex> lock(state->mu);
    auto it = state->slots.find(type_);
    if (it == state->slots.end()) {
        return false;
    }

    auto& callbacks = it->second;
    const auto new_end = std::remove(callbacks.begin(), callbacks.end(), slot_);
    const bool removed = new_end != callbacks.end();
    callbacks.erase(new_end, callbacks.end());

    if (callbacks.empty()) {
        state->slots.erase(it);
    }

    return removed;
}

bool DefaultNotifier::SubscriptionImpl::active() const noexcept {
    return slot_ && slot_->active.load(std::memory_order_acquire);
}

DefaultNotifier::DefaultNotifier() : state_(std::make_shared<State>()) {}

DefaultNotifier::~DefaultNotifier() {
    (void)clear_all();
}

std::shared_ptr<Notifier::Subscription> DefaultNotifier::subscribe_erased(
    std::type_index type,
    std::function<void(const void*)> cb) {
    auto slot = std::make_shared<Slot>(std::move(cb));

    {
        std::lock_guard<std::mutex> lock(state_->mu);
        state_->slots[type].push_back(slot);
    }

    return std::make_shared<SubscriptionImpl>(state_, type, std::move(slot));
}

bool DefaultNotifier::send_erased(std::type_index type, const void* event) {
    std::vector<std::shared_ptr<Slot>> snapshot;
    {
        std::lock_guard<std::mutex> lock(state_->mu);
        const auto it = state_->slots.find(type);
        if (it == state_->slots.end()) {
            return false;
        }
        snapshot = it->second;
    }

    bool dispatched = false;
    for (const auto& slot : snapshot) {
        if (!slot->active.load(std::memory_order_acquire)) {
            continue;
        }

        dispatched = true;
        const auto begin = std::chrono::steady_clock::now();
        try {
            slot->cb(event);
        } catch (const std::exception& ex) {
            SEMI_LOG_ERROR("callback threw for event type {}: {}", type.name(), ex.what());
        } catch (...) {
            SEMI_LOG_ERROR("callback threw for event type {}: unknown exception", type.name());
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - begin);
        log_slow_callback(type, elapsed);
    }

    return dispatched;
}

bool DefaultNotifier::clear_erased(std::type_index type) noexcept {
    std::vector<std::shared_ptr<Slot>> removed;
    {
        std::lock_guard<std::mutex> lock(state_->mu);
        auto it = state_->slots.find(type);
        if (it == state_->slots.end()) {
            return false;
        }
        removed = std::move(it->second);
        state_->slots.erase(it);
    }

    for (const auto& slot : removed) {
        slot->active.store(false, std::memory_order_release);
    }

    return !removed.empty();
}

bool DefaultNotifier::clear_all() noexcept {
    std::vector<std::shared_ptr<Slot>> removed;
    {
        std::lock_guard<std::mutex> lock(state_->mu);
        for (auto& [_, callbacks] : state_->slots) {
            removed.insert(removed.end(), callbacks.begin(), callbacks.end());
        }
        state_->slots.clear();
    }

    for (const auto& slot : removed) {
        slot->active.store(false, std::memory_order_release);
    }

    return !removed.empty();
}

void DefaultNotifier::log_slow_callback(std::type_index type,
                                            std::chrono::microseconds elapsed) noexcept {
    if (elapsed > kErrorCallbackCost) {
        SEMI_LOG_ERROR(
            "slow callback for event type {} cost {}us",
            type.name(),
            elapsed.count());
        return;
    }

    if (elapsed > kWarnCallbackCost) {
        SEMI_LOG_WARN(
            "slow callback for event type {} cost {}us",
            type.name(),
            elapsed.count());
    }
}

} // namespace semi::infra
