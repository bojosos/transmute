#include "fx/translator/emit_system_core_like.hpp"

#include "transmute/fx/context.hpp"

#include <cstddef>

namespace transmute::fx::systest {

using x64::Cond;
using x64::Gp;
using x64::Xmm;
using x64::ptr;

namespace {

constexpr Gp kScratch[] = {Gp::r8, Gp::r9, Gp::r10, Gp::r11, Gp::rcx, Gp::rdx, Gp::rax};
constexpr std::uint32_t kScratchMask = (1u << 8) | (1u << 9) | (1u << 10) | (1u << 11) | (1u << 1) | (1u << 2) | 1u;
constexpr Gp kCtx = Gp::r15;
constexpr unsigned kFlagsSlot = 60;          // saved RFLAGS of the last set_flags
constexpr unsigned kArgSlot = 56;            // helper argument staging (3 slots)

x64::Mem gmem(unsigned g) { return ptr(kCtx, x_disp(g)); }
x64::Mem nzcv_mem() { return ptr(kCtx, ctx_disp(offsetof(GuestContext, nzcv))); }

constexpr Cond kSub[14] = {Cond::e, Cond::ne, Cond::ae, Cond::b, Cond::s, Cond::ns, Cond::o,
                           Cond::no, Cond::a, Cond::be, Cond::ge, Cond::l, Cond::g, Cond::le};

} // namespace

CoreLikeContext::CoreLikeContext(x64::Assembler& assembler, std::uint64_t pc, const CoreLikeOptions& options)
    : as_(assembler), pc_(pc), options_(options) {
    begin_instruction(pc);
}

Gp CoreLikeContext::alloc() {
    for (const auto r : kScratch) {
        if (free_ & (1u << x64::id(r))) {
            free_ &= ~(1u << x64::id(r));
            ++used_;
            if (used_ > max_used_) max_used_ = used_;
            return r;
        }
    }
    throw std::runtime_error("CoreLikeContext: scratch overflow");
}

Xmm CoreLikeContext::alloc_vec() {
    if (next_vec_ >= 16) throw std::runtime_error("CoreLikeContext: xmm overflow");
    return x64::xmm(next_vec_++);
}

Gp CoreLikeContext::read_gpr(unsigned index, bool sp) {
    if (index == 31 && !sp) {
        const Gp t = alloc();
        as_.mov_imm(t, 0);
        return t;
    }
    if (pinned(index)) return pin(index);
    const Gp t = alloc();
    as_.mov(64, t, gmem(index));
    return t;
}

Gp CoreLikeContext::write_gpr(unsigned index, bool sp) {
    if (index == 31 && !sp) return alloc();
    if (pinned(index)) return pin(index);
    if (pending_count_ >= pending_.size()) throw std::runtime_error("CoreLikeContext: too many writes");
    const Gp t = alloc();
    pending_[pending_count_++] = {index, t};
    return t;
}

Gp CoreLikeContext::copy_gpr(unsigned index, bool sp) {
    const Gp t = alloc();
    if (index == 31 && !sp) as_.mov_imm(t, 0);
    else if (pinned(index)) as_.mov(64, t, pin(index));
    else as_.mov(64, t, gmem(index));
    return t;
}

Gp CoreLikeContext::temp_gpr_prefer(Gp preferred) {
    if (!options_.hooks) return Gp::none;
    const auto bit = 1u << x64::id(preferred);
    if (!(kScratchMask & bit) || !(free_ & bit)) return Gp::none;
    free_ &= ~bit;
    ++used_;
    if (used_ > max_used_) max_used_ = used_;
    return preferred;
}

void CoreLikeContext::load_gpr(Gp dst, unsigned index, bool sp) {
    if (!options_.hooks) { EmitContext::load_gpr(dst, index, sp); return; }
    if (index == 31 && !sp) as_.mov_imm(dst, 0);
    else if (pinned(index)) { if (pin(index) != dst) as_.mov(64, dst, pin(index)); }
    else as_.mov(64, dst, gmem(index));
}

void CoreLikeContext::store_gpr(unsigned index, Gp src, bool sp) {
    if (!options_.hooks) { EmitContext::store_gpr(index, src, sp); return; }
    if (index == 31 && !sp) return;
    if (pinned(index)) { if (pin(index) != src) as_.mov(64, pin(index), src); }
    else as_.mov(64, gmem(index), src);
}

Xmm CoreLikeContext::read_vec(unsigned index) {
    const Xmm x = alloc_vec();
    as_.movdqu(x, ptr(kCtx, v_disp(index)));
    return x;
}

Xmm CoreLikeContext::write_vec(unsigned index) {
    const Xmm x = alloc_vec();
    pending_vec_[pending_vec_count_++] = {index, x};
    return x;
}

void CoreLikeContext::set_flags(FlagsProducer producer) {
    // pushfq; pop qword [r15 + spill]: no GPR needed now; combined at the end.
    as_.pushfq();
    as_.emit_rm(0, false, {0x8f}, 0, ptr(kCtx, spill_disp(kFlagsSlot)));
    flags_pending_ = true;
    producer_ = producer;
}

void CoreLikeContext::combine_flags(Gp t, Gp u) {
    const auto saved = ptr(kCtx, spill_disp(kFlagsSlot));
    as_.mov(32, t, saved);
    as_.and_(32, t, 0xc0);              // SF ZF
    as_.shl(32, t, 24);
    if (producer_ != FlagsProducer::logic) {
        as_.mov(32, u, saved);
        as_.and_(32, u, 1);             // CF
        if (producer_ == FlagsProducer::sub) as_.xor_(32, u, 1);
        as_.shl(32, u, 29);
        as_.or_(32, t, u);
        as_.mov(32, u, saved);
        as_.and_(32, u, 0x800);         // OF
        as_.shl(32, u, 17);
        as_.or_(32, t, u);
    }
    as_.mov(64, nzcv_mem(), t);
    flags_pending_ = false;
}

void CoreLikeContext::set_nzcv(Gp value) {
    as_.mov(64, nzcv_mem(), value);
    flags_pending_ = false;
}

Gp CoreLikeContext::get_nzcv() {
    if (flags_pending_) {
        const Gp t = alloc(), u = alloc();
        combine_flags(t, u);
    }
    const Gp t = alloc();
    as_.mov(32, t, nzcv_mem());
    return t;
}

Cond CoreLikeContext::condition(unsigned arm_condition) {
    if (flags_pending_) {
        const Gp t = alloc(), u = alloc();
        combine_flags(t, u);
    }
    const Gp nzcv = alloc(), image = alloc(), bit = alloc();
    as_.mov(32, nzcv, nzcv_mem());
    as_.mov_imm(image, 2);
    as_.mov(32, bit, nzcv);
    as_.shr(32, bit, 24);
    as_.and_(32, bit, 0xc0);
    as_.or_(32, image, bit);
    as_.mov(32, bit, nzcv);
    as_.shr(32, bit, 29);
    as_.and_(32, bit, 1);
    as_.xor_(32, bit, 1);
    as_.or_(32, image, bit);
    as_.mov(32, bit, nzcv);
    as_.shr(32, bit, 17);
    as_.and_(32, bit, 0x800);
    as_.or_(32, image, bit);
    as_.push(image);
    as_.popfq();
    return kSub[arm_condition];
}

Gp CoreLikeContext::call_helper(unsigned id, std::initializer_list<HelperArg> args, HelperEffects effects) {
    if (pending_count_ != 0 || pending_vec_count_ != 0) throw std::logic_error("call_helper after writes");
    unsigned slot = kArgSlot;
    for (const auto& arg : args) {
        if (arg.kind == HelperArg::Kind::host) as_.mov(64, ptr(kCtx, spill_disp(slot)), arg.reg);
        ++slot;
    }
    // Host arguments are staged; r10/r11 are volatile across the call anyway.
    if (flags_pending_) combine_flags(Gp::r10, Gp::r11);
    const Gp regs[] = {Gp::rdx, Gp::r8, Gp::r9};
    unsigned i = 0;
    for (const auto& arg : args) {
        const Gp r = regs[i];
        switch (arg.kind) {
        case HelperArg::Kind::host: as_.mov(64, r, ptr(kCtx, spill_disp(kArgSlot + i))); break;
        case HelperArg::Kind::immediate: as_.mov_imm(r, arg.value); break;
        case HelperArg::Kind::guest_gpr: {
            const auto g = static_cast<unsigned>(arg.value);
            if (g == 31) as_.mov_imm(r, 0);
            else if (pinned(g)) as_.mov(64, r, pin(g));
            else as_.mov(64, r, gmem(g));
            break;
        }
        }
        ++i;
    }
    if (effects != HelperEffects::pure) flush_pins();
    as_.lea(64, Gp::rcx, ptr(kCtx, -kContextBias));
    as_.call(ptr(kCtx, helper_disp(id)));
    if (effects == HelperEffects::reads_writes_state) load_pins();
    free_ = kScratchMask & ~1u;   // everything but rax is free again
    used_ = 1;
    next_vec_ = 0;
    return Gp::rax;
}

void CoreLikeContext::begin_instruction(std::uint64_t pc) {
    pc_ = pc;
    free_ = kScratchMask;
    used_ = 0;
    next_vec_ = 0;
    pending_count_ = 0;
    pending_vec_count_ = 0;
    flags_pending_ = false;
}

void CoreLikeContext::end_instruction() {
    for (unsigned i = 0; i < pending_count_; ++i) as_.mov(64, gmem(pending_[i].guest), pending_[i].host);
    for (unsigned i = 0; i < pending_vec_count_; ++i)
        as_.movdqu(ptr(kCtx, v_disp(pending_vec_[i].guest)), pending_vec_[i].host);
    // All scratch is dead now; rax/rcx are free to combine flags.
    if (flags_pending_) combine_flags(Gp::rax, Gp::rcx);
    pending_count_ = 0;
    pending_vec_count_ = 0;
}

void CoreLikeContext::load_pins() {
    for (unsigned g = 0; g < 32; ++g) if (pinned(g)) as_.mov(64, pin(g), gmem(g));
}

void CoreLikeContext::flush_pins() {
    for (unsigned g = 0; g < 32; ++g) if (pinned(g)) as_.mov(64, gmem(g), pin(g));
}

} // namespace transmute::fx::systest
