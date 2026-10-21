#pragma once

// Host area "text": the host's wide strings. fx keeps module names and paths
// as std::wstring (UTF-16 on win32, where they come from); these convert
// them for messages and from UTF-8 paths (docs/FX_HOST_SEAMS.md, "text").

#include "fx/host/host.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace transmute::fx::host {

// Converts the zero-terminated `text` to UTF-8 in `out` (`size` bytes,
// zero-terminated when it fits); returns the bytes written including the
// zero, or 0 if `out` is too small or `text` cannot be converted.
std::size_t to_utf8(const wchar_t* text, char* out, std::size_t size) noexcept;
// The same into a string (empty if `text` cannot be converted).
std::string to_utf8(std::wstring_view text);
// UTF-8 to the host's wide string (empty if `text` is not valid UTF-8 the
// host can convert).
std::wstring from_utf8(std::string_view text);

} // namespace transmute::fx::host
