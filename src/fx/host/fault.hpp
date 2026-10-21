#pragma once

// Host area "fault": the host's synchronous faults (docs/FX_HOST_SEAMS.md,
// "faults").
//
// The runtime installs one first-chance handler (a vectored exception
// handler on win32; sigaction(SIGSEGV/SIGBUS/SIGILL/SIGTRAP) on Linux). The
// handler sees the fault as a FaultRecord and the interrupted host thread's
// registers as a CpuContext, and either resumes the thread (with registers
// it may have changed) or passes the fault on.
//
// FaultRecord is fx's guest exception model (NTSTATUS codes, access-type
// parameters), which has the layout of the Win64 EXCEPTION_RECORD so the
// win32 backend passes the host's record through. A Linux backend fills one
// from siginfo_t: SIGSEGV -> kStatusAccessViolation with parameters
// {access, si_addr}.

#include "fx/host/host.hpp"

#include <cstdint>

namespace transmute::fx::host {

// NTSTATUS codes of host faults (and of the guest exceptions fx raises).
inline constexpr std::uint32_t kFaultAccessViolation = 0xC0000005u;
inline constexpr std::uint32_t kFaultInPageError = 0xC0000006u;
inline constexpr std::uint32_t kFaultGuardPage = 0x80000001u;
inline constexpr std::uint32_t kFaultDatatypeMisalignment = 0x80000002u;
inline constexpr std::uint32_t kFaultBreakpoint = 0x80000003u;
inline constexpr std::uint32_t kFaultIllegalInstruction = 0xC000001Du;
inline constexpr std::uint32_t kFaultIntegerDivideByZero = 0xC0000094u;
inline constexpr std::uint32_t kFaultIntegerOverflow = 0xC0000095u;
inline constexpr std::uint32_t kFaultPrivilegedInstruction = 0xC0000096u;
inline constexpr std::uint32_t kFaultStackOverflow = 0xC00000FDu;
inline constexpr std::uint32_t kFaultNoncontinuable = 0x1u;   // FaultRecord::flags
// FaultRecord::parameters[0] of an access violation.
inline constexpr std::uint64_t kFaultRead = 0;
inline constexpr std::uint64_t kFaultWrite = 1;
inline constexpr std::uint64_t kFaultExecute = 8;

struct FaultRecord {
    std::uint32_t code;
    std::uint32_t flags;
    FaultRecord* chained;
    void* address;               // host pc of the fault
    std::uint32_t parameter_count;
    std::uint64_t parameters[15];   // access violation: {access, data address}
};
static_assert(sizeof(FaultRecord) == 0x98);

// The interrupted thread's x64 register state (the win32 CONTEXT; a
// ucontext_t on Linux). Accessors are inline in the backend:
//   std::uint64_t& rip(CpuContext&), rsp(CpuContext&)
//   std::uint64_t& gpr(CpuContext&, unsigned reg)   x64 encoding: 0 rax .. 15 r15
//   std::uint32_t eflags(const CpuContext&)
//   std::uint8_t* xmm(CpuContext&, unsigned reg)    16 bytes, 0..15
struct CpuContext;

enum class FaultAction { resume, pass };
using FaultHandler = FaultAction (*)(FaultRecord& record, CpuContext& context);

// Installs `handler` ahead of every other handler; false if the host refused.
bool install_fault_handler(FaultHandler handler) noexcept;

// Writes a one-line description of the fault to stderr and, for access
// violations, the host call stack with module offsets. `annotate` marks
// frames (" (code arena)") for which it returns true.
void trace_fault(const FaultRecord& record, const CpuContext& context, bool (*annotate)(std::uint64_t pc)) noexcept;

} // namespace transmute::fx::host

#if defined(TRANSMUTE_FX_HOST_WIN32)
#include "fx/host/win32/fault.hpp"
#endif
