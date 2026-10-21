// The dispatcher: enters translated code and handles its exits. Re-entrant
// (call_guest nests through host callbacks) and lock-free on the hit path.

#include "fx/common/interpreter.hpp"
#include "fx/loader/loader.hpp"
#include "fx/runtime/runtime_internal.hpp"
#include "fx/translator/emit_fp.hpp"
#include "fx/host/env.hpp"
#include "fx/host/text.hpp"
#include "fx/host/vm.hpp"

#include <immintrin.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>

namespace transmute::fx {

// One source of truth for the FPCR/FPSR <-> MXCSR protocol: emit_fp.hpp.
std::uint32_t mxcsr_for_guest(std::uint32_t fpcr, std::uint32_t fpsr) noexcept {
    return fp_mxcsr_for_state(fpcr, fpsr);
}

std::uint32_t fpsr_with_mxcsr_flags(std::uint32_t fpsr, std::uint32_t mxcsr) noexcept {
    return fp_read_fpsr(fpsr, mxcsr);
}

// The C++ forms of fp_boundary.hpp's emit_fp_state_to_context /
// emit_fp_state_from_context (with the ThreadState::fp_host marker).
void fp_state_to_context(GuestContext& ctx) noexcept {
    ctx.fpsr = fp_read_fpsr(ctx.fpsr, _mm_getcsr());
    if (auto* ts = static_cast<rt::ThreadState*>(ctx.thread)) ts->fp_host = 1;
}

void fp_state_from_context(GuestContext& ctx) noexcept {
    if (auto* ts = static_cast<rt::ThreadState*>(ctx.thread)) ts->fp_host = 0;
    const std::uint32_t current = _mm_getcsr();
    const std::uint32_t csr = fp_mxcsr_for_state(ctx.fpcr, ctx.fpsr) | (current & 0x2u);   // DE as it is
    ctx.fpsr &= ~kFpsrMxcsrMask;
    if (current != csr) _mm_setcsr(csr);
}

} // namespace transmute::fx

