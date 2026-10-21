#pragma once

// Host area "env": the process environment (docs/FX_HOST_SEAMS.md, "env").

#include "fx/host/host.hpp"

#include <cstddef>

namespace transmute::fx::host {

// GetEnvironmentVariableA semantics: copies `name`'s value with its
// terminating zero into `value` and returns its length; if it does not fit,
// returns the size needed (including the zero); 0 if `name` is not set.
// `value` may be nullptr with `size` 0 (a presence check).
std::size_t get_env(const char* name, char* value, std::size_t size) noexcept;
// The same for UTF-16 names and values (the cache's paths); `size` counts
// wchar_t.
std::size_t get_env(const wchar_t* name, wchar_t* value, std::size_t size) noexcept;

} // namespace transmute::fx::host
