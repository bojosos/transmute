#pragma once

#include "alchemy/process.hpp"

#include <iosfwd>

namespace alchemy {

// Static inspection only: no guest initializers or application code execute.
[[nodiscard]] int inspect_program(const std::filesystem::path& path,
    const RunOptions& options, bool recursive, bool scan_bundle, bool json,
    std::ostream& output);

} // namespace alchemy
