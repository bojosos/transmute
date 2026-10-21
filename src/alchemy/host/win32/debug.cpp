#include "alchemy/host/debug.hpp"

#include "alchemy/host/win32/win32.hpp"

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
