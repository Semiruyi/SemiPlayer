#include "infrastructure/audio_output/byte_spsc_ring.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstring>
#include <vector>

namespace semi::infra::audio_output {

struct ByteSpscRing::Impl {
    std::vector<std::byte> storage;
    std::atomic<std::uint64_t> read_index{0};
    std::atomic<std::uint64_t> write_index{0};
};

ByteSpscRing::ByteSpscRing()
    : impl_(std::make_unique<Impl>()) {}

ByteSpscRing::~ByteSpscRing() = default;

void ByteSpscRing::resize(std::size_t capacity) {
    assert(capacity > 0);
    impl_->storage.assign(capacity, std::byte{0});
    impl_->read_index.store(0, std::memory_order_relaxed);
    impl_->write_index.store(0, std::memory_order_relaxed);
}

void ByteSpscRing::clear() noexcept {
    impl_->storage.clear();
    impl_->storage.shrink_to_fit();
    impl_->read_index.store(0, std::memory_order_relaxed);
    impl_->write_index.store(0, std::memory_order_relaxed);
}

void ByteSpscRing::reset() noexcept {
    const auto write = impl_->write_index.load(std::memory_order_acquire);
    impl_->read_index.store(write, std::memory_order_release);
}

std::size_t ByteSpscRing::available() const noexcept {
    const auto write = impl_->write_index.load(std::memory_order_acquire);
    const auto read = impl_->read_index.load(std::memory_order_acquire);
    return static_cast<std::size_t>(write - read);
}

bool ByteSpscRing::try_write(const std::byte* source, std::size_t count) noexcept {
    if (count == 0) {
        return true;
    }
    const auto write = impl_->write_index.load(std::memory_order_relaxed);
    const auto read = impl_->read_index.load(std::memory_order_acquire);
    const auto used = static_cast<std::size_t>(write - read);
    if (count > impl_->storage.size() - used) {
        return false;
    }

    const auto offset = static_cast<std::size_t>(write % impl_->storage.size());
    const auto first = std::min(count, impl_->storage.size() - offset);
    std::memcpy(impl_->storage.data() + offset, source, first);
    if (count > first) {
        std::memcpy(impl_->storage.data(), source + first, count - first);
    }
    impl_->write_index.store(write + count, std::memory_order_release);
    return true;
}

std::size_t ByteSpscRing::try_read(std::byte* destination, std::size_t count) noexcept {
    if (count == 0 || impl_->storage.empty()) {
        return 0;
    }
    const auto read = impl_->read_index.load(std::memory_order_relaxed);
    const auto write = impl_->write_index.load(std::memory_order_acquire);
    const auto copied = std::min(count, static_cast<std::size_t>(write - read));
    if (copied == 0) {
        return 0;
    }

    const auto offset = static_cast<std::size_t>(read % impl_->storage.size());
    const auto first = std::min(copied, impl_->storage.size() - offset);
    std::memcpy(destination, impl_->storage.data() + offset, first);
    if (copied > first) {
        std::memcpy(destination + first, impl_->storage.data(), copied - first);
    }
    impl_->read_index.store(read + copied, std::memory_order_release);
    return copied;
}

} // namespace semi::infra::audio_output
