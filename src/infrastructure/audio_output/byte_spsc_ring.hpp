#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace semi::infra::audio_output {

// Fixed-capacity byte ring for exactly one producer and one consumer. resize(),
// clear(), and reset() require the consumer to be stopped.
class ByteSpscRing final {
public:
    ByteSpscRing();
    ~ByteSpscRing();

    ByteSpscRing(const ByteSpscRing&) = delete;
    ByteSpscRing& operator=(const ByteSpscRing&) = delete;
    ByteSpscRing(ByteSpscRing&&) = delete;
    ByteSpscRing& operator=(ByteSpscRing&&) = delete;

    void resize(std::size_t capacity);
    void clear() noexcept;
    void reset() noexcept;

    [[nodiscard]] std::size_t available() const noexcept;
    [[nodiscard]] bool try_write(const std::byte* source, std::size_t count) noexcept;
    [[nodiscard]] std::size_t try_read(std::byte* destination, std::size_t count) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace semi::infra::audio_output