namespace transmute::fx::rt {
namespace {

std::atomic<SvcHandler> g_svc_handler{nullptr};

constexpr bool is_brk(std::uint32_t word) { return (word & 0xffe0001fu) == 0xd4200000u; }
constexpr bool is_svc(std::uint32_t word) { return (word & 0xffe0001fu) == 0xd4000001u; }
constexpr std::uint32_t imm16(std::uint32_t word) { return (word >> 5) & 0xffffu; }

void trace_exit(const GuestContext& ctx, std::uint32_t reason) {
    std::fprintf(stderr, "fx exit %u pc=0x%llx arg=0x%llx\n", reason, static_cast<unsigned long long>(ctx.pc),
                 static_cast<unsigned long long>(ctx.exit_arg));
}

// ---- diagnostics ----------------------------------------------------------------------------
// Runtime-detected failures print one line (reason, guest pc as module+offset,
// x30, sp) before the guest exception is dispatched, so an unhandled one never
// ends the process silently. Faults a guest may handle on purpose (access
// violations, breakpoints) print only with TRANSMUTE_FX_TRACE_EXCEPTIONS=1 or
// RuntimeOptions::trace_exits.
thread_local char t_interpreter_message[256];

bool trace_exceptions() {
    static const bool enabled = [] {
        char value[8] = {};
        return host::get_env("TRANSMUTE_FX_TRACE_EXCEPTIONS", value, sizeof(value)) > 0 && value[0] != '0';
    }();
    return enabled || options().trace_exits;
}

void describe(std::uint64_t address, char* out, std::size_t size) {
    if (const auto* module = find_guest_module(address)) {
        char name[128] = {};
        host::to_utf8(module->name.c_str(), name, sizeof(name) - 1);
        std::snprintf(out, size, "0x%llx (%s+0x%llx)", static_cast<unsigned long long>(address), name,
                      static_cast<unsigned long long>(address - module->base));
    } else {
        std::snprintf(out, size, "0x%llx", static_cast<unsigned long long>(address));
    }
}

std::atomic<unsigned> g_reports{0};

void report(const GuestContext& ctx, const char* reason, const char* detail = nullptr) {
    if (g_reports.fetch_add(1, std::memory_order_relaxed) >= 64) return;   // do not flood
    char pc[192], lr[192];
    describe(ctx.pc, pc, sizeof(pc));
    describe(ctx.x[30], lr, sizeof(lr));
    std::fprintf(stderr, "fx runtime: %s at guest pc %s; x30=%s sp=0x%llx thread %lu%s%s\n", reason, pc, lr,
                 static_cast<unsigned long long>(ctx.x[31]), static_cast<unsigned long>(host::current_thread_id()),
                 detail ? ": " : "",
                 detail ? detail : "");
    std::fflush(stderr);
}

void raise(GuestContext& ctx, const GuestExceptionRecord& record) {
    count(g_counters.guest_exceptions);
    if (trace_exceptions()) {
        char detail[96];
        std::snprintf(detail, sizeof(detail), "code 0x%08x params 0x%llx 0x%llx", record.code,
                      static_cast<unsigned long long>(record.parameters[0]),
                      static_cast<unsigned long long>(record.parameters[1]));
        report(ctx, "guest exception", detail);
    }
    dispatch_guest_exception(ctx, record);
}

void raise_simple(GuestContext& ctx, std::uint32_t code, std::uint32_t count_params = 0,
                  std::uint64_t p0 = 0, std::uint64_t p1 = 0) {
    GuestExceptionRecord record{};
    record.code = code;
    record.address = ctx.pc;
    record.parameter_count = count_params;
    record.parameters[0] = p0;
    record.parameters[1] = p1;
    raise(ctx, record);
}

void raise_execute_fault(GuestContext& ctx, const char* reason) {
    host::vm::RegionInfo info;
    const bool committed = host::vm::query(reinterpret_cast<const void*>(ctx.pc), info) && info.committed();
    report(ctx, reason);
    raise_simple(ctx, kStatusAccessViolation, 2, committed ? 8 : 0, ctx.pc);
}

void raise_misaligned(GuestContext& ctx) {
    report(ctx, "branch to a misaligned pc");
    raise_simple(ctx, kStatusDatatypeMisalignment);
}

void raise_brk(GuestContext& ctx, std::uint32_t immediate) {
    switch (immediate) {
    case 0xf003: __fastfail(static_cast<unsigned>(ctx.x[0]));     // __fastfail: terminate like native
    case 0xf001: raise_simple(ctx, kStatusAssertionFailure); return;
    case 0xf004: raise_simple(ctx, kStatusIntegerDivideByZero); return;
    default: raise_simple(ctx, kStatusBreakpoint, 1, 0); return;
    }
}

void handle_svc(GuestContext& ctx, std::uint32_t immediate) {
    if (auto handler = g_svc_handler.load(std::memory_order_acquire)) {
        handler(ctx, immediate);
        return;
    }
    ctx.pc -= 4;
    report(ctx, "SVC without a handler");
    raise_simple(ctx, kStatusIllegalInstruction);
}

// ---- interpreter fallback ---------------------------------------------------------------
enum class InterpretStatus { ok, illegal, fault };

InterpretStatus interpret_cpp(GuestContext& ctx) {
    try {
        interpret_one(ctx);
        return InterpretStatus::ok;
    } catch (const std::exception& error) {
        std::snprintf(t_interpreter_message, sizeof(t_interpreter_message), "%s", error.what());
        return InterpretStatus::illegal;
    }
}

// A guarded frame without C++ objects (host::call_guarded): host faults while
// interpreting (guest memory accesses) become guest exceptions. ctx is only
// written on success.
InterpretStatus interpret_guarded(GuestContext& ctx, host::FaultRecord* fault) {
    InterpretStatus status = InterpretStatus::fault;
    if (!host::call_guarded([&] { status = interpret_cpp(ctx); }, *fault)) return InterpretStatus::fault;
    return status;
}

// System registers the reference interpreter does not model.
bool interpret_system_register(GuestContext& ctx, std::uint32_t word) {
    const auto rt = word & 31u;
    const auto op = word & 0xffffffe0u;
    std::uint64_t* reg = nullptr;
    bool read = false;
    switch (op) {
    case 0xd53bd040u: reg = &ctx.tpidr_el0; read = true; break;    // MRS TPIDR_EL0
    case 0xd51bd040u: reg = &ctx.tpidr_el0; break;                 // MSR TPIDR_EL0
    case 0xd53bd060u: reg = &ctx.tpidrro_el0; read = true; break;  // MRS TPIDRRO_EL0
    default: return false;
    }
    if (read) { if (rt != 31) ctx.x[rt] = *reg; }
    else *reg = rt == 31 ? 0 : ctx.x[rt];
    ctx.pc += 4;
    return true;
}

GuestExceptionRecord record_from(const host::FaultRecord& fault, std::uint64_t pc) {
    return guest_fault_record(fault, pc);
}

// Executes the instruction at ctx.pc outside translated code (host MXCSR).
void interpret_step(GuestContext& ctx) {
    count(g_counters.interpreted);
    std::uint32_t word;
    if ((ctx.pc & 3) != 0) { raise_misaligned(ctx); return; }
    if (!safe_read32(ctx.pc, word)) { raise_execute_fault(ctx, "instruction fetch from unreadable memory"); return; }
    if (is_brk(word)) { raise_brk(ctx, imm16(word)); return; }
    if (is_svc(word)) { ctx.pc += 4; handle_svc(ctx, imm16(word)); return; }
    if (interpret_system_register(ctx, word)) return;
    host::FaultRecord fault{};
    switch (interpret_guarded(ctx, &fault)) {
    case InterpretStatus::ok: return;
    case InterpretStatus::illegal:
        report(ctx, "illegal or unsupported instruction", t_interpreter_message);
        raise_simple(ctx, kStatusIllegalInstruction);
        return;
    case InterpretStatus::fault: raise(ctx, record_from(fault, ctx.pc)); return;
    }
}

// ---- resolution -------------------------------------------------------------------------------
// Host code for ctx.pc, translating if needed, or nullptr when ctx.pc is
// kReturnToHostPc. Raises guest exceptions for non-executable targets and
// continues with the state the exception dispatcher returns.
const void* code_for_pc(GuestContext& ctx) {
    for (;;) {
        const auto pc = ctx.pc;
        // Lock-free first: the authoritative map is searched by
        // translate_and_install (guest code) or find_entry below, so a miss
        // here never takes the map lock twice (preloaders hold it often).
        if (const void* host = lookup_probe(pc)) return host;
        if (pc == kReturnToHostPc) return find_entry(pc);
        if ((pc & 3) != 0) { raise_misaligned(ctx); continue; }
        if (is_guest_code(pc)) {
            const void* host = translate_and_install(pc);
            clear_stack_residue();   // its frames held code addresses
            if (host) return host;
            raise_execute_fault(ctx, "guest code could not be read for translation");
            continue;
        }
        if (const void* host = find_entry(pc)) return host;
        namespace vm = host::vm;
        vm::RegionInfo info;
        if (vm::query(reinterpret_cast<const void*>(pc), info) && info.committed() &&
            (info.protect & (vm::kExecute | vm::kExecuteRead | vm::kExecuteReadWrite | vm::kExecuteWriteCopy))) {
            // Host code of unknown signature (e.g. a function pointer from a host API).
            const auto thunk = bind_unknown_host_target(reinterpret_cast<const void*>(pc));
            const void* host = thunk ? find_entry(thunk) : nullptr;
            if (!host && thunk && thunk != pc) host = reinterpret_cast<const void*>(thunk);
            if (host) {
                register_host_entry(pc, host);
                if (const void* entry = find_entry(pc)) return entry;
            }
            raise_execute_fault(ctx, "host code target could not be bound");
            continue;
        }
        raise_execute_fault(ctx, "branch to memory that is neither guest code nor host code");
    }
}

const void* handle_link(GuestContext& ctx, std::uint64_t field) {
    const auto target = ctx.pc;
    const void* code = code_for_pc(ctx);
    if (code && ctx.pc == target) link_exit(field, target, code);
    return code;
}

void raise_from_exit(GuestContext& ctx, ThreadState& ts, std::uint64_t code) {
    GuestExceptionRecord record{};
    if (ts.pending_valid) {
        record = ts.pending;
        ts.pending_valid = false;
    } else {
        record.code = static_cast<std::uint32_t>(code);
        record.address = ctx.pc;
    }
    raise(ctx, record);
}

// ---- call state -------------------------------------------------------------------------------
// State a host->guest call preserves for the code it interrupted: x18-x30,
// SP, PC, NZCV and d8-d15 (the outer frame may be a thunk in a host call).
struct SavedState {
    std::uint64_t x[14];   // x18..x31(sp)
    std::uint64_t pc, nzcv;
    std::uint64_t d[8];
};

void save_state(const GuestContext& ctx, SavedState& s) {
    std::memcpy(s.x, &ctx.x[18], sizeof(s.x));
    s.pc = ctx.pc;
    s.nzcv = ctx.nzcv;
    for (unsigned i = 0; i < 8; ++i) s.d[i] = ctx.v[8 + i][0];
}

void restore_state(GuestContext& ctx, const SavedState& s) {
    std::memcpy(&ctx.x[18], s.x, sizeof(s.x));
    ctx.pc = s.pc;
    ctx.nzcv = s.nzcv;
    for (unsigned i = 0; i < 8; ++i) ctx.v[8 + i][0] = s.d[i];
}

void push_return_to_host(GuestContext& ctx) {
    auto& entry = ctx.rsb[ctx.rsb_top % kRsbEntries];
    entry.guest_return = kReturnToHostPc;
    entry.host_entry = g_stubs.return_to_host;
    ++ctx.rsb_top;
}

// Runs `entry` with x30 = kReturnToHostPc; restores the saved state after.
// host_rsp and MXCSR are restored too: a C++ exception unwinding through
// translated frames (unwind info from the arena) skips dispatch_exit.
void invoke(GuestContext& ctx, std::uint64_t entry, const SavedState& saved) {
    auto& ts = *static_cast<ThreadState*>(ctx.thread);
    struct Restore {
        GuestContext& ctx;
        ThreadState& ts;
        const SavedState& saved;
        std::uint64_t host_rsp;
        unsigned mxcsr;
        std::uint32_t fp_host;
        std::uintptr_t activation_cookie;
        int exceptions = std::uncaught_exceptions();
        ~Restore() {
            --ctx.nesting;
            ctx.host_rsp = host_rsp;
            // A C++ exception unwound translated frames past dispatch_exit:
            // the guest's FPSR flags are still in MXCSR (region entry moved
            // them there), keep them before MXCSR is restored -- unless it
            // left host code at an FP boundary (a host callback), where
            // ctx.fpsr is authoritative and MXCSR's flags are stale.
            if (std::uncaught_exceptions() > exceptions && !ts.fp_host)
                ctx.fpsr = fpsr_with_mxcsr_flags(ctx.fpsr, _mm_getcsr());
            ts.fp_host = fp_host;
            if (_mm_getcsr() != mxcsr) _mm_setcsr(mxcsr);
            if (activation_cookie) host::deactivate_context(activation_cookie);
            restore_state(ctx, saved);
        }
    } restore{ctx, ts, saved, ctx.host_rsp, _mm_getcsr(), ts.fp_host, 0};
    ts.fp_host = 0;   // the nested run is guest code, not the caller's host code
    // Borrowed host threads get the guest activation context for the duration
    // of the outermost guest entry only (see ThreadState::borrowed).
    if (ctx.nesting == 0 && ts.borrowed)
        if (void* activation = guest_activation_context()) restore.activation_cookie = host::activate_context(activation);
    ctx.x[30] = kReturnToHostPc;
    ctx.pc = entry;
    push_return_to_host(ctx);
    ++ctx.nesting;
    run_guest(ctx);
}

// Writes stack arguments below the current guest SP (16-byte aligned).
void push_stack_args(GuestContext& ctx, const std::uint64_t* args, std::size_t count) {
    auto sp = ctx.x[31] & ~std::uint64_t{15};
    if (count) {
        sp -= (count * 8 + 15) & ~std::size_t{15};
        std::memcpy(reinterpret_cast<void*>(sp), args, count * 8);
    }
    ctx.x[31] = sp;
}

} // namespace

void process_interrupts(GuestContext& ctx) {
    auto& ts = *static_cast<ThreadState*>(ctx.thread);
    ctx.interrupt_pending = 0;
    std::vector<std::function<void(GuestContext&)>> requests;
    {
        ExclusiveLock lock(ts.interrupt_lock);
        requests.swap(ts.interrupts);
    }
    for (auto& request : requests) request(ctx);
}

void run_guest(GuestContext& ctx) {
    auto& ts = *static_cast<ThreadState*>(ctx.thread);
    const bool trace = options().trace_exits;
    const void* code = code_for_pc(ctx);
    while (code) {
        if (ctx.interrupt_pending) {
            process_interrupts(ctx);
            code = code_for_pc(ctx);
            if (!code) break;
        }
        ctx.exit_reason = static_cast<std::uint32_t>(ExitReason::none);
        // The mirrored FPSR flags move into MXCSR for the run, so ctx.fpsr
        // cannot hold stale copies of flags guest or host code clears in
        // MXCSR meanwhile (architectural FPSR = ctx.fpsr | flags(MXCSR),
        // emit_fp.hpp fp_read_fpsr; IDC has no MXCSR bit and stays).
        const std::uint32_t entry_mxcsr = mxcsr_for_guest(ctx.fpcr, ctx.fpsr);
        ctx.fpsr &= ~kFpsrMxcsrMask;
        const auto mxcsr = g_stubs.enter(&ctx, code, entry_mxcsr);
        ctx.fpsr = fpsr_with_mxcsr_flags(ctx.fpsr, mxcsr);
        const auto reason = ctx.exit_reason;
        const auto arg = ctx.exit_arg;
        // A link exit's argument is a field in translated code: left in the
        // context of a thread that then blocks, it would keep that code's
        // chunk from reclaim (which scans contexts) for as long as it blocks.
        ctx.exit_arg = 0;
        ++ts.exits[reason & 15];   // per-thread: no shared cache line on the exit path
        if (trace) trace_exit(ctx, reason);
        switch (static_cast<ExitReason>(reason)) {
        case ExitReason::link:
            code = handle_link(ctx, arg);
            break;
        case ExitReason::lookup_miss:
        case ExitReason::redispatch:
            code = code_for_pc(ctx);
            break;
        case ExitReason::tier_up:
            tier_up(ctx.pc);
            clear_stack_residue();
            code = code_for_pc(ctx);
            break;
        case ExitReason::tier_up_loop:   // a loop of the region at `arg` is hot; resume at its header (ctx.pc)
            tier_up(arg);
            clear_stack_residue();
            code = code_for_pc(ctx);
            break;
        case ExitReason::interpret:
            interpret_step(ctx);
            code = code_for_pc(ctx);
            break;
        case ExitReason::svc:
            handle_svc(ctx, static_cast<std::uint32_t>(arg));
            code = code_for_pc(ctx);
            break;
        case ExitReason::brk:
            raise_brk(ctx, static_cast<std::uint32_t>(arg));
            code = code_for_pc(ctx);
            break;
        case ExitReason::return_to_host:
            return;
        case ExitReason::interrupt:
            process_interrupts(ctx);
            code = code_for_pc(ctx);
            break;
        case ExitReason::raise:
            raise_from_exit(ctx, ts, arg);
            code = code_for_pc(ctx);
            break;
        default:
            report(ctx, "translated code exited with an unknown reason");
            fatal("fx runtime: unknown exit reason");
        }
    }
}

std::uint64_t host_call_bridge_entry(std::uint64_t target, const std::uint64_t* args, std::uint64_t* xmm) {
    count(g_counters.host_callbacks);
    GuestContext& ctx = ensure_thread_context();
    SavedState saved;
    save_state(ctx, saved);
    // Positional integer marshalling: Win64 slots 0..7 -> x0..x7, slots 8..15
    // -> guest stack (only those inside the host stack), xmm0..3 -> v0..v3.
    const auto stack_end = host::thread_stack_base();
    const auto slots_available = (stack_end - reinterpret_cast<std::uint64_t>(args)) / 8;
    const auto slots = static_cast<std::size_t>(std::min<std::uint64_t>(16, slots_available));
    std::uint64_t values[16] = {};
    std::memcpy(values, args, slots * 8);
    push_stack_args(ctx, values + 8, slots > 8 ? slots - 8 : 0);
    for (unsigned i = 0; i < 8; ++i) ctx.x[i] = values[i];
    for (unsigned i = 0; i < 4; ++i) {
        ctx.v[i][0] = xmm[2 * i];
        ctx.v[i][1] = xmm[2 * i + 1];
    }
    invoke(ctx, target, saved);
    xmm[0] = ctx.v[0][0];
    xmm[1] = ctx.v[0][1];
    return ctx.x[0];
}

// helper_interpret_one: the instruction at ctx->pc, without leaving translated code.
void interpret_one_helper(GuestContext* ctx) {
    fp_sync_fpsr(*ctx);   // cumulative MXCSR flags -> ctx.fpsr before the interpreter runs
    std::uint32_t word = 0;
    if ((ctx->pc & 3) != 0 || !safe_read32(ctx->pc, word)) exit_to_dispatcher(*ctx, ExitReason::redispatch, 0);
    if (is_brk(word)) exit_to_dispatcher(*ctx, ExitReason::brk, imm16(word));
    if (is_svc(word)) {
        ctx->pc += 4;
        exit_to_dispatcher(*ctx, ExitReason::svc, imm16(word));
    }
    count(g_counters.interpreted);
    if (interpret_system_register(*ctx, word)) return;
    _mm_setcsr(0x1f80);
    host::FaultRecord fault{};
    const auto status = interpret_guarded(*ctx, &fault);
    fp_load_mxcsr(*ctx);  // the interpreter may have changed FPCR/FPSR
    if (status == InterpretStatus::ok) return;
    auto& ts = *static_cast<ThreadState*>(ctx->thread);
    if (status == InterpretStatus::illegal) {
        report(*ctx, "illegal or unsupported instruction", t_interpreter_message);
        ts.pending = {};
        ts.pending.code = kStatusIllegalInstruction;
        ts.pending.address = ctx->pc;
    } else {
        ts.pending = record_from(fault, ctx->pc);
    }
    ts.pending_valid = true;
    exit_to_dispatcher(*ctx, ExitReason::raise, ts.pending.code);
}

// From translated code (loop headers): the requests get the architectural
// FPSR/FPCR in ctx, and their changes reach MXCSR before the guest resumes.
void check_interrupt_helper(GuestContext* ctx) {
    if (!ctx->interrupt_pending) return;
    fp_state_to_context(*ctx);
    process_interrupts(*ctx);
    fp_state_from_context(*ctx);
}

} // namespace transmute::fx::rt

