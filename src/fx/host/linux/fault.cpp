// linux backend of fx/host/fault.hpp: one SA_SIGINFO handler for the
// synchronous fault signals, on an alternate signal stack.
//
// Order, as with a first vectored handler and __try on Windows: fx's handler
// sees every fault first; a fault it passes on goes to the innermost guarded
// call that accepts it (guard.hpp), then to the handler that was installed
// before ours, then to the default action (the fault repeats with SIG_DFL and
// ends the process with its signal and a core dump).
#include "fx/host/fault.hpp"
#include "fx/host/guard.hpp"
#include "fx/host/linux/internal.hpp"

#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace transmute::fx::host {

static_assert(kCpuContextBytes == sizeof(ucontext_t));
static_assert(linux_context::kGregs == offsetof(ucontext_t, uc_mcontext.gregs));
static_assert(linux_context::kFpregs == offsetof(ucontext_t, uc_mcontext.fpregs));
static_assert(linux_context::kXmm == offsetof(struct _libc_fpstate, _xmm));
static_assert(linux_context::kRip == REG_RIP && linux_context::kEfl == REG_EFL && linux_context::kErr == REG_ERR &&
              linux_context::kTrapno == REG_TRAPNO);
static_assert(REG_RAX == 13 && REG_RCX == 14 && REG_RDX == 12 && REG_RBX == 11 && REG_RSP == 15 && REG_RBP == 10 &&
              REG_RSI == 9 && REG_RDI == 8 && REG_R8 == 0 && REG_R15 == 7);

namespace {

constexpr int kSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE};
constexpr std::size_t kSignalStackBytes = 64 * 1024;

FaultHandler g_handler = nullptr;
struct sigaction g_previous[sizeof(kSignals) / sizeof(kSignals[0])];

thread_local void* t_signal_stack = nullptr;

std::uint32_t fault_code(int signal, const siginfo_t& info) noexcept {
    switch (signal) {
    case SIGSEGV: return kFaultAccessViolation;
    case SIGBUS: return info.si_code == BUS_ADRALN ? kFaultDatatypeMisalignment : kFaultInPageError;
    case SIGILL: return info.si_code == ILL_PRVOPC ? kFaultPrivilegedInstruction : kFaultIllegalInstruction;
    case SIGTRAP: return kFaultBreakpoint;
    case SIGFPE: return info.si_code == FPE_INTOVF ? kFaultIntegerOverflow : kFaultIntegerDivideByZero;
    default: return kFaultIllegalInstruction;
    }
}

bool accepts(linux_guard::Filter filter, std::uint32_t code) noexcept {
    switch (filter) {
    case linux_guard::Filter::any: return true;
    case linux_guard::Filter::memory:
        return code == kFaultAccessViolation || code == kFaultInPageError || code == kFaultGuardPage;
    case linux_guard::Filter::access: return code == kFaultAccessViolation || code == kFaultInPageError;
    }
    return false;
}

// Jumps to the innermost guarded call that takes the fault; returns if none.
void deliver_to_guard(const FaultRecord& record) noexcept {
    for (auto* frame = linux_guard::t_top; frame; frame = frame->previous) {
        if (!accepts(frame->filter, record.code)) continue;
        linux_guard::t_top = frame->previous;
        if (frame->record) *frame->record = record;
        siglongjmp(frame->env, 1);
    }
}

std::size_t slot_of(int signal) noexcept {
    for (std::size_t i = 0; i < sizeof(kSignals) / sizeof(kSignals[0]); ++i)
        if (kSignals[i] == signal) return i;
    return 0;
}

