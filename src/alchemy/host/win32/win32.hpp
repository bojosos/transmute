#pragma once

// Private helpers of the Win32 host backend. Only src/alchemy/host/win32/
// includes this header (and through it, Windows headers).

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <WinSock2.h>
#include <Windows.h>

#include "alchemy/host/host.hpp"

#include <string>
#include <string_view>

namespace alchemy::host::win32 {

[[nodiscard]] inline HANDLE native(Handle handle) noexcept { return reinterpret_cast<HANDLE>(handle.value); }
// A Handle for `value`; INVALID_HANDLE_VALUE and NULL are both "none".
[[nodiscard]] inline Handle wrap(HANDLE value) noexcept {
    return value == INVALID_HANDLE_VALUE ? Handle{} : Handle{reinterpret_cast<std::uintptr_t>(value)};
}

// Win32 / WinSock error codes as host errors.
[[nodiscard]] Error error(DWORD code) noexcept;
[[nodiscard]] inline Error last_error() noexcept { return error(GetLastError()); }
[[nodiscard]] inline Error last_socket_error() noexcept { return error(static_cast<DWORD>(WSAGetLastError())); }

// A FILETIME duration (100 ns units) in nanoseconds.
[[nodiscard]] inline std::uint64_t nanoseconds(const FILETIME& time) noexcept {
    return ((static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime) * 100;
}

// A session-local object name ("Local\<name>"); empty stays empty (a private object).
[[nodiscard]] std::wstring local_name(std::string_view name);

// UTF-8 <-> UTF-16 (invalid sequences become U+FFFD).
[[nodiscard]] std::wstring widen(std::string_view text);
[[nodiscard]] std::string narrow(std::wstring_view text);

} // namespace alchemy::host::win32
