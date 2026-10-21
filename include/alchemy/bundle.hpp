#pragma once

#include <filesystem>
#include <vector>

namespace alchemy {

[[nodiscard]] std::filesystem::path discover_program(const std::filesystem::path& path);
[[nodiscard]] std::vector<std::filesystem::path> bundle_macho_files(const std::filesystem::path& path);

} // namespace alchemy
