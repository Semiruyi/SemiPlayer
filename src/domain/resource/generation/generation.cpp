#include "domain/resource/generation/generation.hpp"

#include "domain/resource/generation/generation_events.hpp"
#include "infrastructure/notifier/notifier.hpp"

#include <utility>

namespace semi::domain {

Generation::Generation(std::shared_ptr<infra::Notifier> notifier) noexcept : notifier_(std::move(notifier)) {}

Generation::Value Generation::bump() noexcept {
    const Value next = value_.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (notifier_) {
        try {
            (void)notifier_->send(GenerationChanged{.value = next});
        } catch (...) {
            // Consumers also compare current(), so a failed wake-up cannot lose the transition.
        }
    }
    return next;
}

Generation::Value Generation::current() const noexcept {
    return value_.load(std::memory_order_acquire);
}

bool Generation::is_current(Value gen) const noexcept {
    return gen == current();
}

} // namespace semi::domain
