// DarwinServices' synchronous fault signals: a guest fault (EXC_BAD_ACCESS,
// EXC_BAD_INSTRUCTION, EXC_BREAKPOINT, EXC_ARITHMETIC) runs the thread's
// sigaction handler for its signal, as xnu's trap delivery does, on the
// faulting thread and without a nested guest call: the engine's fault
// continuation (alchemy::fault_signal_delivery) points the thread at the
// handler with a signal frame on its stack, and the handler returns through
// __alchemy_sigreturn or leaves with siglongjmp.
#include "alchemy/darwin/libsystem/services_impl.hpp"

namespace alchemy {

namespace {

// The signal frame, below the faulting sp's 128-byte red zone (or at the top
// of the alternate stack): ucontext_t at sp, its mcontext64 after it, then
// siginfo_t. ucontext_t: uc_onstack @0, uc_sigmask @4, uc_stack @8
// {ss_sp, ss_size, ss_flags}, uc_link @32, uc_mcsize @40, uc_mcontext @48.
// mcontext64: es @0 {far, esr @8, exception @12}, ss @16 {x0..x28, fp @248,
// lr @256, sp @264, pc @272, cpsr @280}, ns @288 {v0..v31, fpsr @800, fpcr @804}.
// siginfo_t: si_signo @0, si_errno @4, si_code @8, si_pid @12, si_uid @16,
// si_status @20, si_addr @24 (104 bytes).
constexpr std::uint64_t kUcontext = 64, kMcontext = 816, kSiginfo = 104;
constexpr std::uint64_t kFrame = (kUcontext + kMcontext + kSiginfo + 15) & ~15ULL;
constexpr std::uint64_t kRedZone = 128;
constexpr std::uint32_t kUnblockable = (1U << 8) | (1U << 16);   // SIGKILL, SIGSTOP

} // namespace

void DarwinServices::Impl::install_fault_signals() {
    // The handler's return address. sp is the frame again when it returns;
    // the thread resumes at the mcontext's pc (the faulting instruction unless
    // the handler moved it) with every register and the mask from the frame.
    const auto sigreturn = context.install_function("__alchemy_sigreturn", [this](auto& s, auto& m) {
        const auto frame = s.sp, machine = frame + kUcontext;
        bool readable = true;
        try { validate(m, frame, kUcontext + kMcontext, false); } catch (const ServiceError&) { readable = false; }
        if (!readable || m.read64(frame + 48) != machine || m.read64(frame + 40) != kMcontext)
            throw std::runtime_error("Darwin signal handler returned with sp off its signal frame");
        for (unsigned index = 0; index < 29; ++index) s.x[index] = m.read64(machine + 16 + index * 8);
        s.x[29] = m.read64(machine + 248); s.x[30] = m.read64(machine + 256);
        s.sp = m.read64(machine + 264); s.pc = m.read64(machine + 272); s.nzcv = m.read32(machine + 280);
        for (unsigned index = 0; index < 32; ++index) s.v[index] = {m.read64(machine + 288 + index * 16), m.read64(machine + 296 + index * 16)};
        s.fpsr = m.read32(machine + 800); s.fpcr = m.read32(machine + 804);
        const auto identity = context.current_thread()->identity;
        { std::lock_guard lock(mutex); signal_masks[identity] = m.read32(frame + 4) & ~kUnblockable; }
        resume_at_pc() = true;
    }, "alchemy.internal");

    fault_signal_delivery() = [this, sigreturn](transmute::CpuState& s, transmute::GuestMemory& m, const FaultSignal& fault) {
        const auto signal = static_cast<std::size_t>(fault.signal);
        const auto bit = 1U << (signal - 1);
        const auto identity = context.current_thread()->identity;
        std::uint64_t top = s.sp - kRedZone;
        std::uint32_t mask;
        bool on_stack = false;
        SignalAction action;
        AlternateStack alternate;
        {
            std::lock_guard lock(mutex);
            action = signal_table()[signal];
            mask = signal_masks[identity];
            // Caught and not blocked; else xnu's default action (a trap is not ignorable).
            if (action.handler <= 1 || (mask & bit)) return false;
            if (const auto found = alternate_stacks.find(identity); found != alternate_stacks.end()) alternate = found->second;
        }
        if ((action.flags & 1) && !alternate.disabled) {   // SA_ONSTACK: nested handlers stay below sp
            const bool running_on = s.sp > alternate.base && s.sp <= alternate.base + alternate.size;
            if (!running_on) top = alternate.base + alternate.size;
            on_stack = true;
        }
        const auto frame = (top - kFrame) & ~15ULL, machine = frame + kUcontext, info = machine + kMcontext;
        try { validate(m, frame, kFrame, true); }
        catch (const ServiceError&) { return false; }   // no room for the frame: the default action
        m.fill_bytes(frame, 0, kFrame);
        m.write32(frame, on_stack ? 1 : 0); m.write32(frame + 4, mask);
        m.write64(frame + 8, alternate.base); m.write64(frame + 16, alternate.size);
        m.write32(frame + 24, (on_stack ? 1U : 0U) | (alternate.disabled ? 4U : 0U));
        m.write64(frame + 40, kMcontext); m.write64(frame + 48, machine);
        m.write64(machine, fault.address); m.write32(machine + 8, fault.esr); m.write32(machine + 12, fault.exception);
        for (unsigned index = 0; index < 29; ++index) m.write64(machine + 16 + index * 8, s.x[index]);
        m.write64(machine + 248, s.x[29]); m.write64(machine + 256, s.x[30]);
        m.write64(machine + 264, s.sp); m.write64(machine + 272, s.pc); m.write32(machine + 280, s.nzcv);
        for (unsigned index = 0; index < 32; ++index) {
            m.write64(machine + 288 + index * 16, s.v[index][0]); m.write64(machine + 296 + index * 16, s.v[index][1]);
        }
        m.write32(machine + 800, s.fpsr); m.write32(machine + 804, s.fpcr);
        m.write32(info, static_cast<std::uint32_t>(fault.signal)); m.write32(info + 8, static_cast<std::uint32_t>(fault.code));
        m.write64(info + 24, fault.address);
        {
            std::lock_guard lock(mutex);
            if (action.flags & 4) signal_table()[signal] = SignalAction{};   // SA_RESETHAND
            // sa_mask and, without SA_NODEFER, the signal itself while the handler runs.
            signal_masks[identity] = (mask | action.mask | ((action.flags & 0x10) ? 0U : bit)) & ~kUnblockable;
        }
        s.x[0] = fault.signal; s.x[1] = info; s.x[2] = frame;
        s.x[30] = sigreturn; s.sp = frame; s.pc = action.handler;
        resume_at_pc() = true;
        return true;
    };
}

} // namespace alchemy
