#pragma once

// Transmute fx engine: per-thread guest context shared by translated code,
// the runtime, thunks and the exception dispatcher.
//
// Owner: integration (frozen layout). Fields may be APPENDED by components
// (keep offsets of existing fields stable). Translated code addresses this
// structure through r15 == reinterpret_cast<char*>(ctx) + kContextBias so the
// 32 integer registers are reachable with 8-bit displacements.

#include <cstddef>
#include <cstdint>

namespace transmute::fx {

inline constexpr std::int32_t kContextBias = 128;
inline constexpr unsigned kRsbEntries = 32;      // power of two
inline constexpr unsigned kSpillSlots = 64;
inline constexpr unsigned kHelperSlots = 128;

// Guest PC used as the return address of a host->guest call. Returning to it
// leaves translated code with ExitReason::return_to_host. Never a valid
// guest or host address.
inline constexpr std::uint64_t kReturnToHostPc = 0x0000'7fff'ffff'0000ULL;

enum class ExitReason : std::uint32_t {
    none = 0,
    // Unlinked direct branch. ctx->pc = target. exit_arg = host address of the
    // 4-byte aligned rel32 field the runtime may patch to link the branch.
    link = 1,
    // Indirect branch target (ctx->pc) missed the lookup table.
    lookup_miss = 2,
    // Execute exactly one instruction at ctx->pc in the fallback interpreter.
    interpret = 3,
    // SVC #imm at ctx->pc - 4 (exit_arg = imm16). Windows user code normally
    // never issues SVC; this exists for the macOS personality.
    svc = 4,
    // BRK #imm at ctx->pc (exit_arg = imm16): raise a guest breakpoint.
    brk = 5,
    // Reached kReturnToHostPc: a host->guest call finished.
    return_to_host = 6,
    // ctx->interrupt_pending was observed at a safe point.
    interrupt = 7,
    // Translated code requests a guest exception. exit_arg = NTSTATUS code;
    // ctx->pc = faulting guest instruction, state flushed.
    raise = 8,
    // The guest wrote to translated code (or invalidated it); re-dispatch ctx->pc.
    redispatch = 9,
    // (additive, translator core) A tier-0 region's entry counter reached
    // zero at its start: ctx->pc = region start, state flushed. The runtime
    // may retranslate it as a larger (tier-1) region, then re-dispatches.
    tier_up = 10,
    // (additive, translator core) A loop of a counted first-tier region ran
    // its iteration budget (tier 2): exit_arg = that region's start, ctx->pc
    // = the loop header, state flushed. The runtime may retranslate the
    // region at exit_arg (optimized), then re-dispatches ctx->pc.
    tier_up_loop = 11,
};

// A return-stack-buffer entry pushed by translated BL/BLR. A RET whose target
// equals guest_return jumps straight to host_entry (which expects the
// standard "everything flushed" entry state; the cross-region convention
// registers are in their host registers, see stub_indirect_live).
struct RsbEntry {
    std::uint64_t guest_return;
    const void* host_entry;
};

struct alignas(64) GuestContext {
    // ---- architectural integer state (disp8 from r15) ----------------------
    std::uint64_t x[32];          // x[0..30]; x[31] holds SP (never ZR).
    // ---- control state -----------------------------------------------------
    std::uint64_t pc;
    std::uint64_t nzcv;           // ARM layout: N=bit31 Z=30 C=29 V=28. Valid when flushed.
    std::uint32_t fpcr;           // guest FPCR (mirrored into MXCSR while running)
    std::uint32_t fpsr;           // guest FPSR; cumulative flags also live in MXCSR (see docs)
    std::uint64_t tpidr_el0;
    std::uint64_t tpidrro_el0;
    // ---- exit protocol -----------------------------------------------------
    std::uint32_t exit_reason;    // ExitReason
    volatile std::uint32_t interrupt_pending;
    std::uint64_t exit_arg;
    std::uint64_t host_rsp;       // host stack pointer saved by the entry stub
    // ---- SIMD/FP state -----------------------------------------------------
    alignas(64) std::uint64_t v[32][2];   // V0..V31 {low, high}
    // ---- exclusive monitor (LDXR/STXR emulated with CMPXCHG) ---------------
    std::uint64_t excl_address;
    std::uint64_t excl_value[2];
    std::uint32_t excl_size;      // bytes reserved; 0 = no reservation
    union {
        std::uint32_t excl_pad;
        // (atomics/system, additive) Nonzero when this host thread may hold a
        // store-release (STLR) in its store buffer that no full fence has
        // drained yet; translated LDAR/LDAXR fence first (RCsc). Byte access
        // only. Thread-private: one GuestContext per host thread.
        std::uint8_t release_pending;
    };
    // ---- return stack buffer -----------------------------------------------
    std::uint32_t rsb_top;        // index of the next push, modulo kRsbEntries
    std::uint32_t rsb_pad;
    alignas(16) RsbEntry rsb[kRsbEntries];
    // ---- runtime linkage (written by the runtime, read by translated code) -
    const std::uint64_t* lookup_table;   // runtime-private encoding; use stub_indirect
    std::uint64_t lookup_mask;
    const void* stub_dispatch_exit;  // jmp: return to the C++ dispatcher (exit_reason set)
    const void* stub_indirect;       // jmp: target in ctx->pc, state flushed
    const void* stub_link;           // jmp: rax = &rel32 to patch, ctx->pc = target
    const void* stub_reserved[5];
    const void* helpers[kHelperSlots];   // call [r15 + helper_disp(id)]; Win64 ABI, rcx = GuestContext*
    // ---- thread environment ------------------------------------------------
    void* shadow_teb;             // what guest x18 points at
    void* host_teb;               // NtCurrentTeb() of the host thread
    void* thread;                 // runtime-owned ThreadState*
    std::uint64_t guest_stack_base;   // highest address (exclusive)
    std::uint64_t guest_stack_limit;  // lowest committed/reserved address
    std::uint64_t host_mxcsr;     // host MXCSR saved at entry
    std::uint64_t nesting;        // host->guest call depth
    std::uint64_t reserved_env[9];
    // ---- scratch for translated code ---------------------------------------
    alignas(16) std::uint64_t spill[kSpillSlots];
    alignas(16) std::uint64_t vspill[32][2];
    // ---- appended (translator core) ------------------------------------------
    // Base of the runtime code arena (runtime_arena_base()), for translated
    // code that inlines the stub_indirect lookup (host = arena_base + (u32)
    // entry; see fx/runtime/runtime.hpp). nullptr: always use stub_indirect.
    const void* arena_base;
    // ---- appended (translator core: cross-region register convention) -----
    // Translated code keeps the convention registers (translator.hpp,
    // translator_register_convention) in host registers across its own
    // boundaries; stub_link and stub_dispatch_exit store them before leaving
    // translated code.
    // jmp: target in ctx->pc, state flushed except the convention registers
    // (still in their hosts); used by translated code, stub_indirect by
    // flushed-state code (thunks, callbacks).
    const void* stub_indirect_live;
    // jmp, from flushed-state code returning through an RSB hit (abi thunks):
    // loads the convention registers, then jmp [r15 + rdx + rsb + 8].
    // nullptr: no convention (jump to the RSB entry directly).
    const void* stub_host_return;
};

// Displacement from r15 for a GuestContext byte offset.
constexpr std::int32_t ctx_disp(std::size_t offset) noexcept {
    return static_cast<std::int32_t>(offset) - kContextBias;
}
constexpr std::int32_t x_disp(unsigned reg) noexcept {
    return ctx_disp(offsetof(GuestContext, x) + reg * 8u);
}
constexpr std::int32_t v_disp(unsigned reg, unsigned half = 0) noexcept {
    return ctx_disp(offsetof(GuestContext, v) + reg * 16u + half * 8u);
}
constexpr std::int32_t helper_disp(unsigned id) noexcept {
    return ctx_disp(offsetof(GuestContext, helpers) + id * sizeof(void*));
}
constexpr std::int32_t spill_disp(unsigned slot) noexcept {
    return ctx_disp(offsetof(GuestContext, spill) + slot * 8u);
}

static_assert(offsetof(GuestContext, x) == 0);
static_assert(x_disp(0) == -128 && x_disp(31) == 120, "x registers use disp8");
static_assert(offsetof(GuestContext, v) % 64 == 0);
static_assert(offsetof(GuestContext, rsb) % 16 == 0);

// Runtime helper slots. Ranges are owned by components so they can append
// ids without coordination. A helper is a Win64-ABI function whose first
// argument is the GuestContext* (not the biased pointer).
enum HelperId : unsigned {
    // core translator / runtime: 0..15
    helper_interpret_one = 0,     // void(GuestContext*): run the instruction at ctx->pc
    helper_check_interrupt = 1,
    // fp / simd: 16..47
    helper_fp_first = 16,
    // atomics / system registers: 48..63
    helper_system_first = 48,
    // abi / thunks: 64..95
    helper_abi_first = 64,
    // seh: 96..111
    helper_seh_first = 96,
    // cache / aot / misc: 112..127
    helper_misc_first = 112,
};

} // namespace transmute::fx
