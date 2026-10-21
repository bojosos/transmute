// x64 code generation for guest->host thunks, override thunks and
// host->guest callback trampolines (owner: abi). See NOTES.md for the entry
// and return protocol.

#include "fx/abi/abi_internal.hpp"
#include "fx/common/x64_assembler.hpp"
#include "fx/runtime/fp_boundary.hpp"

#include <algorithm>
#include <cstddef>
#include <stdexcept>

namespace transmute::fx::abi {

using x64::Assembler;
using x64::Cond;
using x64::Gp;
using x64::Label;
using x64::Mem;
using x64::Xmm;
using x64::ptr;

namespace {

constexpr Gp kCtx = Gp::r15;
constexpr Gp kReturn = Gp::rbx;    // guest x30, host callee-saved across the call
constexpr Gp kGuestSp = Gp::rsi;   // guest SP, host callee-saved across helper calls
constexpr Gp kWinGpr[4] = {Gp::rcx, Gp::rdx, Gp::r8, Gp::r9};

// Extra guest stack slots forwarded for variadic and unknown signatures.
constexpr unsigned kVariadicStackSlots = 16;
constexpr unsigned kUnknownStackSlots = 8;

Mem field(std::size_t offset) { return ptr(kCtx, ctx_disp(offset)); }
Mem xreg(unsigned n) { return ptr(kCtx, x_disp(n)); }
Mem vreg(unsigned n, unsigned half = 0) { return ptr(kCtx, v_disp(n, half)); }

Mem piece_mem(const ArmPiece& p) {
    switch (p.kind) {
    case ArmPiece::gpr: return xreg(p.index);
    case ArmPiece::vreg: return vreg(p.index);
    case ArmPiece::stack: return ptr(kGuestSp, static_cast<std::int32_t>(p.offset));
    }
    throw std::logic_error("bad ArmPiece");
}

std::uint32_t round_up(std::uint32_t v, std::uint32_t a) { return (v + a - 1) & ~(a - 1); }

void emit_mirror_last_error(Assembler& a) {
    // mov eax, gs:[0x68]   (TEB.LastErrorValue, same offset in the ARM64 TEB)
    a.db(0x65);
    Mem teb_last_error;
    teb_last_error.disp = 0x68;
    a.mov(32, Gp::rax, teb_last_error);
    a.mov(64, Gp::rcx, field(offsetof(GuestContext, shadow_teb)));
    const Label skip = a.new_label();
    a.test(64, Gp::rcx, Gp::rcx);
    a.jcc_short(Cond::z, skip);
    a.mov(32, ptr(Gp::rcx, 0x68), Gp::rax);
    a.bind(skip);
}

// Returns to the guest address in rbx: RSB top hit -> its host entry;
// otherwise ctx->pc = x30 and the runtime's indirect-branch stub. x30 is
// written back first: nested callbacks may have clobbered ctx->x[30], and
// scrubbed RSB entries (the runtime's rsb_redispatch) re-read it.
void emit_return_to_guest(Assembler& a) {
    const auto rsb = ctx_disp(offsetof(GuestContext, rsb));
    a.mov(64, xreg(30), kReturn);
    a.mov(32, Gp::rax, field(offsetof(GuestContext, rsb_top)));
    a.sub(32, Gp::rax, 1);
    a.and_(32, Gp::rax, static_cast<std::int32_t>(kRsbEntries - 1));
    a.mov(32, Gp::rdx, Gp::rax);
    a.shl(32, Gp::rdx, 4);
    a.cmp(64, kReturn, ptr(kCtx, Gp::rdx, 1, rsb));
    const Label miss = a.new_label(), direct = a.new_label();
    a.jcc_short(Cond::ne, miss);
    a.mov(32, field(offsetof(GuestContext, rsb_top)), Gp::rax);
    // (translator core, additive) Cross-region register convention: the RSB
    // entry expects the convention registers in their host registers; the
    // runtime's stub_host_return loads them and does this jump (nullptr:
    // no convention, e.g. the abi tests' zeroed contexts).
    a.mov(64, Gp::rax, field(offsetof(GuestContext, stub_host_return)));
    a.test(64, Gp::rax, Gp::rax);
    a.jcc_short(Cond::z, direct);
    a.jmp(Gp::rax);
    a.bind(direct);
    a.jmp(ptr(kCtx, Gp::rdx, 1, rsb + 8));
    a.bind(miss);
    a.mov(64, field(offsetof(GuestContext, pc)), kReturn);
    a.jmp(field(offsetof(GuestContext, stub_indirect)));
}

void store_zero_high(Assembler& a, unsigned v) { a.mov(64, vreg(v, 1), 0); }

// Copies `count` 8-byte slots from [rsi + src] to [rsp + dst], never reading
// at or above ctx->guest_stack_base (missing slots are zeroed).
void emit_guest_stack_copy(Assembler& a, std::int32_t src, std::int32_t dst, unsigned count) {
    if (count == 0) return;
    const Label fast = a.new_label(), done = a.new_label(), loop = a.new_label(),
                zero = a.new_label(), next = a.new_label();
    a.mov(64, Gp::rax, field(offsetof(GuestContext, guest_stack_base)));
    a.test(64, Gp::rax, Gp::rax);
    a.jcc(Cond::z, fast);
    a.sub(64, Gp::rax, kGuestSp);
    if (src != 0) a.sub(64, Gp::rax, src);
    a.cmp(64, Gp::rax, static_cast<std::int32_t>(count * 8));
    a.jcc(Cond::ge, fast);
    // Slow path: rax = bytes readable from the source start (may be <= 0).
    a.mov_imm(Gp::rcx, 0);
    a.bind(loop);
    a.cmp(64, Gp::rcx, static_cast<std::int32_t>(count));
    a.jcc(Cond::ae, done);
    a.mov(64, Gp::rdx, Gp::rcx);
    a.shl(64, Gp::rdx, 3);
    a.cmp(64, Gp::rdx, Gp::rax);
    a.jcc(Cond::ge, zero);
    a.mov(64, Gp::r10, ptr(kGuestSp, Gp::rcx, 8, src));
    a.mov(64, ptr(Gp::rsp, Gp::rcx, 8, dst), Gp::r10);
    a.jmp(next);
    a.bind(zero);
    a.mov(64, ptr(Gp::rsp, Gp::rcx, 8, dst), 0);
    a.bind(next);
    a.add(64, Gp::rcx, 1);
    a.jmp(loop);
    a.bind(fast);
    for (unsigned k = 0; k < count; ++k) {
        a.mov(64, Gp::rax, ptr(kGuestSp, src + static_cast<std::int32_t>(8 * k)));
        a.mov(64, ptr(Gp::rsp, dst + static_cast<std::int32_t>(8 * k)), Gp::rax);
    }
    a.bind(done);
}

// How the Win64 value of an argument is produced.
struct ValueSource {
    enum Kind : std::uint8_t { memory, address } kind{memory};
    Mem mem;            // memory: 8-byte load; address: lea
};

struct RegLoad {
    unsigned pos;
    ValueSource src;
    bool fp;            // xmm (non-variadic floating point)
    bool fp32;
    bool duplicate;     // variadic: also copy the GPR into xmm<pos>
};

void emit_load(Assembler& a, Gp dst, const ValueSource& src) {
    if (src.kind == ValueSource::address) a.lea(64, dst, src.mem);
    else a.mov(64, dst, src.mem);
}

void emit_int_extension(Assembler& a, TypeKind kind) {
    switch (kind) {
    case TypeKind::i8: a.movsx(64, Gp::rax, Gp::rax, 8); break;
    case TypeKind::u8: a.movzx(32, Gp::rax, Gp::rax, 8); break;
    case TypeKind::i16: a.movsx(64, Gp::rax, Gp::rax, 16); break;
    case TypeKind::u16: a.movzx(32, Gp::rax, Gp::rax, 16); break;
    case TypeKind::i32: a.movsx(64, Gp::rax, Gp::rax, 32); break;
    case TypeKind::u32: a.mov(32, Gp::rax, Gp::rax); break;
    default: break;
    }
}

void emit_record_extension(Assembler& a, std::uint32_t size) {
    if (size == 1) a.movzx(32, Gp::rax, Gp::rax, 8);
    else if (size == 2) a.movzx(32, Gp::rax, Gp::rax, 16);
    else if (size == 4) a.mov(32, Gp::rax, Gp::rax);
}

} // namespace

namespace {
// trace_import_call(trace, x30) at thunk entry: rsp is the 16-aligned entry
// rsp with callee shadow space, and all guest state is still in ctx.
void emit_trace_call(Assembler& a, TraceInfo* trace) {
    if (trace == nullptr) return;
    a.mov_imm64(Gp::rcx, reinterpret_cast<std::uint64_t>(trace));
    a.mov(64, Gp::rdx, kReturn);
    a.mov_imm64(Gp::rax, reinterpret_cast<std::uint64_t>(&trace_import_call));
    a.call(Gp::rax);
}
} // namespace

ThunkCode generate_host_thunk(const Signature& sig, const void* native, TraceInfo* trace) {
    Assembler a(512);
    ThunkCode out;
    const bool unknown = sig.unknown;
    const CallPlan plan = unknown ? CallPlan{} : plan_call(sig);

    // ---- positions and frame layout -------------------------------------------
    unsigned extra_reg_slots = 0;     // variadic: x registers after the named arguments
    unsigned extra_stack_slots = 0;   // variadic/unknown: guest stack slots forwarded
    unsigned extra_stack_first = 0;   // first forwarded guest stack slot (index from SP)
    unsigned positions = plan.win_slots;
    if (unknown) {
        positions = 8 + kUnknownStackSlots;
        extra_stack_slots = kUnknownStackSlots;
    } else if (plan.variadic) {
        const unsigned named = plan.arm_named_slots;
        extra_reg_slots = named < 8 ? 8 - named : 0;
        extra_stack_slots = kVariadicStackSlots;
        extra_stack_first = named > 8 ? named - 8 : 0;
        positions = plan.win_slots + extra_reg_slots + extra_stack_slots;
    }
    std::uint32_t frame = 8 * std::max(4u, positions);

    // Temporaries: function-pointer results, record copies, return buffer.
    std::vector<std::int32_t> fn_temp(plan.args.size(), -1);
    std::vector<std::int32_t> rec_temp(plan.args.size(), -1);
    for (std::size_t i = 0; i < plan.args.size(); ++i) {
        const auto& arg = plan.args[i];
        if (arg.type.kind == TypeKind::fnptr) {
            fn_temp[i] = static_cast<std::int32_t>(frame);
            frame += 8;
        }
    }
    frame = round_up(frame, 16);
    for (std::size_t i = 0; i < plan.args.size(); ++i) {
        const auto& arg = plan.args[i];
        if (!arg.type.is_aggregate() || arg.arm_indirect) continue;
        const bool direct = !arg.win_by_reference && arg.arm.size() == 1 &&
                            arg.arm[0].kind != ArmPiece::vreg;
        if (direct) continue;
        rec_temp[i] = static_cast<std::int32_t>(frame);
        frame += round_up(std::max<std::uint32_t>(arg.type.size, 8), 16);
    }
    std::int32_t ret_temp = -1;
    if (plan.win_hidden_return && plan.ret_kind != ReturnKind::record_memory) {
        ret_temp = static_cast<std::int32_t>(frame);
        frame += round_up(plan.ret.size, 16);
    }
    frame = round_up(frame, 16);
    // Up to four positions and no temporaries: the entry rsp already provides
    // the callee's 32-byte shadow space, so no frame is needed (and rsp stays
    // equal to ctx->host_rsp across the call).
    if (frame == 32) frame = 0;

    bool needs_guest_sp = unknown || extra_stack_slots != 0;
    for (const auto& arg : plan.args)
        for (const auto& p : arg.arm)
            if (p.kind == ArmPiece::stack) needs_guest_sp = true;

    // ---- prologue -------------------------------------------------------------
    a.mov(64, kReturn, xreg(30));
    emit_trace_call(a, trace);
    if (frame != 0) a.sub(64, Gp::rsp, static_cast<std::int32_t>(frame));
    out.prolog_bytes = static_cast<std::uint32_t>(a.size());
    out.frame_bytes = frame;
    out.epilog_offset = SIZE_MAX;
    if (needs_guest_sp) a.mov(64, kGuestSp, xreg(31));

    // ---- function-pointer arguments (helper calls clobber volatile registers) --
    for (std::size_t i = 0; i < plan.args.size(); ++i) {
        if (fn_temp[i] < 0) continue;
        const auto& arg = plan.args[i];
        const Label skip = a.new_label();
        a.mov(64, Gp::rcx, piece_mem(arg.arm[0]));
        a.mov(64, ptr(Gp::rsp, fn_temp[i]), Gp::rcx);
        a.test(64, Gp::rcx, Gp::rcx);
        a.jcc(Cond::z, skip);
        a.mov_imm64(Gp::rdx, reinterpret_cast<std::uint64_t>(arg.type.fn));
        a.mov_imm64(Gp::rax, reinterpret_cast<std::uint64_t>(&wrap_callback_argument));
        a.call(Gp::rax);
        a.mov(64, ptr(Gp::rsp, fn_temp[i]), Gp::rax);
        a.bind(skip);
    }

    // ---- aggregate copies -------------------------------------------------------
    for (std::size_t i = 0; i < plan.args.size(); ++i) {
        if (rec_temp[i] < 0) continue;
        const auto& arg = plan.args[i];
        for (std::size_t k = 0; k < arg.arm.size(); ++k) {
            const auto& p = arg.arm[k];
            const auto off = rec_temp[i] + static_cast<std::int32_t>(
                p.kind == ArmPiece::vreg ? k * arg.type.hfa_element_size() : k * 8);
            if (p.bytes == 4) {
                a.mov(32, Gp::rax, piece_mem(p));
                a.mov(32, ptr(Gp::rsp, off), Gp::rax);
            } else {
                a.mov(64, Gp::rax, piece_mem(p));
                a.mov(64, ptr(Gp::rsp, off), Gp::rax);
            }
        }
    }

    // ---- value sources ------------------------------------------------------------
    const auto source_of = [&](std::size_t i) {
        const auto& arg = plan.args[i];
        ValueSource src;
        if (fn_temp[i] >= 0) {
            src.mem = ptr(Gp::rsp, fn_temp[i]);
        } else if (rec_temp[i] >= 0) {
            src.mem = ptr(Gp::rsp, rec_temp[i]);
            if (arg.win_by_reference) src.kind = ValueSource::address;
        } else {
            src.mem = piece_mem(arg.arm[0]);
        }
        return src;
    };

    std::vector<RegLoad> reg_loads;
    if (unknown) {
        for (unsigned n = 0; n < 8; ++n) {
            ValueSource src;
            src.mem = xreg(n);
            if (n < 4) {
                reg_loads.push_back({n, src, false, false, false});
            } else {
                a.mov(64, Gp::rax, src.mem);
                a.mov(64, ptr(Gp::rsp, static_cast<std::int32_t>(8 * n)), Gp::rax);
            }
        }
        emit_guest_stack_copy(a, 0, 64, kUnknownStackSlots);
    } else {
        // Named arguments.
        for (std::size_t i = 0; i < plan.args.size(); ++i) {
            const auto& arg = plan.args[i];
            const auto src = source_of(i);
            if (arg.win_pos < 4) {
                const bool fp = arg.win_fp && !plan.variadic;
                reg_loads.push_back({arg.win_pos, src, fp, arg.type.kind == TypeKind::f32, plan.variadic});
            } else {
                emit_load(a, Gp::rax, src);
                a.mov(64, ptr(Gp::rsp, 8 * arg.win_pos), Gp::rax);
            }
        }
        // Variadic tail: x registers after the named slots, then guest stack.
        if (plan.variadic) {
            unsigned pos = plan.win_slots;
            for (unsigned n = 8 - extra_reg_slots; n < 8; ++n, ++pos) {
                ValueSource src;
                src.mem = xreg(n);
                if (pos < 4) {
                    reg_loads.push_back({pos, src, false, false, true});
                } else {
                    a.mov(64, Gp::rax, src.mem);
                    a.mov(64, ptr(Gp::rsp, static_cast<std::int32_t>(8 * pos)), Gp::rax);
                }
            }
            // Stack tail; positions below 4 (only possible after wide named
            // aggregates) are loaded into registers individually.
            unsigned first = extra_stack_first;
            unsigned count = extra_stack_slots;
            while (count > 0 && pos < 4) {
                ValueSource src;
                src.mem = ptr(kGuestSp, static_cast<std::int32_t>(8 * first));
                reg_loads.push_back({pos, src, false, false, true});
                ++pos, ++first, --count;
            }
            emit_guest_stack_copy(a, static_cast<std::int32_t>(8 * first),
                                  static_cast<std::int32_t>(8 * pos), count);
        }
    }

    // ---- register arguments (last: nothing below clobbers them) -----------------
    if (plan.win_hidden_return) {
        if (plan.ret_kind == ReturnKind::record_memory) a.mov(64, Gp::rcx, xreg(8));
        else a.lea(64, Gp::rcx, ptr(Gp::rsp, ret_temp));
    }
    for (const auto& load : reg_loads) {
        if (load.fp) {
            const auto x = x64::xmm(load.pos);
            if (load.src.kind == ValueSource::address) throw std::logic_error("fp by address");
            if (load.fp32) a.movd(x, load.src.mem);
            else a.movq(x, load.src.mem);
            continue;
        }
        emit_load(a, kWinGpr[load.pos], load.src);
        if (load.duplicate) a.movq(x64::xmm(load.pos), kWinGpr[load.pos]);
    }
    if (unknown) {
        for (unsigned n = 0; n < 4; ++n) a.movq(x64::xmm(n), vreg(n));
    }

    // ---- the call -----------------------------------------------------------------
    out.native_imm = a.mov_imm64(Gp::rax, reinterpret_cast<std::uint64_t>(native));
    a.call(Gp::rax);

    // ---- results ------------------------------------------------------------------
    if (unknown) {
        a.mov(64, xreg(0), Gp::rax);
        a.movq(vreg(0), Xmm::xmm0);
        store_zero_high(a, 0);
    } else {
        const auto& ret = plan.ret;
        switch (plan.ret_kind) {
        case ReturnKind::none:
            break;
        case ReturnKind::integer:
            emit_int_extension(a, ret.kind);
            if (ret.kind == TypeKind::fnptr) {
                a.mov(64, Gp::rcx, Gp::rax);
                a.mov_imm64(Gp::rdx, reinterpret_cast<std::uint64_t>(ret.fn));
                a.mov_imm64(Gp::rax, reinterpret_cast<std::uint64_t>(&unwrap_callback_result));
                a.call(Gp::rax);
            }
            a.mov(64, xreg(0), Gp::rax);
            break;
        case ReturnKind::fp:
            if (ret.kind == TypeKind::f32) {
                a.movd(Gp::rax, Xmm::xmm0);      // zero-extends into rax
                a.mov(64, vreg(0), Gp::rax);
            } else {
                a.movq(vreg(0), Xmm::xmm0);
            }
            store_zero_high(a, 0);
            break;
        case ReturnKind::record_regs:
            if (!plan.win_hidden_return) {
                emit_record_extension(a, ret.size);
                a.mov(64, xreg(0), Gp::rax);
            } else {
                a.mov(64, Gp::rax, ptr(Gp::rsp, ret_temp));
                a.mov(64, xreg(0), Gp::rax);
                if (ret.size > 8) {
                    a.mov(64, Gp::rax, ptr(Gp::rsp, ret_temp + 8));
                    a.mov(64, xreg(1), Gp::rax);
                }
            }
            break;
        case ReturnKind::record_memory:
            break;   // the callee wrote through the guest's x8 buffer
        case ReturnKind::hfa_regs:
            if (!plan.win_hidden_return) {
                // 4- or 8-byte HFA returned in rax: {f1}, {f2} or {d1}.
                if (ret.hfa_double) {
                    a.mov(64, vreg(0), Gp::rax);
                } else {
                    a.mov(64, Gp::rdx, Gp::rax);
                    a.mov(32, Gp::rax, Gp::rax);
                    a.mov(64, vreg(0), Gp::rax);
                    if (ret.hfa_count == 2) {
                        a.shr(64, Gp::rdx, 32);
                        a.mov(64, vreg(1), Gp::rdx);
                        store_zero_high(a, 1);
                    }
                }
                store_zero_high(a, 0);
            } else {
                for (unsigned k = 0; k < ret.hfa_count; ++k) {
                    if (ret.hfa_double) {
                        a.mov(64, Gp::rax, ptr(Gp::rsp, ret_temp + static_cast<std::int32_t>(8 * k)));
                    } else {
                        a.mov(32, Gp::rax, ptr(Gp::rsp, ret_temp + static_cast<std::int32_t>(4 * k)));
                    }
                    a.mov(64, vreg(k), Gp::rax);
                    store_zero_high(a, k);
                }
            }
            break;
        }
    }

    if (frame != 0) a.add(64, Gp::rsp, static_cast<std::int32_t>(frame));
    out.epilog_offset = a.size();     // rsp == entry rsp again from here on
    emit_mirror_last_error(a);
    emit_return_to_guest(a);
    a.finalize();
    out.bytes = a.bytes();
    return out;
}

std::vector<std::uint8_t> generate_override_thunk(OverrideHandler handler, const OverrideBinding* binding,
                                                  TraceInfo* trace) {
    Assembler a(128);
    a.mov(64, kReturn, xreg(30));
    emit_trace_call(a, trace);
    // ctx->pc = return address, so handlers that capture or raise see the
    // caller's state the way the native API would.
    a.mov(64, field(offsetof(GuestContext, pc)), kReturn);
    // FP handoff (src/fx/runtime/fp_boundary.hpp): the handler sees the
    // architectural FPSR in ctx.fpsr; afterwards MXCSR is loaded from
    // ctx.fpcr/ctx.fpsr, so overrides that run the host CRT's fenv functions
    // on MXCSR fold their result back into ctx.fpsr (overrides_crt.cpp).
    // [rsp+8] is callee shadow space, free outside the calls.
    transmute::fx::rt::emit_fp_state_to_context(a, ptr(Gp::rsp, 8));
    a.lea(64, Gp::rcx, ptr(kCtx, -kContextBias));
    a.mov_imm64(Gp::rdx, reinterpret_cast<std::uint64_t>(binding));
    a.mov_imm64(Gp::rax, reinterpret_cast<std::uint64_t>(handler));
    a.call(Gp::rax);      // entry rsp is 16-aligned with callee shadow space
    a.mov(32, Gp::rdi, Gp::rax);
    transmute::fx::rt::emit_fp_state_from_context(a, ptr(Gp::rsp, 8));
    emit_mirror_last_error(a);
    const Label resume = a.new_label();
    a.test(32, Gp::rdi, Gp::rdi);
    a.jcc(Cond::nz, resume);
    emit_return_to_guest(a);
    a.bind(resume);
    a.jmp(field(offsetof(GuestContext, stub_indirect)));
    a.finalize();
    return a.bytes();
}

std::vector<std::uint8_t> generate_callback_trampoline(const CallbackInfo* info, std::uint32_t* prolog_bytes,
                                                       std::uint32_t* frame_bytes) {
    Assembler a(128);
    // Win64 entry: [rsp] = return address, home area at [rsp+8..40).
    a.mov(64, ptr(Gp::rsp, 8), Gp::rcx);
    a.mov(64, ptr(Gp::rsp, 16), Gp::rdx);
    a.mov(64, ptr(Gp::rsp, 24), Gp::r8);
    a.mov(64, ptr(Gp::rsp, 32), Gp::r9);
    constexpr std::int32_t kFrame = 0x58;   // 32 shadow + 32 xmm + 24 pad; keeps rsp 16-aligned
    a.sub(64, Gp::rsp, kFrame);
    if (prolog_bytes) *prolog_bytes = static_cast<std::uint32_t>(a.size());
    if (frame_bytes) *frame_bytes = kFrame;
    for (unsigned n = 0; n < 4; ++n) a.movq(ptr(Gp::rsp, static_cast<std::int32_t>(32 + 8 * n)), x64::xmm(n));
    a.mov_imm64(Gp::rcx, reinterpret_cast<std::uint64_t>(info));
    a.lea(64, Gp::rdx, ptr(Gp::rsp, kFrame + 8));
    a.lea(64, Gp::r8, ptr(Gp::rsp, 32));
    a.mov_imm64(Gp::rax, reinterpret_cast<std::uint64_t>(&callback_dispatch));
    a.call(Gp::rax);
    a.movq(Xmm::xmm0, ptr(Gp::rsp, 32));
    a.add(64, Gp::rsp, kFrame);
    a.ret();
    a.finalize();
    return a.bytes();
}

} // namespace transmute::fx::abi
