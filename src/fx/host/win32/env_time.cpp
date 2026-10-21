// win32 backend of fx/host/env.hpp and the out-of-line part of fx/host/time.hpp.

#include "fx/host/env.hpp"
#include "fx/host/time.hpp"

#include <windows.h>

namespace transmute::fx::host {

std::size_t get_env(const char* name, char* value, std::size_t size) noexcept {
    return GetEnvironmentVariableA(name, value, static_cast<DWORD>(size));
}

std::size_t get_env(const wchar_t* name, wchar_t* value, std::size_t size) noexcept {
    return GetEnvironmentVariableW(name, value, static_cast<DWORD>(size));
}

namespace {
std::uint64_t value(const FILETIME& t) { return (std::uint64_t{t.dwHighDateTime} << 32) | t.dwLowDateTime; }
} // namespace

std::uint64_t wall_time() noexcept {
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    return value(now);
}

bool process_times(ProcessTimes& times) noexcept {
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return false;
    times.created = value(created);
    times.kernel = value(kernel);
    times.user = value(user);
    return true;
}

void sleep_ms(unsigned milliseconds) noexcept { Sleep(milliseconds); }

} // namespace transmute::fx::host
