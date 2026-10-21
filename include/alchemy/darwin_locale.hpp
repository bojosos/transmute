#pragma once

#include "alchemy/process_context.hpp"

namespace alchemy {
// C/POSIX locale services and explicitly checked Darwin 32-bit wchar operations.
[[nodiscard]] std::function<bool(std::uint64_t)> install_darwin_locale(ProcessContext& context);
}
