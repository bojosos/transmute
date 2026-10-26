#pragma once

#include "alchemy/process_context.hpp"

namespace alchemy {
// Additional independently authored libc and Darwin helpers used by guest runtimes.
// Installed callbacks own their state through ProcessContext's callback lifetime.
void install_darwin_support(ProcessContext& context);
// String, wide-string, search.h, stdlib, time, ctype, signal and path helpers
// (misc/libc_extra.cpp); install_darwin_support installs them last.
void install_darwin_libc_extra(ProcessContext& context);
} // namespace alchemy
