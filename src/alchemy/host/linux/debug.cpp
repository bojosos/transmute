// Linux host backend, area "debug".
//
// on_fatal_fault installs the handler that ends up last in the chain: the fx
// engine installs its fault handler later and passes on what it does not
// handle to the one installed before it (fx/host/linux/fault.cpp).
#include "alchemy/host/debug.hpp"
#include "alchemy/host/linux/linux.hpp"

#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>

namespace alchemy::host::debug {

namespace {

void (*g_report)(const Fault&) = nullptr;

void fatal_handler(int signal, siginfo_t* info, void* raw) {
    const auto* context = static_cast<const ucontext_t*>(raw);
    Fault fault;
    fault.code = static_cast<std::uint32_t>(signal);
    fault.address = static_cast<std::uintptr_t>(context->uc_mcontext.gregs[REG_RIP]);
    if (signal == SIGSEGV || signal == SIGBUS) {
        const auto error = context->uc_mcontext.gregs[REG_ERR];
        fault.access = (error & 0x10) ? Access::execute : (error & 0x2) ? Access::write : Access::read;
        fault.data_address = reinterpret_cast<std::uintptr_t>(info->si_addr);
    }
    void* frames[64];
    const int count = backtrace(frames, 64);
    fault.stack = std::span<const std::uintptr_t>(reinterpret_cast<const std::uintptr_t*>(frames), static_cast<std::size_t>(count));
    if (g_report) g_report(fault);
    // The report returned: SA_RESETHAND restored the default action, which
    // ends the process (with a core dump) when the fault repeats.
}

} // namespace

void on_fatal_fault(void (*report)(const Fault& fault)) {
    g_report = report;
    struct sigaction action {};
    action.sa_sigaction = &fatal_handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&action.sa_mask);
    for (const int signal : {SIGSEGV, SIGBUS, SIGILL, SIGFPE}) sigaction(signal, &action, nullptr);
}

std::size_t capture_stack(std::span<std::uintptr_t> frames, unsigned skip) noexcept {
    void* raw[128];
    const int count = backtrace(raw, static_cast<int>(std::min<std::size_t>(128, frames.size() + skip + 1)));
    std::size_t written = 0;
    for (int i = static_cast<int>(skip) + 1; i < count && written < frames.size(); ++i)
        frames[written++] = reinterpret_cast<std::uintptr_t>(raw[i]);
    return written;
}

// Walking other threads needs them stopped in a signal handler; not yet.
std::vector<ThreadStack> other_thread_stacks(std::size_t) { return {}; }

std::string describe_address(std::uintptr_t address) {
    char text[512];
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(address), &info) && info.dli_fname) {
        const char* name = info.dli_fname;
        for (const char* p = info.dli_fname; *p; ++p) if (*p == '/') name = p + 1;
        std::snprintf(text, sizeof(text), "0x%llx %s+0x%llx", static_cast<unsigned long long>(address), name,
                      static_cast<unsigned long long>(address - reinterpret_cast<std::uintptr_t>(info.dli_fbase)));
    } else {
        std::snprintf(text, sizeof(text), "0x%llx (no module)", static_cast<unsigned long long>(address));
    }
    return text;
}

} // namespace alchemy::host::debug
