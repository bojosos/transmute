#pragma once

#include "alchemy/process_context.hpp"

namespace alchemy {
// Additional independently authored libc and Darwin helpers used by guest runtimes.
// Installed callbacks own their state through ProcessContext's callback lifetime.
void install_darwin_support(ProcessContext& context);
} // namespace alchemy
