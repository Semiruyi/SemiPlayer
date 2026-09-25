#include "domain/resource/generation/generation.hpp"

#include "domain/resource/generation/generation_events.hpp"
#include "infrastructure/notifier/notifier.hpp"

#include <utility>

namespace semi::domain {

Generation::Generation(std::shared_ptr<infra::Notifier> notifier) noexcept
    : notifier_(std::move(notifier)) {}

Generation::Value Generation::bump(std::optional<std::int64_t> seek_target_pts_us) noexcept {
    Value next = 0;
    {
        // Serialize writers so the published value and its seek context cannot be paired
        // with different concurrent bumps.
        std::lock_guard lock(context_mutex_);
        next = value_.load(std::memory_order_relaxed) + 1;
        context_ = Snapshot{
            .value = next,
            .seek_target_pts_us = seek_target_pts_us,
        };
        value_.store(next, std::memory_order_release);
    }
    if (notifier_) {
        try {
            (void)notifier_->send(GenerationChanged{.value = next});
        } catch (...) {
            // Consumers also compare current(), so a failed wake-up cannot lose the transition.
        }
    }
    return next;
}

std::optional<std::int64_t> Generation::seek_target_for(Value generation) const noexcept {
    std::lock_guard lock(context_mutex_);
    if (context_.value != generation) {
        return std::nullopt;
    }
    return context_.seek_target_pts_us;
}

Generation::Snapshot Generation::snapshot() const noexcept {
    std::lock_guard lock(context_mutex_);
    return context_;
}

Generation::Value Generation::current() const noexcept {
    return value_.load(std::memory_order_acquire);
}

bool Generation::is_current(Value gen) const noexcept {
    return gen == current();
}

} // namespace semi::domain