void signal_handler(int signal, siginfo_t* info, void* raw_context) {
    const int saved_errno = errno;
    auto& context = *static_cast<CpuContext*>(raw_context);
    const auto* uc = static_cast<const ucontext_t*>(raw_context);
    FaultRecord record{};
    record.code = fault_code(signal, *info);
    record.address = reinterpret_cast<void*>(rip(context));
    if (signal == SIGSEGV || signal == SIGBUS) {
        const auto address = reinterpret_cast<std::uint64_t>(info->si_addr);
        const auto error = static_cast<std::uint64_t>(uc->uc_mcontext.gregs[REG_ERR]);
        // The page-fault error code: bit 1 a write, bit 4 an instruction fetch.
        std::uint64_t access = kFaultRead;
        if (error & 0x10) access = kFaultExecute;
        else if (error & 0x2) access = kFaultWrite;
        if (signal == SIGSEGV && address == rip(context)) access = kFaultExecute;
        if (signal == SIGSEGV && linux_internal::take_guard_page(address)) record.code = kFaultGuardPage;
        record.parameter_count = 2;
        record.parameters[0] = access;
        record.parameters[1] = address;
    }
    if (g_handler && g_handler(record, context) == FaultAction::resume) {
        errno = saved_errno;
        return;
    }
    deliver_to_guard(record);
    // Nobody here wants it: the handler installed before ours, else the
    // default action when the faulting instruction runs again.
    const auto& previous = g_previous[slot_of(signal)];
    if (previous.sa_flags & SA_SIGINFO) {
        if (previous.sa_sigaction) {
            previous.sa_sigaction(signal, info, raw_context);
            errno = saved_errno;
            return;
        }
    } else if (previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN) {
        previous.sa_handler(signal);
        errno = saved_errno;
        return;
    }
    struct sigaction fallback {};
    fallback.sa_handler = SIG_DFL;
    sigemptyset(&fallback.sa_mask);
    sigaction(signal, &fallback, nullptr);
    if (signal == SIGTRAP) raise(signal);   // int3 does not repeat on return
    errno = saved_errno;
}

void write_text(const char* text, int length) noexcept {
    while (length > 0) {
        const auto n = ::write(STDERR_FILENO, text, static_cast<std::size_t>(length));
        if (n <= 0) return;
        text += n;
        length -= static_cast<int>(n);
    }
}

} // namespace

namespace linux_internal {

void ensure_signal_stack() noexcept {
    if (t_signal_stack) return;
    void* memory = mmap(nullptr, kSignalStackBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (memory == MAP_FAILED) return;
    stack_t stack{};
    stack.ss_sp = memory;
    stack.ss_size = kSignalStackBytes;
    if (sigaltstack(&stack, nullptr) != 0) {
        munmap(memory, kSignalStackBytes);
        return;
    }
    t_signal_stack = memory;
}

} // namespace linux_internal

// Puts the handler first for every fault signal. A handler someone installed
// in the meantime (over ours) becomes the one ours passes faults on to.
bool install_signal_handlers() noexcept {
    linux_internal::ensure_signal_stack();
    struct sigaction action {};
    action.sa_sigaction = &signal_handler;
    // SA_NODEFER: a guarded call's siglongjmp leaves no signal blocked.
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&action.sa_mask);
    for (std::size_t i = 0; i < sizeof(kSignals) / sizeof(kSignals[0]); ++i) {
        struct sigaction old {};
        if (sigaction(kSignals[i], &action, &old) != 0) return false;
        if (!((old.sa_flags & SA_SIGINFO) && old.sa_sigaction == &signal_handler)) g_previous[i] = old;
    }
    linux_guard::g_handler_installed.store(true, std::memory_order_relaxed);
    return true;
}

bool install_fault_handler(FaultHandler handler) noexcept {
    g_handler = handler;
    return install_signal_handlers();
}

void linux_guard::install_signal_handler() noexcept {
    static std::mutex lock;
    std::lock_guard guard(lock);
    if (!linux_guard::g_handler_installed.load(std::memory_order_relaxed)) install_signal_handlers();
}

void trace_fault(const FaultRecord& record, const CpuContext& context, bool (*annotate)(std::uint64_t pc)) noexcept {
    char line[200];
    int n = std::snprintf(line, sizeof(line), "fx fault: code %08x rip %p info %p %p\n", record.code,
                          reinterpret_cast<void*>(rip(context)),
                          reinterpret_cast<void*>(record.parameter_count > 0 ? record.parameters[0] : 0),
                          reinterpret_cast<void*>(record.parameter_count > 1 ? record.parameters[1] : 0));
    write_text(line, n);
    if (record.code != kFaultAccessViolation) return;
    void* frames[24];
    const int count = backtrace(frames, 24);
    for (int i = 0; i < count; ++i) {
        Dl_info module{};
        const char* name = "?";
        std::uint64_t offset = 0;
        if (dladdr(frames[i], &module) && module.dli_fname) {
            name = module.dli_fname;
            for (const char* p = module.dli_fname; *p; ++p) if (*p == '/') name = p + 1;
            offset = reinterpret_cast<std::uint64_t>(frames[i]) - reinterpret_cast<std::uint64_t>(module.dli_fbase);
        }
        const bool marked = annotate && annotate(reinterpret_cast<std::uint64_t>(frames[i]));
        n = std::snprintf(line, sizeof(line), "    #%d %p %s+0x%llx%s\n", i, frames[i], name,
                          static_cast<unsigned long long>(offset), marked ? " (code arena)" : "");
        write_text(line, n);
    }
}

} // namespace transmute::fx::host
