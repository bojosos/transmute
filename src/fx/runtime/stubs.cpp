// Runtime stubs, generated once into the first 64 KiB of the code arena.
//
//   offset 0  miss            ctx->pc = rcx; exit_reason = lookup_miss (empty slots of inline lookups decode here)
//             indirect        inline lookup of ctx->pc; hit -> jmp entry, miss -> miss
//             link            rax = &rel32, ctx->pc = target -> exit(link, exit_arg = rax)
//             dispatch_exit   restore the C++ frame saved by enter and return
//             enter           C++ -> translated code (Win64 function)
//             return_to_host  exit(return_to_host)
//             interpret_pc    exit(interpret) at ctx->pc
//             rsb_redispatch  ctx->pc = x30; jmp indirect
//             abort           rcx = ctx: r15 = ctx + bias; jmp dispatch_exit
//             host_call_bridge  Win64 function for host calls into guest code
//             indirect_live, dispatch_exit_live, host_return (convention)
//
// Cross-region register convention (translator.hpp,
// translator_register_convention): stubs entered from translated code (miss,
// link, return_to_host, rsb_redispatch, indirect_live, dispatch_exit_live)
// store the convention registers from their hosts first; stubs that enter
// translated code (enter, indirect hits, host_return) load them. `indirect`
// is also entered from flushed-state code (thunks, callbacks): it never
// stores, and its misses (including empty slots) use a non-storing miss.
// dispatch_exit itself never stores (aborts from helpers, VEH resumes).
// Without convention registers the *_live stubs are the plain ones.
//
// enter frame (rsp after the prolog, 16-byte aligned):
//   [rsp+0, 32)    shadow space for helpers called by translated code
//   [rsp+32]       host MXCSR        [rsp+36] guest MXCSR scratch
//   [rsp+40]       previous ctx->host_rsp (nested call_guest)
//   [rsp+64, 224)  xmm6..xmm15
//   [rsp+232, 296) rbx rbp rdi rsi r12 r13 r14 r15 (pushed), then the return address

#include "fx/common/x64_assembler.hpp"
#include "fx/runtime/runtime_internal.hpp"
#include "fx/host/vm.hpp"

#include <cstddef>
#include <cstring>
#include <stdexcept>

// translator_register_convention() lives in fx_translator; executables that
// link the runtime with a stand-in translator (the runtime tests) get no
// convention registers.
namespace transmute::fx {
const RegisterConvention& register_convention_absent() {
    static const RegisterConvention none{};
    return none;
}
} // namespace transmute::fx
#pragma comment(linker, "/alternatename:?translator_register_convention@fx@transmute@@YAAEBURegisterConvention@12@XZ=?register_convention_absent@fx@transmute@@YAAEBURegisterConvention@12@XZ")