namespace transmute::fx {

void set_svc_handler(SvcHandler handler) { rt::g_svc_handler.store(handler, std::memory_order_release); }

[[noreturn]] void exit_to_dispatcher(GuestContext& ctx, ExitReason reason, std::uint64_t arg) {
    // From host code at an FP boundary (a host callback, an interrupt
    // request): ctx.fpsr is authoritative, so the stale MXCSR flags must not
    // be folded in at the exit (the next region entry loads MXCSR from ctx).
    if (auto* ts = static_cast<rt::ThreadState*>(ctx.thread); ts && ts->fp_host) {
        ts->fp_host = 0;
        _mm_setcsr(_mm_getcsr() & ~kMxcsrStatusMask);
    }
    ctx.exit_reason = static_cast<std::uint32_t>(reason);
    ctx.exit_arg = arg;
    rt::g_stubs.abort_to_dispatcher(&ctx);
    __assume(0);
}

void call_guest_prepared(GuestContext& ctx, std::uint64_t entry) {
    rt::ensure_initialized();
    rt::SavedState saved;
    rt::save_state(ctx, saved);
    rt::invoke(ctx, entry, saved);
}

std::uint64_t call_guest(std::uint64_t entry, std::span<const std::uint64_t> args) {
    GuestContext& ctx = ensure_thread_context();
    rt::SavedState saved;
    rt::save_state(ctx, saved);
    rt::push_stack_args(ctx, args.size() > 8 ? args.data() + 8 : nullptr, args.size() > 8 ? args.size() - 8 : 0);
    for (std::size_t i = 0; i < args.size() && i < 8; ++i) ctx.x[i] = args[i];
    rt::invoke(ctx, entry, saved);   // restores x18-x31/pc/nzcv/d8-d15, not x0
    return ctx.x[0];
}

} // namespace transmute::fx
