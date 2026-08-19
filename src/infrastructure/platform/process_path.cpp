#include "infrastructure/platform/process_path.hpp"

#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace semi::infra::platform {

std::optional<std::filesystem::path> executable_directory() noexcept {
    try {
#if defined(_WIN32)
        std::vector<wchar_t> buffer(32768);
        const DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length != 0 && length < buffer.size()) {
            return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
        }
#elif defined(__linux__)
        std::error_code ec;
        const auto path = std::filesystem::read_symlink("/proc/self/exe", ec);
        if (!ec) {
            return path.parent_path();
        }
#endif
    } catch (...) {
    }

    return std::nullopt;
}

} // namespace semi::infra::platform
