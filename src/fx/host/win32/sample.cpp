// win32 backend of fx/host/sample.hpp.

#include "fx/host/sample.hpp"

#include <windows.h>
#include <dbghelp.h>

#pragma comment(lib, "dbghelp.lib")

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace transmute::fx::host::sample {

static_assert(kCpuContextBytes == sizeof(CONTEXT) && alignof(Registers) >= alignof(CONTEXT));

Thread open_thread(std::uint32_t id) noexcept {
    return OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id);
}

Thread open_current_thread() noexcept {
    HANDLE thread = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &thread, 0, FALSE,
                         DUPLICATE_SAME_ACCESS))
        return nullptr;
    return thread;
}

bool suspend(Thread thread) noexcept { return SuspendThread(thread) != static_cast<DWORD>(-1); }

void resume(Thread thread) noexcept { ResumeThread(thread); }

bool capture(Thread thread, Registers& registers, bool integer) noexcept {
    auto& context = *reinterpret_cast<CONTEXT*>(registers.storage);
    std::memset(&context, 0, sizeof(context));
    context.ContextFlags = integer ? CONTEXT_CONTROL | CONTEXT_INTEGER : CONTEXT_CONTROL;
    return GetThreadContext(thread, &context) != FALSE;
}

void close_thread(Thread thread) noexcept {
    if (thread) CloseHandle(thread);
}

std::size_t capture_words(Thread thread, std::uint64_t* out, std::size_t max, std::uint64_t& sp) noexcept {
    alignas(16) CONTEXT context;
    std::memset(&context, 0, sizeof(context));
    context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_FLOATING_POINT;
    if (!GetThreadContext(thread, &context)) return 0;
    sp = context.Rsp;
    const DWORD64 gprs[] = {context.Rax, context.Rcx, context.Rdx, context.Rbx, context.Rsp, context.Rbp,
                            context.Rsi, context.Rdi, context.R8,  context.R9,  context.R10, context.R11,
                            context.R12, context.R13, context.R14, context.R15, context.Rip};
    std::size_t n = 0;
    for (const DWORD64 value : gprs)
        if (n < max) out[n++] = value;
    const M128A* xmm = &context.Xmm0;
    for (unsigned i = 0; i < 16; ++i) {
        if (n < max) out[n++] = xmm[i].Low;
        if (n < max) out[n++] = static_cast<std::uint64_t>(xmm[i].High);
    }
    return n;
}

bool scan_stack(std::uint64_t sp, void (*word)(void* user, std::uint64_t value), void* user) noexcept {
    sp &= ~std::uint64_t{7};
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<const void*>(sp), &info, sizeof(info)) != sizeof(info) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        return false;
    const auto* p = reinterpret_cast<const volatile std::uint64_t*>(sp);
    const auto* end = reinterpret_cast<const volatile std::uint64_t*>(reinterpret_cast<std::uint64_t>(info.BaseAddress) +
                                                                      info.RegionSize);
    __try {
        for (; p < end; ++p) word(user, *p);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

std::size_t copy_stack(std::uint64_t sp, std::uint8_t* out, std::size_t size) noexcept {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<const void*>(sp), &info, sizeof(info)) != sizeof(info) || info.State != MEM_COMMIT)
        return 0;
    const auto end = reinterpret_cast<std::uint64_t>(info.BaseAddress) + info.RegionSize;
    const auto bytes = static_cast<std::size_t>(std::min<std::uint64_t>(size, end - sp));
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(sp), bytes);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return bytes;
}

namespace {

unsigned unwind_guarded(CONTEXT& context, std::uint64_t real, std::uint64_t lo, std::size_t size,
                        std::uint64_t* frames, unsigned max, bool (*stop)(std::uint64_t)) noexcept {
    const std::uint64_t delta = lo - real;   // modular
    const auto relocate = [&](DWORD64& reg) {
        if (reg >= real && reg - real < size) reg += delta;
    };
    const auto in_copy = [&](std::uint64_t at, std::size_t bytes) { return at >= lo && at + bytes <= lo + size; };
    unsigned n = 0;
    __try {
        while (n < max && context.Rip != 0 && !(stop && stop(context.Rip))) {
            frames[n++] = context.Rip;
            relocate(context.Rbp);
            DWORD64 image = 0;
            PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(context.Rip, &image, nullptr);
            if (!fe) {
                if (!in_copy(context.Rsp, 8)) break;
                context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
                context.Rsp += 8;
                continue;
            }
            void* handler_data = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, context.Rip, fe, &context, &handler_data, &establisher, nullptr);
            if (!in_copy(context.Rsp, 0)) break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

} // namespace

unsigned unwind_copy(const Registers& registers, const std::uint8_t* copy, std::size_t size, std::uint64_t* frames,
                     unsigned max, bool (*stop)(std::uint64_t pc)) noexcept {
    CONTEXT context = *reinterpret_cast<const CONTEXT*>(registers.storage);
    const std::uint64_t real = context.Rsp;
    const auto lo = reinterpret_cast<std::uint64_t>(copy);
    context.Rsp += lo - real;
    return unwind_guarded(context, real, lo, size, frames, max, stop);
}

bool load_symbols() noexcept {
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    return SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
}

std::string symbol(std::uint64_t pc) {
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256]{};
    auto* info = reinterpret_cast<SYMBOL_INFO*>(buffer);
    info->SizeOfStruct = sizeof(SYMBOL_INFO);
    info->MaxNameLen = 255;
    DWORD64 displacement = 0;
    if (!SymFromAddr(GetCurrentProcess(), pc, &displacement, info)) return {};
    return info->Name;
}

bool source_line(std::uint64_t pc, std::string& file, unsigned& line) {
    IMAGEHLP_LINE64 info{};
    info.SizeOfStruct = sizeof(info);
    DWORD displacement = 0;
    if (!SymGetLineFromAddr64(GetCurrentProcess(), pc, &displacement, &info)) return false;
    const char* name = std::strrchr(info.FileName, '\\');
    file = name ? name + 1 : info.FileName;
    line = info.LineNumber;
    return true;
}

std::string describe(std::uint64_t pc, bool symbols) {
    HMODULE module = nullptr;
    char name[MAX_PATH] = "?";
    std::uint64_t base = 0;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(pc), &module)) {
        GetModuleFileNameA(module, name, MAX_PATH);
        base = reinterpret_cast<std::uint64_t>(module);
    }
    const char* file = std::strrchr(name, '\\');
    std::string out = file ? file + 1 : name;
    const std::string sym = symbols ? symbol(pc) : std::string();
    if (!sym.empty()) {
        out += "!";
        out += sym;
    } else if (base) {
        char text[32];
        std::snprintf(text, sizeof(text), "+0x%llx", static_cast<unsigned long long>(pc - base));
        out += text;
    }
    return out;
}

bool wait_module(std::uint64_t& begin, std::uint64_t& end) noexcept {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(ntdll);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const std::uint8_t*>(ntdll) + dos->e_lfanew);
    begin = reinterpret_cast<std::uint64_t>(ntdll);
    end = begin + nt->OptionalHeader.SizeOfImage;
    return true;
}

Timer create_timer() noexcept {
    return CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
}

void wait(Timer timer, std::uint32_t microseconds) noexcept {
    if (!timer) {
        Sleep(1);
        return;
    }
    LARGE_INTEGER due;
    due.QuadPart = -static_cast<LONGLONG>(microseconds) * 10;
    SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
    WaitForSingleObject(timer, INFINITE);
}

} // namespace transmute::fx::host::sample
