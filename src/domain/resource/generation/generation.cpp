#include "domain/resource/generation/generation.hpp"

namespace semi::domain {

Generation::Generation() noexcept = default;

void Generation::bump() noexcept {
    value_.fetch_add(1, std::memory_order_acq_rel);
}

Generation::Value Generation::current() const noexcept {
    return value_.load(std::memory_order_acquire);
}

bool Generation::is_current(Value gen) const noexcept {
    return gen == current();
}

} // namespace semi::domain
