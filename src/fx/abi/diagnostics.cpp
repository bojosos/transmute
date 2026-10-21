// Import and exit tracing (owner: abi).
//
//   TRANSMUTE_FX_TRACE_IMPORTS=1      log every call through a host thunk or override
//   TRANSMUTE_FX_TRACE_IMPORTS=first  log each import on its first call only
//   TRANSMUTE_FX_TRACE_EXIT=1         log process-exit paths with their guest caller
//
// The import trace costs nothing when off: the mode is read once, and thunks
// bound while it is off contain no tracing code.

#include "fx/abi/abi_internal.hpp"
#include "transmute/fx/process.hpp"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace transmute::fx::abi {
namespace {

std::string environment(const char* name) {
    char value[64] = {};
    const DWORD n = GetEnvironmentVariableA(name, value, sizeof value);
    if (n == 0 || n >= sizeof value) return {};
    return value;
}

void write_stderr(const char* text, std::size_t size) {
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    DWORD written = 0;
    if (err && err != INVALID_HANDLE_VALUE) WriteFile(err, text, static_cast<DWORD>(size), &written, nullptr);
}

std::string narrow(const std::wstring& text) {
    std::string out;
    for (const wchar_t ch : text) out.push_back(ch < 128 ? static_cast<char>(ch) : '?');
    return out;
}

} // namespace

TraceMode import_trace_mode() noexcept {
    static const TraceMode mode = [] {
        const auto value = environment("TRANSMUTE_FX_TRACE_IMPORTS");
        if (value.empty() || value == "0") return TraceMode::off;
        if (value == "first") return TraceMode::first;
        return TraceMode::all;
    }();
    return mode;
}

bool exit_trace_enabled() noexcept {
    static const bool enabled = [] {
        const auto value = environment("TRANSMUTE_FX_TRACE_EXIT");
        return !value.empty() && value != "0";
    }();
    return enabled;
}

std::string describe_guest_address(std::uint64_t address) {
    char buffer[64];
    if (const auto* module = find_guest_module(address)) {
        std::snprintf(buffer, sizeof buffer, "+0x%llx", static_cast<unsigned long long>(address - module->base));
        return narrow(module->name) + buffer;
    }
    HMODULE host = nullptr;
    wchar_t path[MAX_PATH] = {};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &host) &&
        GetModuleFileNameW(host, path, MAX_PATH) != 0) {
        std::wstring name(path);
        const auto slash = name.find_last_of(L"\\/");
        if (slash != std::wstring::npos) name = name.substr(slash + 1);
        std::snprintf(buffer, sizeof buffer, "+0x%llx",
                      static_cast<unsigned long long>(address - reinterpret_cast<std::uint64_t>(host)));
        return "host:" + narrow(name) + buffer;
    }
    std::snprintf(buffer, sizeof buffer, "0x%llx", static_cast<unsigned long long>(address));
    return buffer;
}

void trace_import_call(TraceInfo* info, std::uint64_t caller) noexcept {
    const DWORD saved_error = GetLastError();   // the traced API's error semantics must not change
    if (import_trace_mode() == TraceMode::first) {
        if (info->seen) { SetLastError(saved_error); return; }
        info->seen = true;   // benign race: at worst a second line
    }
    try {
        std::string line = "fx call ";
        line += info->dll;
        line += '!';
        line += info->name;
        line += " from ";
        line += describe_guest_address(caller);
        line += " [tid ";
        line += std::to_string(GetCurrentThreadId());
        line += "]\n";
        write_stderr(line.data(), line.size());
    } catch (...) {
    }
    SetLastError(saved_error);
}

void trace_callback(std::uint64_t guest_function, const char* sig, bool had_context) noexcept {
    const DWORD saved_error = GetLastError();
    try {
        std::string line = "fx callback ";
        line += describe_guest_address(guest_function);
        line += ' ';
        line += sig;
        line += had_context ? " (nested)" : " (adopted thread)";
        line += " [tid ";
        line += std::to_string(GetCurrentThreadId());
        line += "]\n";
        write_stderr(line.data(), line.size());
    } catch (...) {
    }
    SetLastError(saved_error);
}

void trace_exit(const char* api, std::uint32_t code, std::uint64_t caller) noexcept {
    if (!exit_trace_enabled()) return;
    const DWORD saved_error = GetLastError();
    try {
        char head[128];
        std::snprintf(head, sizeof head, "fx exit: %s code=%u (0x%x) from ", api, code, code);
        std::string line = head;
        line += describe_guest_address(caller);
        line += " [tid ";
        line += std::to_string(GetCurrentThreadId());
        line += "]\n";
        write_stderr(line.data(), line.size());
    } catch (...) {
    }
    SetLastError(saved_error);
}

} // namespace transmute::fx::abi
