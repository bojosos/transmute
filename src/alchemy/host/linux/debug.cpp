// Linux host backend, area "debug".
//
// on_fatal_fault's handler reports only the faults nobody else takes, in
// either installation order. Installed after the fx engine's fault handler
// (fx/host/linux/fault.cpp), it first passes each fault to that handler,
// which resumes a guest fault, jumps out of a guarded read, or (not its
// fault) restores the default action; only then is the fault reported.
// Installed before, it is the handler the fx engine's passes faults on to.
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

constexpr int kSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE};
struct sigaction g_previous[sizeof(kSignals) / sizeof(kSignals[0])];

std::size_t slot_of(int signal) noexcept {
    for (std::size_t i = 0; i < sizeof(kSignals) / sizeof(kSignals[0]); ++i)
        if (kSignals[i] == signal) return i;
    return 0;
}

void fatal_handler(int signal, siginfo_t* info, void* raw);

struct sigaction installed(int signal) noexcept {
    struct sigaction now {};
    sigaction(signal, nullptr, &now);
    return now;
}

void fatal_handler(int signal, siginfo_t* info, void* raw) {
    // While ours is the installed handler, the one installed before it (the
    // fx engine's) gets the fault first. It returns having resumed it, or
    // having restored the default action because the fault is not its own;
    // a guarded read never returns. When another handler was installed over
    // ours, that one called us as its own previous handler: report.
    const auto current = installed(signal);
    if ((current.sa_flags & SA_SIGINFO) && current.sa_sigaction == &fatal_handler) {
        const auto& previous = g_previous[slot_of(signal)];
        bool called = false;
        if ((previous.sa_flags & SA_SIGINFO) && previous.sa_sigaction) {
            previous.sa_sigaction(signal, info, raw);
            called = true;
        } else if (!(previous.sa_flags & SA_SIGINFO) && previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN) {
            previous.sa_handler(signal);
            called = true;
        }
        if (called) {
            const auto after = installed(signal);
            const bool gave_up = !(after.sa_flags & SA_SIGINFO) && after.sa_handler == SIG_DFL;
            if (!gave_up) return;
        }
    }
    // Reported once: the default action ends the process (with a core dump)
    // when the faulting instruction runs again.
    struct sigaction fallback {};
    fallback.sa_handler = SIG_DFL;
    sigemptyset(&fallback.sa_mask);
    sigaction(signal, &fallback, nullptr);
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
}

} // namespace

void on_fatal_fault(void (*report)(const Fault& fault)) {
    g_report = report;
    struct sigaction action {};
    action.sa_sigaction = &fatal_handler;
    // SA_NODEFER: a guarded read that a previous handler ends with siglongjmp
    // (fx's guards, which keep the signal mask) leaves the signal unblocked.
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&action.sa_mask);
    for (std::size_t i = 0; i < sizeof(kSignals) / sizeof(kSignals[0]); ++i) {
        struct sigaction old {};
        if (sigaction(kSignals[i], &action, &old) != 0) continue;
        if (!((old.sa_flags & SA_SIGINFO) && old.sa_sigaction == &fatal_handler)) g_previous[i] = old;
    }
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
