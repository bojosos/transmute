#pragma once

#include "transmute/a64.hpp"

namespace transmute {

// Only APIs with scalar inputs/results and no guest-memory or callback effects
// are eligible. The loader binds the returned native wrapper once per import.
[[nodiscard]] NativeImportCall resolve_direct_win32_import(const Import& imported) noexcept;

} // namespace transmute
