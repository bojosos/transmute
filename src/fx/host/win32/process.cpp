// win32 backend of fx/host/process.hpp.

#include "fx/host/process.hpp"

#include <windows.h>

#include <iterator>

namespace transmute::fx::host {

std::uint32_t process_id() noexcept { return GetCurrentProcessId(); }

double process_age_ms() noexcept {
    FILETIME created, exited, kernel, user, now;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    GetSystemTimePreciseAsFileTime(&now);
    const auto value = [](const FILETIME& t) { return (unsigned long long)t.dwHighDateTime << 32 | t.dwLowDateTime; };
    return static_cast<double>(value(now) - value(created)) / 1e4;
}

std::wstring executable_path() {
    wchar_t self[MAX_PATH * 2];
    const DWORD length = GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
    return length > 0 && length < std::size(self) ? std::wstring(self, length) : std::wstring();
}

bool process_qos_explicit() noexcept {
    PROCESS_POWER_THROTTLING_STATE current{};
    current.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    return GetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &current, sizeof(current)) &&
           (current.ControlMask & PROCESS_POWER_THROTTLING_EXECUTION_SPEED);
}

void set_process_qos(bool eco) noexcept {
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    state.StateMask = eco ? PROCESS_POWER_THROTTLING_EXECUTION_SPEED : 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state));
}

void set_process_idle_priority() noexcept { SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS); }

bool process_shutting_down() noexcept {
    using ShutdownFn = BOOLEAN(NTAPI*)();
    static const auto fn = reinterpret_cast<ShutdownFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlDllShutdownInProgress"));
    return fn && fn() != 0;
}

void terminate_process(std::uint32_t exit_code) noexcept { TerminateProcess(GetCurrentProcess(), exit_code); }

namespace {

struct ExitWatch {
    HANDLE process;
    void (*on_exit)(void*);
    void* context;
};

void CALLBACK helper_exited(void* parameter, BOOLEAN) {
    auto* watch = static_cast<ExitWatch*>(parameter);
    CloseHandle(watch->process);
    watch->on_exit(watch->context);
    delete watch;
}

} // namespace

bool spawn_background_process(const std::wstring& program, const std::wstring& command_line,
                              void (*on_exit)(void* context), void* context) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // No console, no inherited handles, idle priority; out of the caller's job if it allows that.
    const DWORD base_flags = DETACHED_PROCESS | IDLE_PRIORITY_CLASS | CREATE_UNICODE_ENVIRONMENT;
    std::wstring line = command_line;
    BOOL ok = CreateProcessW(program.c_str(), line.data(), nullptr, nullptr, FALSE, base_flags | CREATE_BREAKAWAY_FROM_JOB,
                             nullptr, nullptr, &si, &pi);
    if (!ok) {
        line = command_line;
        ok = CreateProcessW(program.c_str(), line.data(), nullptr, nullptr, FALSE, base_flags, nullptr, nullptr, &si, &pi);
    }
    if (!ok) return false;
    CloseHandle(pi.hThread);
    auto* watch = new ExitWatch{pi.hProcess, on_exit, context};
    HANDLE wait = nullptr;
    if (!RegisterWaitForSingleObject(&wait, pi.hProcess, &helper_exited, watch, INFINITE,
                                     WT_EXECUTEONLYONCE | WT_EXECUTELONGFUNCTION)) {
        CloseHandle(pi.hProcess);
        delete watch;
    }
    // `wait` is never unregistered: one small registration per helper.
    return true;
}

} // namespace transmute::fx::host