namespace transmute::fx::rt {

Stubs g_stubs;

namespace {

using x64::Cond;
using x64::Gp;
using x64::Label;
using x64::ptr;

constexpr std::int32_t kPc = ctx_disp(offsetof(GuestContext, pc));
constexpr std::int32_t kExitReason = ctx_disp(offsetof(GuestContext, exit_reason));
constexpr std::int32_t kExitArg = ctx_disp(offsetof(GuestContext, exit_arg));
constexpr std::int32_t kHostRsp = ctx_disp(offsetof(GuestContext, host_rsp));
constexpr std::int32_t kLookupTable = ctx_disp(offsetof(GuestContext, lookup_table));

constexpr std::int32_t kFrameSize = 232;
constexpr std::int32_t kHostMxcsr = 32;
constexpr std::int32_t kGuestMxcsr = 36;
constexpr std::int32_t kOldHostRsp = 40;
constexpr std::int32_t kXmmSave = 64;
constexpr std::int32_t kBridgeFrame = 0x68;

constexpr Gp kSaved[8] = {Gp::rbx, Gp::rbp, Gp::rdi, Gp::rsi, Gp::r12, Gp::r13, Gp::r14, Gp::r15};

// ---- UNWIND_INFO ------------------------------------------------------------
enum : std::uint8_t { kPushNonvol = 0, kAllocLarge = 1, kAllocSmall = 2, kSaveXmm128 = 8 };

struct UnwindOp {
    std::uint8_t offset;     // prolog offset after the instruction
    std::uint8_t op;
    std::uint8_t info;
    std::int32_t extra;      // -1 = none, else one 16-bit slot
};

std::vector<std::uint8_t> build_unwind(const std::vector<UnwindOp>& ops, std::size_t prolog_size,
                                       bool generic) {
    std::vector<std::uint16_t> codes;
    for (auto it = ops.rbegin(); it != ops.rend(); ++it) {
        const std::uint16_t offset = generic ? 0 : it->offset;
        codes.push_back(static_cast<std::uint16_t>(offset | ((it->op | (it->info << 4)) << 8)));
        if (it->extra >= 0) codes.push_back(static_cast<std::uint16_t>(it->extra));
    }
    const auto count = codes.size();
    if (codes.size() % 2) codes.push_back(0);
    if (prolog_size > 255 || count > 255) throw std::logic_error("unwind info too large");
    std::vector<std::uint8_t> bytes = {
        1,                                                   // version 1, no flags
        static_cast<std::uint8_t>(generic ? 0 : prolog_size),
        static_cast<std::uint8_t>(count),
        0,                                                   // no frame register
    };
    for (auto code : codes) {
        bytes.push_back(static_cast<std::uint8_t>(code));
        bytes.push_back(static_cast<std::uint8_t>(code >> 8));
    }
    return bytes;
}

// ---- abi code frames ----------------------------------------------------------
// Unwind data for abi thunks/trampolines (abi_code_frame), precomputed per
// frame size so the unwind callback never allocates: [by_call][bytes / 8].
constexpr std::uint32_t kMaxAbiFrame = 8192;
std::uint32_t g_abi_unwind[2][kMaxAbiFrame / 8 + 1];

void build_abi_unwind(const std::vector<UnwindOp>& enter_ops) {
    std::vector<std::uint8_t> blob;
    std::vector<std::uint32_t> offsets[2];
    for (int by_call = 0; by_call < 2; ++by_call) {
        for (std::uint32_t bytes = 0; bytes <= kMaxAbiFrame; bytes += 8) {
            // A jmp-entered thunk sits on the enter-stub frame; a called
            // trampoline is an ordinary Win64 function. Its own allocation is
            // undone first (it is the last "prolog" operation).
            std::vector<UnwindOp> ops = by_call ? std::vector<UnwindOp>{} : enter_ops;
            if (bytes > 0 && bytes <= 128) ops.push_back({0, kAllocSmall, static_cast<std::uint8_t>((bytes - 8) / 8), -1});
            else if (bytes > 128) ops.push_back({0, kAllocLarge, 0, static_cast<std::int32_t>(bytes / 8)});
            const auto info = build_unwind(ops, 0, true);
            while (blob.size() % 4) blob.push_back(0);
            offsets[by_call].push_back(static_cast<std::uint32_t>(blob.size()));
            blob.insert(blob.end(), info.begin(), info.end());
        }
    }
    auto* memory = static_cast<std::uint8_t*>(arena_allocate(blob.size(), 16));
    std::memcpy(memory, blob.data(), blob.size());
    const auto base = static_cast<std::uint32_t>(memory - arena_base());
    for (int by_call = 0; by_call < 2; ++by_call)
        for (std::size_t i = 0; i < offsets[by_call].size(); ++i) g_abi_unwind[by_call][i] = base + offsets[by_call][i];
}

} // namespace

std::uint32_t abi_frame_unwind_rva(std::uint32_t frame_bytes, bool entered_by_call) noexcept {
    if (frame_bytes % 8 != 0 || frame_bytes > kMaxAbiFrame) return 0;
    return g_abi_unwind[entered_by_call ? 1 : 0][frame_bytes / 8];
}

void store_globals(x64::Assembler& a) {
    const auto& conv = translator_register_convention();
    for (unsigned g = 0; g < 32; ++g)
        if ((conv.guest_mask >> g) & 1u) a.mov(64, ptr(Gp::r15, x_disp(g)), x64::gp(static_cast<unsigned>(conv.host[g])));
}

void load_globals(x64::Assembler& a) {
    const auto& conv = translator_register_convention();
    for (unsigned g = 0; g < 32; ++g)
        if ((conv.guest_mask >> g) & 1u) a.mov(64, x64::gp(static_cast<unsigned>(conv.host[g])), ptr(Gp::r15, x_disp(g)));
}

void generate_stubs() {
    auto* const base = arena_base();
    x64::Assembler a(4096);
    const bool globals = translator_register_convention().guest_mask != 0;
    const Label start = a.bound_label();
    const Label dispatch_exit = a.new_label();
    const Label indirect = a.new_label();
    const Label miss = a.new_label();   // flushed state (no stores)

    // ---- miss (offset 0): entered from translated inline lookups ------------
    // An empty slot "hits" here (tag 0: a target below 4 MiB). Inline lookups
    // store ctx->pc only on their own miss path, so the target (rcx) is
    // stored here. Only inline lookups come here: `indirect` checks for empty
    // slots and takes `miss`.
    a.mov(64, ptr(Gp::r15, kPc), Gp::rcx);
    store_globals(a);
    a.bind(miss);
    a.mov(32, ptr(Gp::r15, kExitReason), static_cast<std::int32_t>(ExitReason::lookup_miss));
    a.jmp(dispatch_exit);

    // ---- indirect ----------------------------------------------------------
    a.align(16);
    const auto indirect_offset = a.size();
    a.bind(indirect);
    a.mov(64, Gp::rax, ptr(Gp::r15, kPc));
    a.test(8, Gp::rax, 3);
    a.jcc(Cond::ne, miss);
    a.mov(64, Gp::rcx, Gp::rax);
    a.shr(64, Gp::rax, static_cast<std::uint8_t>(kLookupTagShift));
    a.shr(64, Gp::rcx, 2);
    a.xor_(32, Gp::rcx, Gp::rax);
    a.and_(32, Gp::rcx, static_cast<std::int32_t>(kLookupMask));
    a.mov(64, Gp::rdx, ptr(Gp::r15, kLookupTable));
    a.mov(64, Gp::rdx, ptr(Gp::rdx, Gp::rcx, 8));
    a.test(64, Gp::rdx, Gp::rdx);   // an empty slot would "hit" offset 0, which stores rcx and the globals
    a.jcc(Cond::e, miss);
    a.mov(64, Gp::rcx, Gp::rdx);
    a.shr(64, Gp::rcx, 32);
    a.cmp(64, Gp::rax, Gp::rcx);
    a.jcc(Cond::ne, miss);
    a.mov(32, Gp::rax, Gp::rdx);
    a.lea(64, Gp::rcx, x64::rip(start));
    a.add(64, Gp::rax, Gp::rcx);
    load_globals(a);   // rax, rcx, rdx are not convention hosts
    a.jmp(Gp::rax);

    // ---- indirect_live: from translated code ---------------------------------
    std::size_t indirect_live_offset = indirect_offset;
    if (globals) {
        a.align(16);
        indirect_live_offset = a.size();
        store_globals(a);
        a.jmp(indirect);
    }

    // ---- table shadows (runtime.hpp): rcx = target, rdx = index ----------------
    // miss: an empty slot (or another table's BR switching this thread's shadow).
    a.align(16);
    const auto shadow_miss_offset = a.size();
    a.mov(64, ptr(Gp::r15, kPc), Gp::rcx);
    a.mov(64, ptr(Gp::r15, kExitArg), Gp::rdx);
    store_globals(a);
    a.mov(32, ptr(Gp::r15, kExitReason), static_cast<std::int32_t>(ExitReason::table_shadow));
    a.jmp(dispatch_exit);
    // lookup: a slot that holds another target; rcx takes the lookup.
    a.align(16);
    const auto shadow_lookup_offset = a.size();
    a.mov(64, ptr(Gp::r15, kPc), Gp::rcx);
    store_globals(a);
    a.jmp(indirect);

    // ---- rsb_push (helper_rsb_push): rdx = guest return pc, r8 = host continuation --
    // Entered by call from translated code (TRANSMUTE_FX_CORE=rsbcall); keeps
    // EFLAGS, clobbers rax and r9, as the in-line push (region_emitter.cpp).
    a.align(16);
    const auto rsb_push_offset = a.size();
    {
        const auto top = ptr(Gp::r15, ctx_disp(offsetof(GuestContext, rsb_top)));
        const auto rsb = ctx_disp(offsetof(GuestContext, rsb));
        a.movzx(32, Gp::rax, top, 8);
        a.lea(32, Gp::r9, ptr(Gp::rax, 1));
        a.mov(8, top, Gp::r9);
        a.lea(32, Gp::rax, ptr(Gp::rax, Gp::rax, 1));
        a.mov(64, ptr(Gp::r15, Gp::rax, 8, rsb), Gp::rdx);
        a.mov(64, ptr(Gp::r15, Gp::rax, 8, rsb + 8), Gp::r8);
        a.ret();
    }

    // ---- link --------------------------------------------------------------
    a.align(16);
    const auto link_offset = a.size();
    store_globals(a);
    a.mov(64, ptr(Gp::r15, kExitArg), Gp::rax);
    a.mov(32, ptr(Gp::r15, kExitReason), static_cast<std::int32_t>(ExitReason::link));
    a.jmp(dispatch_exit);

    // ---- return_to_host ----------------------------------------------------
    a.align(16);
    const auto return_offset = a.size();
    store_globals(a);
    a.mov(32, ptr(Gp::r15, kExitReason), static_cast<std::int32_t>(ExitReason::return_to_host));
    a.jmp(dispatch_exit);

    // ---- interpret_pc ------------------------------------------------------
    a.align(16);
    const auto interpret_offset = a.size();
    store_globals(a);   // entered from enter (convention registers loaded)
    a.mov(32, ptr(Gp::r15, kExitReason), static_cast<std::int32_t>(ExitReason::interpret));
    a.jmp(dispatch_exit);

    // ---- rsb_redispatch ----------------------------------------------------
    a.align(16);
    const auto rsb_offset = a.size();
    store_globals(a);   // entered by translated RET (or a host return that loaded them)
    a.mov(64, Gp::rax, ptr(Gp::r15, x_disp(30)));
    a.mov(64, ptr(Gp::r15, kPc), Gp::rax);
    a.jmp(indirect);

    // ---- abort_to_dispatcher(ctx) -----------------------------------------
    a.align(16);
    const auto abort_offset = a.size();
    a.lea(64, Gp::r15, ptr(Gp::rcx, kContextBias));
    a.jmp(dispatch_exit);

    // ---- dispatch_exit_live: translated exits with a reason ---------------
    std::size_t exit_live_offset = 0;
    if (globals) {
        a.align(16);
        exit_live_offset = a.size();
        store_globals(a);
        a.jmp(dispatch_exit);
    }

    // ---- host_return: flushed-state RSB hit (abi thunks), rdx = slot ---------
    std::size_t host_return_offset = 0;
    if (globals) {
        a.align(16);
        host_return_offset = a.size();
        load_globals(a);
        a.jmp(ptr(Gp::r15, Gp::rdx, 1, ctx_disp(offsetof(GuestContext, rsb)) + 8));
    }

    // ---- dispatch_exit -----------------------------------------------------
    a.align(16);
    const auto exit_offset = a.size();
    if (!globals) exit_live_offset = exit_offset;
    a.bind(dispatch_exit);
    a.mov(64, Gp::rsp, ptr(Gp::r15, kHostRsp));
    a.stmxcsr(ptr(Gp::rsp, kGuestMxcsr));
    {
        const Label same = a.new_label();     // ldmxcsr only when the value changes
        a.mov(32, Gp::rax, ptr(Gp::rsp, kGuestMxcsr));
        a.cmp(32, Gp::rax, ptr(Gp::rsp, kHostMxcsr));
        a.jcc_short(Cond::e, same);
        a.ldmxcsr(ptr(Gp::rsp, kHostMxcsr));
        a.bind(same);
    }
    a.mov(64, Gp::rax, ptr(Gp::rsp, kOldHostRsp));
    a.mov(64, ptr(Gp::r15, kHostRsp), Gp::rax);
    for (unsigned i = 0; i < 10; ++i)
        a.movdqa(x64::xmm(6 + i), ptr(Gp::rsp, kXmmSave + 16 * static_cast<std::int32_t>(i)));
    a.mov(32, Gp::rax, ptr(Gp::rsp, kGuestMxcsr));
    a.add(64, Gp::rsp, kFrameSize);
    for (int i = 7; i >= 0; --i) a.pop(kSaved[i]);
    a.ret();

    // ---- enter(ctx, code, mxcsr) -------------------------------------------
    a.align(16);
    const auto enter_offset = a.size();
    std::vector<UnwindOp> enter_ops;
    const auto here = [&] { return static_cast<std::uint8_t>(a.size() - enter_offset); };
    for (auto reg : kSaved) {
        a.push(reg);
        enter_ops.push_back({here(), kPushNonvol, static_cast<std::uint8_t>(x64::id(reg)), -1});
    }
    a.sub(64, Gp::rsp, kFrameSize);
    enter_ops.push_back({here(), kAllocLarge, 0, kFrameSize / 8});
    for (unsigned i = 0; i < 10; ++i) {
        const std::int32_t slot = kXmmSave + 16 * static_cast<std::int32_t>(i);
        a.movdqa(ptr(Gp::rsp, slot), x64::xmm(6 + i));
        enter_ops.push_back({here(), kSaveXmm128, static_cast<std::uint8_t>(6 + i), slot / 16});
    }
    const auto enter_prolog = a.size() - enter_offset;
    a.stmxcsr(ptr(Gp::rsp, kHostMxcsr));
    a.mov(32, ptr(Gp::rsp, kGuestMxcsr), Gp::r8);
    {
        const Label same = a.new_label();
        a.cmp(32, Gp::r8, ptr(Gp::rsp, kHostMxcsr));
        a.jcc_short(Cond::e, same);
        a.ldmxcsr(ptr(Gp::rsp, kGuestMxcsr));
        a.bind(same);
    }
    a.lea(64, Gp::r15, ptr(Gp::rcx, kContextBias));
    a.mov(64, Gp::rax, ptr(Gp::r15, kHostRsp));
    a.mov(64, ptr(Gp::rsp, kOldHostRsp), Gp::rax);
    a.mov(64, ptr(Gp::r15, kHostRsp), Gp::rsp);
    load_globals(a);   // rdx (code) is not a convention host
    a.jmp(Gp::rdx);
    const auto enter_end = a.size();

    // ---- host_call_bridge --------------------------------------------------
    // Entered (by redirecting an execute fault) exactly as if the host had
    // called the guest function: [rsp] = return address, Win64 arguments,
    // r10 = guest target.
    a.align(16);
    const auto bridge_offset = a.size();
    std::vector<UnwindOp> bridge_ops;
    a.mov(64, ptr(Gp::rsp, 8), Gp::rcx);
    a.mov(64, ptr(Gp::rsp, 16), Gp::rdx);
    a.mov(64, ptr(Gp::rsp, 24), Gp::r8);
    a.mov(64, ptr(Gp::rsp, 32), Gp::r9);
    a.sub(64, Gp::rsp, kBridgeFrame);
    bridge_ops.push_back({static_cast<std::uint8_t>(a.size() - bridge_offset), kAllocSmall,
                          static_cast<std::uint8_t>((kBridgeFrame - 8) / 8), -1});
    const auto bridge_prolog = a.size() - bridge_offset;
    for (unsigned i = 0; i < 4; ++i) a.movdqu(ptr(Gp::rsp, 32 + 16 * static_cast<std::int32_t>(i)), x64::xmm(i));
    a.mov(64, Gp::rcx, Gp::r10);
    a.lea(64, Gp::rdx, ptr(Gp::rsp, kBridgeFrame + 8));
    a.lea(64, Gp::r8, ptr(Gp::rsp, 32));
    a.mov_imm64(Gp::rax, reinterpret_cast<std::uint64_t>(&host_call_bridge_entry));
    a.call(Gp::rax);
    a.movdqu(x64::xmm(0), ptr(Gp::rsp, 32));
    a.add(64, Gp::rsp, kBridgeFrame);
    a.ret();
    const auto bridge_end = a.size();

    a.finalize();

    // ---- unwind data -------------------------------------------------------
    auto& bytes = a.bytes();
    const auto append = [&](const std::vector<std::uint8_t>& data) {
        while (bytes.size() % 4) bytes.push_back(0xcc);
        const auto rva = static_cast<std::uint32_t>(bytes.size());
        bytes.insert(bytes.end(), data.begin(), data.end());
        return rva;
    };
    const auto enter_unwind = append(build_unwind(enter_ops, enter_prolog, false));
    const auto generic_unwind = append(build_unwind(enter_ops, enter_prolog, true));
    const auto bridge_unwind = append(build_unwind(bridge_ops, bridge_prolog, false));
    if (bytes.size() > kStubAreaSize) fatal("fx runtime: stub area overflow");

    std::memcpy(base, bytes.data(), bytes.size());
    host::vm::flush_instruction_cache(base, bytes.size());

    g_stubs.miss = base;
    g_stubs.indirect = base + indirect_offset;
    g_stubs.indirect_live = base + indirect_live_offset;
    g_stubs.dispatch_exit_live = base + exit_live_offset;
    g_stubs.host_return = globals ? base + host_return_offset : nullptr;
    g_stubs.link = base + link_offset;
    g_stubs.rsb_push = base + rsb_push_offset;
    g_stubs.table_shadow_miss = base + shadow_miss_offset;
    g_stubs.table_shadow_lookup = base + shadow_lookup_offset;
    g_stubs.return_to_host = base + return_offset;
    g_stubs.rsb_redispatch = base + rsb_offset;
    g_stubs.interpret_pc = base + interpret_offset;
    g_stubs.abort_to_dispatcher = reinterpret_cast<AbortFn>(base + abort_offset);
    g_stubs.dispatch_exit = base + exit_offset;
    g_stubs.enter = reinterpret_cast<EnterFn>(base + enter_offset);
    g_stubs.host_call_bridge = base + bridge_offset;

    std::vector<host::UnwindEntry> precise = {
        {static_cast<std::uint32_t>(enter_offset), static_cast<std::uint32_t>(enter_end), enter_unwind},
        {static_cast<std::uint32_t>(bridge_offset), static_cast<std::uint32_t>(bridge_end), bridge_unwind},
    };
    build_abi_unwind(enter_ops);
    arena_install_unwind(precise, generic_unwind);
}

const void* make_trampoline(const void* target) {
    x64::Assembler a(32);
    const Label data = a.new_label();
    a.jmp(x64::rip(data));
    a.bind(data);
    a.dq(reinterpret_cast<std::uint64_t>(target));
    a.finalize();
    auto* memory = static_cast<std::uint8_t*>(arena_allocate(a.size(), 16));
    std::memcpy(memory, a.data(), a.size());
    host::vm::flush_instruction_cache(memory, a.size());
    return memory;
}

} // namespace transmute::fx::rt
