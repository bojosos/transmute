#pragma once

#include "alchemy/process_context.hpp"

#include <string_view>

namespace alchemy {

// Independently authored LP64 Accelerate compatibility for bounded numeric work.
void install_darwin_numeric(ProcessContext& context);

// Empty means implemented or outside this provider. Doctor uses a nonempty
// requirement to distinguish a diagnostic binding from supported execution.
[[nodiscard]] std::string_view deferred_numeric_api(std::string_view symbol);

} // namespace alchemy
