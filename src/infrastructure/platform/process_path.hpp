#pragma once

#include <filesystem>
#include <optional>

namespace semi::infra::platform {

std::optional<std::filesystem::path> executable_directory() noexcept;

} // namespace semi::infra::platform
