#include "alchemy/host/debug.hpp"

#include "alchemy/host/win32/win32.hpp"

#include <TlHelp32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>

namespace alchemy::host::debug {
namespace {

std::atomic<void (*)(const Fault&)> g_report{};

LONG WINAPI unhandled(EXCEPTION_POINTERS* pointers) {
    const auto report = g_report.load();
    if (!report) return EXCEPTION_CONTINUE_SEARCH;
    const auto* record = pointers->ExceptionRecord;
    Fault fault;
    fault.code = record->ExceptionCode;
    fault.address = reinterpret_cast<std::uintptr_t>(record->ExceptionAddress);
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        fault.access = record->ExceptionInformation[0] == 1 ? Access::write : record->ExceptionInformation[0] == 8 ? Access::execute : Access::read;
        fault.data_address = static_cast<std::uintptr_t>(record->ExceptionInformation[1]);
    }
    // Host stack: x64 unwind from the faulting context.
    static std::array<std::uintptr_t, 40> frames;
    std::size_t count = 0;
    CONTEXT context = *pointers->ContextRecord;
    while (count < frames.size() && context.Rip) {
        frames[count++] = static_cast<std::uintptr_t>(context.Rip);
        DWORD64 image{};
        auto* function = RtlLookupFunctionEntry(context.Rip, &image, nullptr);
        if (!function) {
            // Leaf function: the return address is at [rsp].
            context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
            context.Rsp += 8;
        } else {
            void* handler_data{};
            DWORD64 frame{};
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, context.Rip, function, &context, &handler_data, &frame, nullptr);
        }
    }
    fault.stack = std::span<const std::uintptr_t>(frames.data(), count);
    report(fault);
    return EXCEPTION_CONTINUE_SEARCH;
}

// x64 unwind of a suspended thread, within its stack's committed region
// [low, high). Only the innermost frame may be a leaf without unwind data (a
// system call stub); anywhere else that is engine-generated code, where the
// walk ends. No allocation and no loader calls: the thread may hold the locks.
std::size_t unwind_suspended(CONTEXT& context, std::uintptr_t* frames, std::size_t capacity, DWORD64 low, DWORD64 high) {
    std::size_t count = 0;
    __try {
        while (count < capacity && context.Rip && context.Rsp >= low && context.Rsp < high) {
            frames[count++] = static_cast<std::uintptr_t>(context.Rip);
            DWORD64 image{};
            auto* function = RtlLookupFunctionEntry(context.Rip, &image, nullptr);
            if (!function) {
                if (count > 1) break;
                context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
                context.Rsp += 8;
            } else {
                void* handler_data{};
                DWORD64 frame{};
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, context.Rip, function, &context, &handler_data, &frame, nullptr);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return count;
}

} // namespace

void on_fatal_fault(void (*report)(const Fault& fault)) {
    g_report.store(report);
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(unhandled);
}

std::size_t capture_stack(std::span<std::uintptr_t> frames, unsigned skip) noexcept {
    std::array<void*, 62> raw{};
    const auto wanted = static_cast<DWORD>(std::min<std::size_t>(frames.size(), raw.size()));
    // +1: this function's own frame.
    const auto count = RtlCaptureStackBackTrace(skip + 1, wanted, raw.data(), nullptr);
    for (USHORT index = 0; index < count; ++index) frames[index] = reinterpret_cast<std::uintptr_t>(raw[index]);
    return count;
}

std::vector<ThreadStack> other_thread_stacks(std::size_t depth) {
    std::vector<ThreadStack> stacks;
    const auto self = GetCurrentThreadId(), process = GetCurrentProcessId();
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return stacks;
    std::vector<DWORD> threads;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry))
        if (entry.th32OwnerProcessID == process && entry.th32ThreadID != self) threads.push_back(entry.th32ThreadID);
    CloseHandle(snapshot);
    std::vector<std::uintptr_t> buffer(depth);
    for (const auto id : threads) {
        const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id);
        if (!thread) continue;
        std::size_t count = 0;
        if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
            alignas(16) CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            MEMORY_BASIC_INFORMATION stack{};
            if (GetThreadContext(thread, &context) &&
                VirtualQuery(reinterpret_cast<void*>(context.Rsp), &stack, sizeof(stack)) == sizeof(stack)) {
                const auto low = reinterpret_cast<DWORD64>(stack.BaseAddress);
                count = unwind_suspended(context, buffer.data(), buffer.size(), low, low + stack.RegionSize);
            }
            ResumeThread(thread);
        }
        FILETIME created{}, exited{}, kernel{}, user{};
        std::uint64_t cpu_ms = 0;
        if (GetThreadTimes(thread, &created, &exited, &kernel, &user)) {
            const auto ticks = [](const FILETIME& t) { return (std::uint64_t{t.dwHighDateTime} << 32) | t.dwLowDateTime; };
            cpu_ms = (ticks(kernel) + ticks(user)) / 10'000;   // 100 ns units
        }
        CloseHandle(thread);
        stacks.push_back({id, cpu_ms, std::vector<std::uintptr_t>(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(count))});
    }
    return stacks;
}

std::string describe_address(std::uintptr_t address) {
    char out[MAX_PATH + 64];
    HMODULE module{};
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(address), &module)) {
        std::snprintf(out, sizeof(out), "0x%llx (no module)", static_cast<unsigned long long>(address));
        return out;
    }
    char path[MAX_PATH]{};
    GetModuleFileNameA(module, path, MAX_PATH);
    const char* name = path;
    for (const char* p = path; *p; ++p) if (*p == '\\' || *p == '/') name = p + 1;
    std::snprintf(out, sizeof(out), "0x%llx %s+0x%llx", static_cast<unsigned long long>(address), name,
                  static_cast<unsigned long long>(address - reinterpret_cast<std::uintptr_t>(module)));
    return out;
}

} // namespace alchemy::host::debug
