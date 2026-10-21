// Atomics: ordered loads/stores, exclusives (LDXR/STXR/LDXP/STXP), LL/SC loop
// fusion and LSE atomics (owner: atomics/system). See emit_system.hpp and
// NOTES_SYSTEM.md for the design.
#include "fx/translator/emit_system_internal.hpp"

#include <array>
#include <cstddef>
#include <optional>

namespace transmute::fx::system_detail {
namespace {

using x64::Alu;
using x64::Assembler;
using x64::Cond;
using x64::Label;
using x64::ptr;

inline std::int32_t sign_extend(std::uint32_t value, unsigned bits) {
    const std::uint32_t m = 1u << (bits - 1);
    return static_cast<std::int32_t>((value ^ m) - m);
}

// ---- decoding -------------------------------------------------------------------

struct Excl {
    bool pair{};
    bool load{};
    bool ordered{};      // acquire (loads) / release (stores)
    unsigned bytes{};    // single: element size; pair: total size (8 or 16)
    unsigned rt{}, rt2{}, rs{}, rn{};
};

bool decode_exclusive(std::uint32_t w, Excl& e) {
    if ((w & 0x3f000000u) != 0x08000000u) return false;
    if (w & (1u << 23)) return false;                      // o2: ordered / CAS
    const bool o1 = (w & (1u << 21)) != 0;
    e.load = (w & (1u << 22)) != 0;
    e.ordered = (w & (1u << 15)) != 0;
    e.rs = rm_of(w);
    e.rt2 = (w >> 10) & 31u;
    e.rn = rn_of(w);
    e.rt = rd_of(w);
    const unsigned size = w >> 30;
    // Rs of loads and Rt2 of single-register forms are should-be-one fields:
    // other values are CONSTRAINED UNPREDICTABLE and may execute as the
    // canonical encoding, which is what this does.
    if (e.load) e.rs = 31;
    if (!o1) {
        e.rt2 = 31;
        e.pair = false;
        e.bytes = 1u << size;
        return true;
    }
    if (size < 2) return false;                            // CASP space
    e.pair = true;
    e.bytes = size == 3 ? 16 : 8;
    return true;
}

bool is_cbnz(std::uint32_t w, unsigned reg, std::int32_t offset_words, bool* is64 = nullptr) {
    if ((w & 0x7f000000u) != 0x35000000u) return false;
    if ((w & 31u) != reg) return false;
    if (sign_extend((w >> 5) & 0x7ffffu, 19) != offset_words) return false;
    if (is64) *is64 = (w & 0x80000000u) != 0;
    return true;
}

bool is_barrier(std::uint32_t w) {   // DMB/DSB (any domain/type)
    return (w & 0xfffff0dfu) == 0xd503309fu;
}

bool is_mov_reg(std::uint32_t w, unsigned& rd, unsigned& rm, bool& is64) {
    if ((w & 0x7fe0ffe0u) != 0x2a0003e0u) return false;
    rd = rd_of(w);
    rm = rm_of(w);
    is64 = (w & 0x80000000u) != 0;
    return rd != 31;
}

// Second operand of an ALU/compare instruction, independent of the loaded value.
struct Operand2 {
    enum class Kind : std::uint8_t { imm, shifted, extended } kind{Kind::imm};
    std::uint64_t value{};     // imm
    unsigned reg{31};          // shifted / extended
    unsigned type{};           // shift type (LSL/LSR/ASR/ROR) or extend option
    unsigned amount{};
    bool invert{};
};

enum class AluOp : std::uint8_t { add, sub, rsub, and_, orr, eor };

struct AluDecode {
    AluOp op{};
    bool is64{};
    bool flags{};        // ADDS/SUBS (extended fusion only)
    unsigned rd{};
    Operand2 other;
};

// ALU instruction that computes Rd from the loaded value `old` (register rt)
// and an operand that does not depend on it. Flag-setting forms only for
// ADDS/SUBS (rt as first operand, or either operand of ADDS) and only when
// extended fusion is on.
bool decode_alu(std::uint32_t w, unsigned rt, AluDecode& d) {
    d.is64 = (w & 0x80000000u) != 0;
    d.rd = rd_of(w);
    const unsigned rn = rn_of(w), rm = rm_of(w);
    const unsigned width = d.is64 ? 64 : 32;
    const bool arith = (w & 0x1f800000u) == 0x11000000u || (w & 0x1f200000u) == 0x0b000000u;
    d.flags = arith && (w & 0x20000000u) != 0;   // (bit 29 is part of opc for logical ops)
    if (d.flags && !extended_fusion()) return false;
    if ((w & 0x1f800000u) == 0x11000000u) {   // ADD/SUB(S) imm
        if (rn != rt || d.rd == 31) return false;
        d.op = (w & 0x40000000u) ? AluOp::sub : AluOp::add;
        d.other.kind = Operand2::Kind::imm;
        d.other.value = static_cast<std::uint64_t>((w >> 10) & 0xfffu) << ((w & (1u << 22)) ? 12 : 0);
        return true;
    }
    if ((w & 0x1f200000u) == 0x0b000000u) {   // ADD/SUB(S) shifted
        const unsigned type = (w >> 22) & 3u, amount = (w >> 10) & 63u;
        if (type == 3 || amount >= width || d.rd == 31) return false;
        const bool sub = (w & 0x40000000u) != 0;
        if (rn == rt && rm != rt) {
            d.op = sub ? AluOp::sub : AluOp::add;
            d.other = {Operand2::Kind::shifted, 0, rm, type, amount, false};
            return true;
        }
        if (rm == rt && rn != rt && amount == 0) {
            if (sub && d.flags) return false;
            d.op = sub ? AluOp::rsub : AluOp::add;
            d.other = {Operand2::Kind::shifted, 0, rn, 0, 0, false};
            return true;
        }
        return false;
    }
    if ((w & 0x7f000000u) == 0x0a000000u || (w & 0x7f000000u) == 0x2a000000u ||
        (w & 0x7f000000u) == 0x4a000000u) {                                        // AND/ORR/EOR (+N)
        const unsigned opc = (w >> 29) & 3u;
        const unsigned type = (w >> 22) & 3u, amount = (w >> 10) & 63u;
        const bool invert = (w & (1u << 21)) != 0;
        if (amount >= width || d.rd == 31) return false;
        d.op = opc == 0 ? AluOp::and_ : opc == 1 ? AluOp::orr : AluOp::eor;
        if (rn == rt && rm != rt) {
            d.other = {Operand2::Kind::shifted, 0, rm, type, amount, invert};
            return true;
        }
        if (rm == rt && rn != rt && amount == 0 && !invert) {
            d.other = {Operand2::Kind::shifted, 0, rn, 0, 0, false};
            return true;
        }
        return false;
    }
    if ((w & 0x7f800000u) == 0x12000000u || (w & 0x7f800000u) == 0x32000000u ||
        (w & 0x7f800000u) == 0x52000000u) {                                        // AND/ORR/EOR imm
        if (rn != rt || d.rd == 31) return false;
        const unsigned opc = (w >> 29) & 3u;
        const unsigned n = (w >> 22) & 1u, immr = (w >> 16) & 63u, imms = (w >> 10) & 63u;
        std::uint64_t value = 0;
        if (!decode_bit_mask(n, imms, immr, d.is64, value)) return false;
        d.op = opc == 0 ? AluOp::and_ : opc == 1 ? AluOp::orr : AluOp::eor;
        d.other.kind = Operand2::Kind::imm;
        d.other.value = value;
        return true;
    }
    return false;
}

struct CmpDecode {
    bool is64{};
    Operand2 operand;
};

// CMP Rt, <operand> (SUBS ZR, Rt, ...): immediate, shifted or extended register.
bool decode_cmp(std::uint32_t w, unsigned rt, CmpDecode& c) {
    c.is64 = (w & 0x80000000u) != 0;
    if (rd_of(w) != 31 || rn_of(w) != rt) return false;
    const unsigned width = c.is64 ? 64 : 32;
    if ((w & 0x7f800000u) == 0x71000000u) {
        c.operand.kind = Operand2::Kind::imm;
        c.operand.value = static_cast<std::uint64_t>((w >> 10) & 0xfffu) << ((w & (1u << 22)) ? 12 : 0);
        return true;
    }
    if ((w & 0x7f200000u) == 0x6b000000u) {
        const unsigned type = (w >> 22) & 3u, amount = (w >> 10) & 63u;
        if (type == 3 || amount >= width || rm_of(w) == rt) return false;
        c.operand = {Operand2::Kind::shifted, 0, rm_of(w), type, amount, false};
        return true;
    }
    if ((w & 0x7fe00000u) == 0x6b200000u) {
        const unsigned option = (w >> 13) & 7u, amount = (w >> 10) & 7u;
        if (amount > 4 || rm_of(w) == rt) return false;
        c.operand = {Operand2::Kind::extended, 0, rm_of(w), option, amount, false};
        return true;
    }
    return false;
}

// ---- emission helpers -------------------------------------------------------------
//
// Register budget: the comments "worst N" count scratch GPRs when nothing is
// pinned and the EmitContext hooks are not overridden (read/copy/temp/write
// and load_gpr/store_gpr each cost one). Every path stays within kBudget, the
// core RegionEmitter's scratch pool, except CASP without hooks (see notes).

constexpr unsigned kBudget = 7;

unsigned bits_of(unsigned bytes) { return bytes * 8u; }
// x64 operand width for a value of `bytes` held zero-extended in a register.
unsigned reg_width(unsigned bytes) { return bytes == 8 ? 64 : 32; }

Mem mem_at(Gp base, std::int32_t disp = 0) { return ptr(base, disp); }

void probe_write(EmitContext& ctx, unsigned bytes, const Mem& m) {
    ctx.mark_may_fault();
    ctx.as().lock();
    ctx.as().alu(Alu::or_, bits_of(bytes > 8 ? 8 : bytes), m, 0);
}

// Register holding `op` (width bits), owned by the caller: a copy of the
// operand register transformed in place, or a temp with the immediate.
Gp materialize(EmitContext& ctx, const Operand2& op, unsigned width) {
    auto& a = ctx.as();
    if (op.kind == Operand2::Kind::imm) {
        const Gp t = ctx.temp_gpr();
        const std::uint64_t v = width == 64 ? op.value : (op.value & 0xffffffffULL);
        a.mov_imm(t, op.invert ? (width == 64 ? ~v : (~v & 0xffffffffULL)) : v);
        return t;
    }
    const Gp t = ctx.copy_gpr(op.reg);
    if (op.kind == Operand2::Kind::extended) {
        switch (op.type) {
        case 0: a.movzx(32, t, t, 8); break;
        case 1: a.movzx(32, t, t, 16); break;
        case 2: a.mov(32, t, t); break;
        case 4: a.movsx(width, t, t, 8); break;
        case 5: a.movsx(width, t, t, 16); break;
        case 6: if (width == 64) a.movsx(64, t, t, 32); break;
        default: break;
        }
        if (op.amount) a.shl(width, t, static_cast<std::uint8_t>(op.amount));
    } else if (op.amount) {
        static constexpr x64::Shift kinds[4] = {x64::Shift::shl, x64::Shift::shr, x64::Shift::sar,
                                               x64::Shift::ror};
        a.shift(kinds[op.type], width, t, static_cast<std::uint8_t>(op.amount));
    }
    if (op.invert) a.not_(width, t);
    if (width == 32) a.mov(32, t, t);
    return t;
}

void clear_reservation(Assembler& a) { a.mov(32, excl_size_mem(), 0); }

// Fused loops end with no reservation. Usually none is outstanding, and a
// load+branch is cheaper than a store that the next locked instruction would
// have to drain from the store buffer. Clobbers EFLAGS.
void clear_reservation_if_set(Assembler& a) {
    const Label skip = a.new_label();
    a.cmp(32, excl_size_mem(), 0);
    a.jcc_short(Cond::e, skip);
    a.mov(32, excl_size_mem(), 0);
    a.bind(skip);
}

void record_reservation(Assembler& a, Gp address, Gp value, unsigned bytes) {
    a.mov(64, excl_address_mem(), address);
    a.mov(64, excl_value_mem(0), value);
    a.mov(32, excl_size_mem(), static_cast<std::int32_t>(bytes));
}

void store_zero(EmitContext& ctx, unsigned guest) {
    if (guest == 31) return;
    const Gp d = ctx.write_gpr(guest);
    ctx.as().mov_imm(d, 0);
}

// ---- values written by a fused span ----------------------------------------------

class Finals {
public:
    struct Value {
        enum class K : std::uint8_t { none, reg, zero } k{K::none};
        Gp r{Gp::none};
        bool fits32{};   // value is zero-extended from 32 bits
    };
    void set_reg(unsigned g, Gp r, bool fits32) { put(g, {Value::K::reg, r, fits32}); }
    void set_zero(unsigned g) { put(g, {Value::K::zero, Gp::none, true}); }
    [[nodiscard]] const Value& get(unsigned g) const { return values_[g]; }
    [[nodiscard]] bool written(unsigned g) const { return g != 31 && values_[g].k != Value::K::none; }
    [[nodiscard]] unsigned count() const { return count_; }
    // Stores every final value (sources are temps, so order is irrelevant).
    void emit(EmitContext& ctx) const {
        for (unsigned i = 0; i < count_; ++i) {
            const auto g = order_[i];
            const auto& v = values_[g];
            if (v.k == Value::K::zero) store_zero(ctx, g);
            else ctx.store_gpr(g, v.r);
        }
    }

private:
    void put(unsigned g, Value v) {
        if (g == 31) return;
        if (values_[g].k == Value::K::none) order_[count_++] = static_cast<std::uint8_t>(g);
        values_[g] = v;
    }
    std::array<Value, 32> values_{};
    std::array<std::uint8_t, 32> order_{};
    unsigned count_{};
};

// Register-only instructions between the loop's CBNZ (or an LSE op) and a
// consumed barrier: MOV (ORR from ZR); with extended fusion also CMP/CMN
// #imm, CMP Rn, Rm, AND #imm and ADD/SUB #imm (non-flag). They read only the
// span's final values or registers the span does not write, and the barrier
// makes them part of the span.
struct TrailOp {
    enum class K : std::uint8_t { mov, alu_imm, cmp_imm, cmn_imm, cmp_reg } k{K::mov};
    unsigned rd{31}, rn{31}, rm{31};
    bool is64{};
    std::uint64_t imm{};
    x64::Alu alu{x64::Alu::and_};   // alu_imm: and_ / add / sub
    [[nodiscard]] bool sets_flags() const { return k == K::cmp_imm || k == K::cmn_imm || k == K::cmp_reg; }
    [[nodiscard]] bool writes() const { return k == K::mov || k == K::alu_imm; }
};

bool decode_trail_op(std::uint32_t w, TrailOp& op) {
    op = {};
    if (is_mov_reg(w, op.rd, op.rm, op.is64)) { op.k = TrailOp::K::mov; return true; }
    if (!extended_fusion()) return false;
    op.is64 = (w & 0x80000000u) != 0;
    op.rd = rd_of(w);
    op.rn = rn_of(w);
    if ((w & 0x7f80001fu) == 0x7100001fu || (w & 0x7f80001fu) == 0x3100001fu) {   // CMP/CMN #imm
        if (op.rn == 31) return false;                                             // (SP)
        op.k = (w & 0x40000000u) ? TrailOp::K::cmp_imm : TrailOp::K::cmn_imm;
        op.imm = static_cast<std::uint64_t>((w >> 10) & 0xfffu) << ((w & (1u << 22)) ? 12 : 0);
        op.rd = 31;
        return true;
    }
    if ((w & 0x7fe0fc1fu) == 0x6b00001fu) {                                        // CMP Rn, Rm (LSL #0)
        op.rm = rm_of(w);
        if (op.rn == 31 || op.rm == 31) return false;
        op.k = TrailOp::K::cmp_reg;
        op.rd = 31;
        return true;
    }
    if ((w & 0x7f800000u) == 0x12000000u) {                                        // AND #imm
        if (op.rd == 31 || op.rn == 31) return false;                              // (WSP/SP, ZR)
        std::uint64_t value = 0;
        if (!decode_bit_mask((w >> 22) & 1u, (w >> 10) & 63u, (w >> 16) & 63u, op.is64, value)) return false;
        if (op.is64 && static_cast<std::int64_t>(value) != static_cast<std::int32_t>(value)) return false;
        op.k = TrailOp::K::alu_imm;
        op.imm = value;
        return true;
    }
    if ((w & 0x3f800000u) == 0x11000000u) {                                        // ADD/SUB #imm (non-flag)
        if (op.rd == 31 || op.rn == 31) return false;                              // (SP)
        op.k = TrailOp::K::alu_imm;
        op.alu = (w & 0x40000000u) ? x64::Alu::sub : x64::Alu::add;
        op.imm = static_cast<std::uint64_t>((w >> 10) & 0xfffu) << ((w & (1u << 22)) ? 12 : 0);
        return true;
    }
    return false;
}

struct Trailing {
    unsigned count{};
    std::array<TrailOp, 3> ops{};
    unsigned consumed{};   // words after the loop (ops + barrier), 0 = none
};

Trailing scan_trailing(std::span<const std::uint32_t> words, unsigned loop_end) {
    Trailing t;
    const unsigned max_ops = extended_fusion() ? 3 : 2;
    for (unsigned k = 0; k <= max_ops && loop_end + k < words.size(); ++k) {
        const auto w = words[loop_end + k];
        if (is_barrier(w)) {
            t.count = k;
            t.consumed = k + 1;
            return t;
        }
        if (k == max_ops || !decode_trail_op(w, t.ops[k])) break;
    }
    return Trailing{};
}

// Guest-register knowledge while walking the trailing ops at fit time.
struct TrailState {
    std::array<bool, 32> written{}, fits{}, zero{};
};

constexpr unsigned kUnsupported = kBudget + 1;

// Worst-case scratch cost of the trailing ops given the span's writes, or
// kUnsupported if an op cannot be fused in this position.
unsigned trailing_cost(const Trailing& t, TrailState s, bool body_flags) {
    unsigned cost = 0;
    const auto source = [&](unsigned g) {          // read-only operand
        if (s.written[g]) return s.zero[g] ? kUnsupported : 0u;   // zero finals: not worth a register
        return 1u;
    };
    for (unsigned k = 0; k < t.count; ++k) {
        const auto& op = t.ops[k];
        switch (op.k) {
        case TrailOp::K::mov:
            if (!s.written[op.rd]) cost += 1;                                  // destination register
            if (op.rm != 31 && !s.written[op.rm]) cost += 1;                   // copy of an unwritten source
            else if (op.rm != 31 && !op.is64 && !s.fits[op.rm]) cost += 1;     // zero-extending copy
            s.fits[op.rd] = !op.is64 || op.rm == 31 || s.fits[op.rm];
            s.zero[op.rd] = op.rm == 31 || (s.written[op.rm] && s.zero[op.rm]);
            break;
        case TrailOp::K::alu_imm: {
            // x64 AND/ADD/SUB clobber EFLAGS: after a flag-setting body only
            // if a later trailing op sets the flags again.
            bool later = false;
            for (unsigned j = k + 1; j < t.count; ++j) later = later || t.ops[j].sets_flags();
            if (body_flags && !later) return kUnsupported;
            if (!s.written[op.rd]) cost += 1;
            cost += 1;                                                          // result (copy or temp)
            s.fits[op.rd] = !op.is64;
            s.zero[op.rd] = op.alu == x64::Alu::and_ && s.written[op.rn] && s.zero[op.rn];
            break;
        }
        case TrailOp::K::cmp_imm: cost += source(op.rn); break;
        case TrailOp::K::cmn_imm: cost += source(op.rn) + 1; break;            // + add temp
        case TrailOp::K::cmp_reg: cost += source(op.rn) + source(op.rm); break;
        }
        if (cost >= kUnsupported) return kUnsupported;
        if (op.writes()) s.written[op.rd] = true;
    }
    return cost;
}

// Drops the trailing ops and barrier if they would exceed the budget.
// `body_flags`: the span itself sets NZCV (ADDS/SUBS bodies).
Trailing fit_trailing(Trailing t, unsigned base_cost, std::initializer_list<unsigned> written_regs,
                      std::initializer_list<unsigned> fits_regs, std::initializer_list<unsigned> zero_regs,
                      bool body_flags = false) {
    TrailState s;
    for (auto g : written_regs) if (g < 31) s.written[g] = true;
    for (auto g : fits_regs) if (g < 31) s.fits[g] = true;
    for (auto g : zero_regs) if (g < 31) s.zero[g] = true;
    if (base_cost + trailing_cost(t, s, body_flags) > kBudget) return Trailing{};
    return t;
}

// Registers for unwritten trailing sources and temps, taken before any guest
// write and before the span's internal labels.
struct TrailRegs {
    std::array<Gp, 3> src{Gp::none, Gp::none, Gp::none};   // mov/and: copy; cmp*: Rn read
    std::array<Gp, 3> rhs{Gp::none, Gp::none, Gp::none};   // cmp reg: Rm read
    std::array<Gp, 3> tmp{Gp::none, Gp::none, Gp::none};   // cmn: add temp; and: result temp
};

TrailRegs copy_trailing_sources(EmitContext& ctx, const Trailing& t, const Finals& finals) {
    TrailRegs r;
    std::array<bool, 32> w{};
    for (unsigned g = 0; g < 31; ++g) w[g] = finals.written(g);
    for (unsigned k = 0; k < t.count; ++k) {
        const auto& op = t.ops[k];
        switch (op.k) {
        case TrailOp::K::mov:
            if (op.rm != 31 && !w[op.rm]) r.src[k] = ctx.copy_gpr(op.rm);
            break;
        case TrailOp::K::alu_imm:
            if (!w[op.rn]) r.src[k] = ctx.copy_gpr(op.rn);
            else r.tmp[k] = ctx.temp_gpr();
            break;
        case TrailOp::K::cmp_imm:
            if (!w[op.rn]) r.src[k] = ctx.read_gpr(op.rn);
            break;
        case TrailOp::K::cmn_imm:
            if (!w[op.rn]) r.src[k] = ctx.read_gpr(op.rn);
            r.tmp[k] = ctx.temp_gpr();
            break;
        case TrailOp::K::cmp_reg:
            if (!w[op.rn]) r.src[k] = ctx.read_gpr(op.rn);
            if (!w[op.rm]) r.rhs[k] = op.rm == op.rn && r.src[k] != Gp::none ? r.src[k] : ctx.read_gpr(op.rm);
            break;
        }
        if (op.writes()) w[op.rd] = true;
    }
    return r;
}

// The last NZCV writer of a span, emitted as its final x64 instruction.
struct FlagsOp {
    enum class K : std::uint8_t { none, cmp_imm, cmp_reg, cmn_imm } k{K::none};
    unsigned width{};
    Gp lhs{Gp::none}, rhs{Gp::none}, tmp{Gp::none};
    std::int32_t imm{};
};

void emit_flags_op(EmitContext& ctx, const FlagsOp& f) {
    auto& a = ctx.as();
    switch (f.k) {
    case FlagsOp::K::none: return;
    case FlagsOp::K::cmp_imm: a.cmp(f.width, f.lhs, f.imm); ctx.set_flags(FlagsProducer::sub); return;
    case FlagsOp::K::cmp_reg: a.cmp(f.width, f.lhs, f.rhs); ctx.set_flags(FlagsProducer::sub); return;
    case FlagsOp::K::cmn_imm:
        a.mov(64, f.tmp, f.lhs);
        a.add(f.width, f.tmp, f.imm);
        ctx.set_flags(FlagsProducer::add);
        return;
    }
}

// Applies the trailing ops to `finals` in program order. Returns the last
// flag-setting op (none if the trailing sets no flags); the caller emits it
// after everything else (x64 AND clobbers EFLAGS).
FlagsOp apply_trailing(EmitContext& ctx, const Trailing& t, Finals& finals, const TrailRegs& regs) {
    auto& a = ctx.as();
    FlagsOp flags;
    // Register holding guest `g`'s current value for a read-only use (zero
    // finals are excluded at fit time).
    const auto value_of = [&](unsigned g, Gp unwritten) {
        const auto& v = finals.get(g);
        return v.k == Finals::Value::K::reg ? v.r : unwritten;
    };
    for (unsigned k = 0; k < t.count; ++k) {
        const auto& op = t.ops[k];
        const unsigned width = op.is64 ? 64 : 32;
        switch (op.k) {
        case TrailOp::K::mov: {
            if (op.rm == 31) { finals.set_zero(op.rd); break; }
            const auto& src = finals.get(op.rm);
            if (src.k == Finals::Value::K::zero) { finals.set_zero(op.rd); break; }
            if (src.k == Finals::Value::K::reg) {
                if (op.is64 || src.fits32) {
                    finals.set_reg(op.rd, src.r, op.is64 ? src.fits32 : true);
                } else {
                    const Gp n = ctx.temp_gpr();
                    a.mov(32, n, src.r);
                    finals.set_reg(op.rd, n, true);
                }
                break;
            }
            if (!op.is64) a.mov(32, regs.src[k], regs.src[k]);
            finals.set_reg(op.rd, regs.src[k], !op.is64);
            break;
        }
        case TrailOp::K::alu_imm: {
            const auto src = finals.get(op.rn);
            if (src.k == Finals::Value::K::zero) {
                if (op.alu == x64::Alu::and_) { finals.set_zero(op.rd); break; }
                std::uint64_t v = op.alu == x64::Alu::add ? op.imm : std::uint64_t{0} - op.imm;
                if (!op.is64) v &= 0xffffffffu;
                a.mov_imm(regs.tmp[k], v);
                finals.set_reg(op.rd, regs.tmp[k], !op.is64);
                break;
            }
            Gp r = regs.src[k];
            if (src.k == Finals::Value::K::reg) {
                r = regs.tmp[k];
                a.mov(64, r, src.r);
            }
            a.alu(op.alu, width, r, static_cast<std::int32_t>(op.imm));
            finals.set_reg(op.rd, r, !op.is64);
            break;
        }
        case TrailOp::K::cmp_imm:
            flags = {FlagsOp::K::cmp_imm, width, value_of(op.rn, regs.src[k]), Gp::none, Gp::none,
                     static_cast<std::int32_t>(op.imm)};
            break;
        case TrailOp::K::cmn_imm:
            flags = {FlagsOp::K::cmn_imm, width, value_of(op.rn, regs.src[k]), Gp::none, regs.tmp[k],
                     static_cast<std::int32_t>(op.imm)};
            break;
        case TrailOp::K::cmp_reg:
            flags = {FlagsOp::K::cmp_reg, width, value_of(op.rn, regs.src[k]), value_of(op.rm, regs.rhs[k]),
                     Gp::none, 0};
            break;
        }
    }
    return flags;
}

// ---- unfused single instructions ------------------------------------------------

// worst 3 (single) / 5 (pairs)
unsigned emit_load_exclusive(EmitContext& ctx, const Excl& e) {
    auto& a = ctx.as();
    const Gp address = ctx.read_gpr(e.rn, true);
    const Gp value = ctx.temp_gpr();
    const Gp high = e.pair ? ctx.temp_gpr() : Gp::none;
    if (e.ordered) acquire_fence(ctx);   // LDAXR/LDAXP: RCsc acquire after an STLR
    if (!e.pair) {
        ctx.mark_may_fault();
        load_zx(a, e.bytes, value, mem_at(address));
        record_reservation(a, address, value, e.bytes);
        ctx.store_gpr(e.rt, value);
        return 1;
    }
    if (e.bytes == 8) {
        ctx.mark_may_fault();
        a.mov(64, value, mem_at(address));
        record_reservation(a, address, value, 8);
        ctx.clobber_host_flags();
        a.mov(64, high, value);
        a.shr(64, high, 32);
        a.mov(32, value, value);
    } else {
        ctx.mark_may_fault();
        a.mov(64, value, mem_at(address));
        ctx.mark_may_fault();
        a.mov(64, high, mem_at(address, 8));
        a.mov(64, excl_address_mem(), address);
        a.mov(64, excl_value_mem(0), value);
        a.mov(64, excl_value_mem(1), high);
        a.mov(32, excl_size_mem(), 16);
    }
    ctx.store_gpr(e.rt, value);
    ctx.store_gpr(e.rt2, high);
    return 1;
}

// single worst 5, W pair worst 7, X pair worst 6
unsigned emit_store_exclusive(EmitContext& ctx, const Excl& e) {
    auto& a = ctx.as();
    const bool wide_pair = e.pair && e.bytes == 16;
    FixedGprs fx = wide_pair ? FixedGprs(ctx, {Gp::rax, Gp::rbx, Gp::rcx, Gp::rdx}) : FixedGprs(ctx, {Gp::rax});
    const Gp status = fx.temp();
    Gp packed = e.pair && !wide_pair ? fx.temp() : Gp::none;
    Gp address = ctx.read_gpr(e.rn, true);
    Gp value = Gp::none, high = Gp::none;
    if (!wide_pair || !fx.hooks()) {
        value = ctx.read_gpr(e.rt);
        if (e.pair) high = ctx.read_gpr(e.rt2);
    }
    ctx.clobber_host_flags();
    const unsigned bytes = wide_pair ? 16 : e.bytes;
    const Label fail = a.new_label(), done = a.new_label();
    a.cmp(64, address, excl_address_mem());
    a.jcc(Cond::ne, fail);
    a.cmp(32, excl_size_mem(), static_cast<std::int32_t>(bytes));
    a.jcc(Cond::ne, fail);
    clear_reservation(a);
    if (fx.clobbers_live()) probe_write(ctx, bytes, mem_at(address));
    Gp a2 = address, v2 = value, h2 = high;
    fx.save({&a2, &v2, &h2});
    a.mov_imm(status, 0);
    if (wide_pair) {
        if (fx.hooks()) {
            ctx.load_gpr(Gp::rcx, e.rt2);
            ctx.load_gpr(Gp::rbx, e.rt);   // last: rbx may hold a pinned source
        } else {
            a.mov(64, Gp::rbx, v2);
            a.mov(64, Gp::rcx, h2);
        }
        a.mov(64, Gp::rax, excl_value_mem(0));
        a.mov(64, Gp::rdx, excl_value_mem(1));
        ctx.mark_may_fault();
        a.lock();
        a.cmpxchg16b(mem_at(a2));
    } else {
        if (e.pair) {   // 2 x 32-bit -> one 64-bit value
            a.mov(64, packed, h2);
            a.shl(64, packed, 32);
            a.mov(32, Gp::rax, v2);
            a.or_(64, packed, Gp::rax);
            v2 = packed;
        }
        a.mov(reg_width(bytes), Gp::rax, excl_value_mem(0));
        ctx.mark_may_fault();
        a.lock();
        a.cmpxchg(bits_of(bytes), mem_at(a2), v2);
    }
    a.setcc(Cond::ne, status);
    fx.restore();
    a.jmp(done);
    a.bind(fail);
    clear_reservation(a);
    a.mov_imm(status, 1);
    a.bind(done);
    ctx.store_gpr(e.rs, status);
    return 1;
}

// ---- fused LL/SC loops -------------------------------------------------------------

// ldxr; <alu>; stxr; cbnz [; mov]* [; dmb]
unsigned fuse_alu(EmitContext& ctx, const Excl& ld, const AluDecode& alu, const Excl& st, Trailing tr) {
    auto& a = ctx.as();
    const unsigned bytes = ld.bytes;
    const unsigned width = alu.is64 ? 64 : 32;
    const bool xadd = alu.op == AluOp::add || alu.op == AluOp::sub;
    const bool need_old = alu.rd != ld.rt;
    const unsigned writes = 2 + (need_old ? 1 : 0);
    // xadd: operand, old, address. loop: 4 either way - claimed rax (which
    // then holds `old`) + fresh, operand, address; or the probe temp (used as
    // `fresh`) + operand, address, old.
    tr = fit_trailing(tr, (xadd ? 3 : 4) + writes, {ld.rt, alu.rd, st.rs},
                      {need_old && bytes <= 4 ? ld.rt : 31u, width == 32 ? alu.rd : 31u, st.rs}, {st.rs});
    Finals finals;
    if (xadd) {
        const Gp operand = materialize(ctx, alu.other, width);
        const Gp old = ctx.temp_gpr();
        const Gp address = ctx.read_gpr(ld.rn, true);
        finals.set_reg(ld.rt, old, bytes <= 4);
        finals.set_reg(alu.rd, operand, width == 32);
        finals.set_zero(st.rs);
        const auto copies = copy_trailing_sources(ctx, tr, finals);
        ctx.clobber_host_flags();
        if (alu.op == AluOp::sub) a.neg(width, operand);
        a.mov(64, old, operand);
        ctx.mark_may_fault();
        a.lock();
        a.xadd(bits_of(bytes), mem_at(address), old);
        zero_extend(a, bytes, old);
        a.add(width, operand, old);   // operand := new value
        clear_reservation_if_set(a);
        emit_flags_op(ctx, apply_trailing(ctx, tr, finals, copies));
        finals.emit(ctx);
        return 4 + tr.consumed;
    }
    FixedGprs fx(ctx, {Gp::rax});
    const Gp fresh = fx.temp();
    const Gp operand = materialize(ctx, alu.other, width);
    Gp address = ctx.read_gpr(ld.rn, true);
    const Gp old = !need_old ? Gp::none : fx.claimed(Gp::rax) ? Gp::rax : fx.temp();
    if (need_old) finals.set_reg(ld.rt, old, bytes <= 4);
    finals.set_reg(alu.rd, fresh, width == 32);
    finals.set_zero(st.rs);
    const auto copies = copy_trailing_sources(ctx, tr, finals);
    ctx.clobber_host_flags();
    ctx.mark_may_fault();
    load_zx(a, bytes, fresh, mem_at(address));
    if (fx.clobbers_live()) probe_write(ctx, bytes, mem_at(address));
    Gp a2 = address, o2 = operand;
    fx.save({&a2, &o2});
    a.mov(64, Gp::rax, fresh);
    const Label retry = a.bound_label();
    switch (alu.op) {
    case AluOp::and_: a.mov(64, fresh, Gp::rax); a.and_(width, fresh, o2); break;
    case AluOp::orr: a.mov(64, fresh, Gp::rax); a.or_(width, fresh, o2); break;
    case AluOp::eor: a.mov(64, fresh, Gp::rax); a.xor_(width, fresh, o2); break;
    case AluOp::rsub: a.mov(width, fresh, o2); a.sub(width, fresh, Gp::rax); break;
    case AluOp::add: a.mov(64, fresh, Gp::rax); a.add(width, fresh, o2); break;
    case AluOp::sub: a.mov(64, fresh, Gp::rax); a.sub(width, fresh, o2); break;
    }
    ctx.mark_may_fault();
    a.lock();
    a.cmpxchg(bits_of(bytes), mem_at(a2), fresh);
    a.jcc(Cond::ne, retry);
    if (old != Gp::none && old != Gp::rax) a.mov(64, old, Gp::rax);
    fx.restore();
    clear_reservation_if_set(a);
    emit_flags_op(ctx, apply_trailing(ctx, tr, finals, copies));
    finals.emit(ctx);
    return 4 + tr.consumed;
}

// ldxr; adds/subs; stxr; cbnz [; ops] [; dmb]: lock xadd, then the new value
// recomputed from the old one with an x64 add/sub that also produces the
// guest NZCV (ADD flags are symmetric, so ADDS with Rt as either operand).
// imm: xadd temp, address (+ new value when Rd != Rt); reg: + operand.
unsigned fuse_alu_flags(EmitContext& ctx, const Excl& ld, const AluDecode& alu, const Excl& st, Trailing tr) {
    auto& a = ctx.as();
    const unsigned bytes = ld.bytes;
    const unsigned width = alu.is64 ? 64 : 32;
    const bool need_old = alu.rd != ld.rt;
    const bool imm = alu.other.kind == Operand2::Kind::imm;   // imm12 (<< 12): fits imm32
    const unsigned writes = 2 + (need_old ? 1 : 0);
    tr = fit_trailing(tr, (imm ? 2 : 3) + (need_old ? 1 : 0) + writes, {ld.rt, alu.rd, st.rs},
                      {need_old && bytes <= 4 ? ld.rt : 31u, width == 32 ? alu.rd : 31u, st.rs}, {st.rs}, true);
    const Gp operand = imm ? Gp::none : materialize(ctx, alu.other, width);
    const Gp old = ctx.temp_gpr();
    const Gp fresh = need_old ? ctx.temp_gpr() : old;
    const Gp address = ctx.read_gpr(ld.rn, true);
    Finals finals;
    if (need_old) finals.set_reg(ld.rt, old, bytes <= 4);
    finals.set_reg(alu.rd, fresh, width == 32);
    finals.set_zero(st.rs);
    const auto copies = copy_trailing_sources(ctx, tr, finals);
    ctx.clobber_host_flags();
    const bool sub = alu.op == AluOp::sub;
    if (imm) {
        a.mov_imm(old, sub ? std::uint64_t{0} - alu.other.value : alu.other.value);
    } else {
        a.mov(64, old, operand);
        if (sub) a.neg(64, old);
    }
    ctx.mark_may_fault();
    a.lock();
    a.xadd(bits_of(bytes), mem_at(address), old);
    zero_extend(a, bytes, old);
    clear_reservation_if_set(a);
    if (need_old) a.mov(64, fresh, old);
    const Alu op = sub ? Alu::sub : Alu::add;
    if (imm) a.alu(op, width, fresh, static_cast<std::int32_t>(alu.other.value));
    else a.alu(op, width, fresh, operand);
    // fit_trailing admits an EFLAGS-clobbering op (AND) only before a trailing flag setter.
    const auto trailing_flags = apply_trailing(ctx, tr, finals, copies);
    if (trailing_flags.k != FlagsOp::K::none) emit_flags_op(ctx, trailing_flags);
    else ctx.set_flags(sub ? FlagsProducer::sub : FlagsProducer::add);
    finals.emit(ctx);
    return 4 + tr.consumed;
}

// ldxr; stxr; cbnz [; mov]* [; dmb]  (exchange, or atomic RMW load when the
// stored register is the loaded one)
unsigned fuse_exchange(EmitContext& ctx, const Excl& ld, const Excl& st, Trailing tr) {
    auto& a = ctx.as();
    const unsigned bytes = ld.bytes;
    const bool reload = st.rt == ld.rt;
    tr = fit_trailing(tr, 2 + 2, {ld.rt, st.rs}, {bytes <= 4 ? ld.rt : 31u, st.rs}, {st.rs});
    const Gp old = reload ? ctx.temp_gpr() : ctx.copy_gpr(st.rt);
    const Gp address = ctx.read_gpr(ld.rn, true);
    Finals finals;
    finals.set_reg(ld.rt, old, bytes <= 4);
    finals.set_zero(st.rs);
    const auto copies = copy_trailing_sources(ctx, tr, finals);
    ctx.clobber_host_flags();
    if (reload) {
        a.mov_imm(old, 0);
        ctx.mark_may_fault();
        a.lock();
        a.xadd(bits_of(bytes), mem_at(address), old);
    } else {
        ctx.mark_may_fault();
        a.xchg(bits_of(bytes), mem_at(address), old);
    }
    zero_extend(a, bytes, old);
    clear_reservation_if_set(a);
    emit_flags_op(ctx, apply_trailing(ctx, tr, finals, copies));
    finals.emit(ctx);
    return 3 + tr.consumed;
}

// MSVC compare-exchange: ldxr; cmp; b.ne end; stxr; cbnz   (cmp == nullptr:
// ldxr; cbnz Rt, end; stxr; cbnz, i.e. expected value zero).   worst 7
unsigned fuse_cas(EmitContext& ctx, const Excl& ld, const CmpDecode* cmp, const Excl& st, unsigned consumed) {
    auto& a = ctx.as();
    const unsigned bytes = ld.bytes;
    const unsigned cmp_width = cmp ? (cmp->is64 ? 64 : 32) : reg_width(bytes);
    const unsigned value_bits = bits_of(bytes);
    const bool imm = !cmp || cmp->operand.kind == Operand2::Kind::imm;
    const std::uint64_t imm_value = !cmp ? 0 : cmp->operand.value;
    bool range_check = false, impossible = false;
    if (cmp && value_bits < cmp_width) {
        if (imm) impossible = (imm_value >> value_bits) != 0;
        else range_check = true;
    }
    FixedGprs fx(ctx, {Gp::rax});
    const Gp old = fx.claimed(Gp::rax) ? Gp::rax : fx.temp();
    const Gp expected = imm ? Gp::none : materialize(ctx, cmp->operand, cmp_width);
    Gp address = ctx.read_gpr(ld.rn, true);
    Gp value = ctx.read_gpr(st.rt);
    const Gp status = ctx.copy_gpr(st.rs);
    ctx.clobber_host_flags();
    const Label slow = a.new_label(), fail = a.new_label(), done = a.new_label();
    if (!impossible) {
        if (range_check) {
            // `old` is ours (rax when claimed): the expected value truncated to
            // the access size differs from it => the compare can never succeed.
            if (value_bits == 8) a.movzx(32, old, expected, 8);
            else if (value_bits == 16) a.movzx(32, old, expected, 16);
            else a.mov(32, old, expected);
            a.cmp(cmp_width, old, expected);
            a.jcc(Cond::ne, slow);
        }
        if (fx.clobbers_live()) probe_write(ctx, bytes, mem_at(address));
        Gp a2 = address, v2 = value, e2 = expected;
        fx.save({&a2, &v2, &e2});
        if (imm) a.mov_imm(Gp::rax, imm_value);
        else a.mov(reg_width(bytes), Gp::rax, e2);
        ctx.mark_may_fault();
        a.lock();
        a.cmpxchg(value_bits, mem_at(a2), v2);
        if (bytes < 4) zero_extend(a, bytes, Gp::rax);   // movzx keeps EFLAGS
        if (old != Gp::rax) a.mov(64, old, Gp::rax);
        fx.restore();
        a.jcc(Cond::ne, fail);
        clear_reservation_if_set(a);
        a.mov_imm(status, 0);
        a.jmp(done);
        a.bind(fail);
        record_reservation(a, address, old, bytes);
        if (range_check) a.jmp(done);
    }
    a.bind(slow);
    if (range_check || impossible) {
        // The compare can never succeed: the loop only loads (and the
        // reservation stays live). No locked instruction: LDAXR needs its
        // RCsc fence (flags are already clobbered, so this is path-neutral).
        if (ld.ordered) acquire_fence(ctx);
        ctx.mark_may_fault();
        load_zx(a, bytes, old, mem_at(address));
        record_reservation(a, address, old, bytes);
    }
    a.bind(done);
    if (cmp) {
        if (imm) a.cmp(cmp_width, old, static_cast<std::int32_t>(imm_value));
        else a.cmp(cmp_width, old, expected);
        ctx.set_flags(FlagsProducer::sub);
    }
    ctx.store_gpr(ld.rt, old);
    ctx.store_gpr(st.rs, status);
    return consumed;
}

// ldxp; stxp; cbnz [; mov]* [; dmb] for 128-bit (cmpxchg16b) and 64-bit pairs.
unsigned fuse_pair(EmitContext& ctx, const Excl& ld, const Excl& st, Trailing tr) {
    auto& a = ctx.as();
    const bool reload = st.rt == ld.rt && st.rt2 == ld.rt2;
    Finals finals;
    if (ld.bytes == 8) {   // worst 6 (reload) / 7 (exchange)
        tr = fit_trailing(tr, (reload ? 3 : 4) + 3, {ld.rt, ld.rt2, st.rs}, {ld.rt, ld.rt2, st.rs}, {st.rs});
        const Gp old = reload ? ctx.temp_gpr() : ctx.copy_gpr(st.rt2);
        const Gp high = ctx.temp_gpr();
        const Gp low = reload ? Gp::none : ctx.read_gpr(st.rt);
        const Gp address = ctx.read_gpr(ld.rn, true);
        finals.set_reg(ld.rt, old, true);
        finals.set_reg(ld.rt2, high, true);
        finals.set_zero(st.rs);
        const auto copies = copy_trailing_sources(ctx, tr, finals);
        ctx.clobber_host_flags();
        if (reload) {
            a.mov_imm(old, 0);
            ctx.mark_may_fault();
            a.lock();
            a.xadd(64, mem_at(address), old);
        } else {
            a.shl(64, old, 32);
            a.mov(32, high, low);
            a.or_(64, old, high);
            ctx.mark_may_fault();
            a.xchg(64, mem_at(address), old);
        }
        a.mov(64, high, old);
        a.shr(64, high, 32);
        a.mov(32, old, old);
        clear_reservation_if_set(a);
        emit_flags_op(ctx, apply_trailing(ctx, tr, finals, copies));
        finals.emit(ctx);
        return 3 + tr.consumed;
    }
    // 128-bit: only the atomic-load form (stxp writes back what ldxp read),
    // which needs no source operands. hooks: rax rcx rdx + address + 3 writes
    // = 7; no hooks: probe temp, address, result copy, 3 writes = 6.
    if (!reload) return 0;
    FixedGprs fx(ctx, {Gp::rax, Gp::rbx, Gp::rcx, Gp::rdx});
    tr = fit_trailing(tr, 7, {ld.rt, ld.rt2, st.rs}, {st.rs}, {st.rs});
    const Gp low = fx.claimed(Gp::rax) ? Gp::rax : fx.temp();
    const Gp high = fx.claimed(Gp::rdx) ? Gp::rdx : fx.temp();
    Gp address = ctx.read_gpr(ld.rn, true);
    finals.set_reg(ld.rt, low, false);
    finals.set_reg(ld.rt2, high, false);
    finals.set_zero(st.rs);
    const auto copies = copy_trailing_sources(ctx, tr, finals);
    ctx.clobber_host_flags();
    if (fx.clobbers_live()) probe_write(ctx, 16, mem_at(address));
    Gp a2 = address;
    fx.save({&a2});
    // rdx:rax == rcx:rbx == 0: memory is either 0 (rewritten with 0) or loaded.
    a.mov_imm(Gp::rax, 0);
    a.mov_imm(Gp::rdx, 0);
    a.mov_imm(Gp::rbx, 0);
    a.mov_imm(Gp::rcx, 0);
    ctx.mark_may_fault();
    a.lock();
    a.cmpxchg16b(mem_at(a2));
    if (low != Gp::rax) a.mov(64, low, Gp::rax);
    if (high != Gp::rdx) a.mov(64, high, Gp::rdx);
    fx.restore();
    clear_reservation_if_set(a);
    emit_flags_op(ctx, apply_trailing(ctx, tr, finals, copies));
    finals.emit(ctx);
    return 3 + tr.consumed;
}

bool distinct(std::initializer_list<unsigned> regs) {
    unsigned seen = 0;
    for (auto r : regs) {
        if (r != 31 && (seen & (1u << r))) return false;
        if (r != 31) seen |= 1u << r;
    }
    return true;
}

unsigned try_fuse(EmitContext& ctx, std::span<const std::uint32_t> words, const Excl& ld) {
    if (!fusion_enabled() || words.size() < 3 || !ld.load) return 0;
    if (ld.rt == 31 || ld.rt == ld.rn) return 0;
    Excl st;
    if (ld.pair) {
        if (ld.rt2 == 31 || ld.rt2 == ld.rn || ld.rt == ld.rt2) return 0;
        if (!decode_exclusive(words[1], st) || st.load || !st.pair || st.bytes != ld.bytes || st.rn != ld.rn) return 0;
        if (!is_cbnz(words[2], st.rs, -2)) return 0;
        const bool reload = st.rt == ld.rt && st.rt2 == ld.rt2;
        if (!reload && (st.rt == ld.rt || st.rt == ld.rt2 || st.rt2 == ld.rt || st.rt2 == ld.rt2)) return 0;
        if (st.rs == 31 || !distinct({st.rs, ld.rt, ld.rt2, ld.rn}) || st.rs == st.rt || st.rs == st.rt2) return 0;
        return fuse_pair(ctx, ld, st, scan_trailing(words, 3));
    }
    // exchange: ldxr; stxr; cbnz
    if (decode_exclusive(words[1], st) && !st.load && !st.pair && st.bytes == ld.bytes && st.rn == ld.rn &&
        is_cbnz(words[2], st.rs, -2)) {
        if (st.rs == 31 || !distinct({st.rs, ld.rt, ld.rn}) || st.rs == st.rt) return 0;
        return fuse_exchange(ctx, ld, st, scan_trailing(words, 3));
    }
    if (words.size() < 4) return 0;
    // ALU read-modify-write: ldxr; op; stxr; cbnz
    AluDecode alu;
    if (decode_alu(words[1], ld.rt, alu) && decode_exclusive(words[2], st) && !st.load && !st.pair &&
        st.bytes == ld.bytes && st.rn == ld.rn && st.rt == alu.rd && is_cbnz(words[3], st.rs, -3)) {
        if (ld.bytes == 8 && !alu.is64) return 0;
        if (alu.rd == ld.rn || st.rs == 31 || !distinct({st.rs, ld.rt, ld.rn}) || st.rs == alu.rd) return 0;
        if (alu.flags) return fuse_alu_flags(ctx, ld, alu, st, scan_trailing(words, 4));
        return fuse_alu(ctx, ld, alu, st, scan_trailing(words, 4));
    }
    // compare with zero: ldxr; cbnz Rt, end; stxr; cbnz Rs, loop
    bool test64 = false;
    if (is_cbnz(words[1], ld.rt, 3, &test64) && decode_exclusive(words[2], st) && !st.load && !st.pair &&
        st.bytes == ld.bytes && st.rn == ld.rn && is_cbnz(words[3], st.rs, -3)) {
        if (ld.bytes == 8 && !test64) return 0;
        if (st.rs == 31 || !distinct({st.rs, ld.rt, ld.rn}) || st.rs == st.rt || st.rt == ld.rt) return 0;
        return fuse_cas(ctx, ld, nullptr, st, 4);
    }
    // compare-exchange: ldxr; cmp; b.ne end; stxr; cbnz
    if (words.size() < 5) return 0;
    CmpDecode cmp;
    if (decode_cmp(words[1], ld.rt, cmp) && words[2] == (0x54000001u | (3u << 5)) &&
        decode_exclusive(words[3], st) && !st.load && !st.pair && st.bytes == ld.bytes && st.rn == ld.rn &&
        is_cbnz(words[4], st.rs, -4)) {
        if (ld.bytes == 8 && !cmp.is64) return 0;
        if (st.rs == 31 || !distinct({st.rs, ld.rt, ld.rn}) || st.rs == st.rt || st.rt == ld.rt) return 0;
        if (cmp.operand.kind == Operand2::Kind::imm && cmp.operand.value > 0x7fffffffULL) return 0;
        return fuse_cas(ctx, ld, &cmp, st, 5);
    }
    return 0;
}

// ---- ordered loads / stores --------------------------------------------------------

// worst 3. `rcsc`: LDAR/LDLAR (ordered after an earlier STLR); LDAPR/LDAPUR
// (RCpc) are plain loads.
unsigned emit_ordered_load(EmitContext& ctx, unsigned bytes, unsigned rt, unsigned rn,
                           std::int32_t disp, int sign_extend_to, bool rcsc) {
    auto& a = ctx.as();
    const Gp address = ctx.read_gpr(rn, true);
    const Gp value = ctx.temp_gpr();
    if (rcsc) acquire_fence(ctx);
    ctx.mark_may_fault();
    if (sign_extend_to == 0) load_zx(a, bytes, value, mem_at(address, disp));
    else a.movsx(static_cast<unsigned>(sign_extend_to), value, mem_at(address, disp), bits_of(bytes));
    if (sign_extend_to == 32) a.mov(32, value, value);
    ctx.store_gpr(rt, value);
    return 1;
}

// worst 2. TSO keeps every earlier access before the store (release). The
// store->load order RCsc needs before a later LDAR is the LDAR's job
// (release_pending); legacy mode, and an LDAR in the same span, use xchg.
unsigned emit_ordered_store(EmitContext& ctx, std::span<const std::uint32_t> words, unsigned bytes, unsigned rt,
                            unsigned rn, std::int32_t disp) {
    auto& a = ctx.as();
    // An LDAR later in the same straight-line code would take a fence after
    // two stores (value + flag): `xchg` is cheaper then (6.8 vs 8.7 ns).
    if (legacy_ordering() || reaches_rcsc_acquire(words, 1)) {
        const Gp copy = ctx.copy_gpr(rt);
        const Gp address = ctx.read_gpr(rn, true);
        ctx.mark_may_fault();
        a.xchg(bits_of(bytes), mem_at(address, disp), copy);   // release + store->load order
        return 1;
    }
    const Gp value = ctx.read_gpr(rt);
    const Gp address = ctx.read_gpr(rn, true);
    ctx.mark_may_fault();
    store_sized(a, bytes, mem_at(address, disp), value);
    note_release(ctx, words);
    return 1;
}

// ---- LSE -------------------------------------------------------------------------------

// A barrier after an LSE instruction (clang-cl `_InterlockedIncrement`:
// `ldaddal; [add;] dmb ish`) is subsumed by the locked instruction, through
// register-only trailing ops as after a fused loop.
class LseTail {
public:
    // base_cost: worst-case scratch use of the instruction without its write.
    LseTail(std::span<const std::uint32_t> words, unsigned base_cost, unsigned rd, unsigned bytes) : rd_(rd) {
        if (fusion_enabled() && words.size() > 1)
            tr_ = fit_trailing(scan_trailing(words, 1), base_cost + (rd != 31 ? 1u : 0u), {rd},
                               {bytes <= 4 ? rd : 31u}, {});
    }
    // After the operands, before any code: names the result register and
    // takes the trailing sources.
    void prepare(EmitContext& ctx, Gp result, bool fits32) {
        finals_.set_reg(rd_, result, fits32);   // (31: ignored)
        regs_ = copy_trailing_sources(ctx, tr_, finals_);
    }
    // After every faulting access: trailing ops, flags, guest writes.
    unsigned finish(EmitContext& ctx) {
        emit_flags_op(ctx, apply_trailing(ctx, tr_, finals_, regs_));
        finals_.emit(ctx);
        return 1 + tr_.consumed;
    }

private:
    unsigned rd_;
    Trailing tr_{};
    Finals finals_;
    TrailRegs regs_;
};

// worst 5
unsigned emit_cas(EmitContext& ctx, std::span<const std::uint32_t> words, unsigned bytes, unsigned rs, unsigned rt,
                  unsigned rn) {
    auto& a = ctx.as();
    LseTail tail(words, 4, rs, bytes);
    FixedGprs fx(ctx, {Gp::rax});
    const Gp result = fx.claimed(Gp::rax) ? Gp::rax : fx.temp();
    Gp desired = ctx.read_gpr(rt);
    Gp address = ctx.read_gpr(rn, true);
    tail.prepare(ctx, result, bytes <= 4);
    ctx.clobber_host_flags();
    if (fx.clobbers_live()) probe_write(ctx, bytes, mem_at(address));
    Gp d2 = desired, a2 = address;
    fx.save({&d2, &a2});
    ctx.load_gpr(Gp::rax, rs);
    if (bytes == 4) a.mov(32, Gp::rax, Gp::rax);
    ctx.mark_may_fault();
    a.lock();
    a.cmpxchg(bits_of(bytes), mem_at(a2), d2);
    zero_extend(a, bytes, Gp::rax);
    if (result != Gp::rax) a.mov(64, result, Gp::rax);
    fx.restore();
    return tail.finish(ctx);
}

// W: worst 4 with hooks (10 without); X: worst 4-5 with hooks (9 without)
unsigned emit_casp(EmitContext& ctx, bool wide, unsigned rs, unsigned rt, unsigned rn) {
    auto& a = ctx.as();
    if (!wide) {
        FixedGprs fx(ctx, {Gp::rax});
        const Gp desired = fx.temp();
        const Gp scratch = fx.temp();
        Gp address = ctx.read_gpr(rn, true);
        ctx.clobber_host_flags();
        ctx.load_gpr(desired, rt + 1);
        a.shl(64, desired, 32);
        ctx.load_gpr(scratch, rt);
        a.mov(32, scratch, scratch);
        a.or_(64, desired, scratch);
        ctx.load_gpr(scratch, rs + 1);
        a.shl(64, scratch, 32);
        if (fx.clobbers_live()) probe_write(ctx, 8, mem_at(address));
        Gp a2 = address;
        fx.save({&a2});
        ctx.load_gpr(Gp::rax, rs);
        a.mov(32, Gp::rax, Gp::rax);
        a.or_(64, Gp::rax, scratch);
        ctx.mark_may_fault();
        a.lock();
        a.cmpxchg(64, mem_at(a2), desired);
        a.mov(64, desired, Gp::rax);
        fx.restore();
        a.mov(64, scratch, desired);
        a.shr(64, scratch, 32);
        a.mov(32, desired, desired);
        ctx.store_gpr(rs, desired);
        ctx.store_gpr(rs + 1, scratch);
        return 1;
    }
    FixedGprs fx(ctx, {Gp::rax, Gp::rbx, Gp::rcx, Gp::rdx});
    const Gp r0 = fx.claimed(Gp::rax) ? Gp::rax : fx.temp();
    const Gp r1 = fx.claimed(Gp::rdx) ? Gp::rdx : fx.temp();
    Gp address = ctx.read_gpr(rn, true);
    // Without the hooks, load_gpr may allocate an implicit register as its
    // read register; read every operand first and let save() redirect them.
    Gp e0 = Gp::none, e1 = Gp::none, n0 = Gp::none, n1 = Gp::none;
    if (!fx.hooks()) {
        e0 = ctx.read_gpr(rs);
        e1 = ctx.read_gpr(rs + 1);
        n0 = ctx.read_gpr(rt);
        n1 = ctx.read_gpr(rt + 1);
    }
    ctx.clobber_host_flags();
    if (fx.clobbers_live()) probe_write(ctx, 16, mem_at(address));
    if (fx.hooks()) {
        fx.save({&address});
        ctx.load_gpr(Gp::rax, rs);
        ctx.load_gpr(Gp::rdx, rs + 1);
        ctx.load_gpr(Gp::rcx, rt + 1);
        ctx.load_gpr(Gp::rbx, rt);   // last: rbx may hold a pinned source
    } else {
        fx.save({&address, &e0, &e1, &n0, &n1});
        a.mov(64, Gp::rax, e0);
        a.mov(64, Gp::rdx, e1);
        a.mov(64, Gp::rcx, n1);
        a.mov(64, Gp::rbx, n0);
    }
    ctx.mark_may_fault();
    a.lock();
    a.cmpxchg16b(mem_at(address));
    if (r0 != Gp::rax) a.mov(64, r0, Gp::rax);
    if (r1 != Gp::rdx) a.mov(64, r1, Gp::rdx);
    fx.restore();
    ctx.store_gpr(rs, r0);
    ctx.store_gpr(rs + 1, r1);
    return 1;
}

// LDADD/LDCLR/LDEOR/LDSET/LDSMAX/LDSMIN/LDUMAX/LDUMIN (opc 0..7) and SWP (opc 8).
// worst 3 (swp/ldadd/st*) / 5 (loops)
unsigned emit_lse_rmw(EmitContext& ctx, std::span<const std::uint32_t> words, unsigned bytes, unsigned opc,
                      unsigned rs, unsigned rt, unsigned rn) {
    auto& a = ctx.as();
    const unsigned bits = bits_of(bytes);
    const bool simple = opc == 8 || opc == 0 || (rt == 31 && opc <= 3);
    LseTail tail(words, simple ? 2 : 4, rt, bytes);
    if (opc == 8) {   // SWP
        const Gp value = ctx.copy_gpr(rs);
        const Gp address = ctx.read_gpr(rn, true);
        tail.prepare(ctx, value, bytes <= 4);
        ctx.mark_may_fault();
        a.xchg(bits, mem_at(address), value);
        zero_extend(a, bytes, value);
        return tail.finish(ctx);
    }
    if (opc == 0) {   // LDADD / STADD
        const Gp value = ctx.copy_gpr(rs);
        const Gp address = ctx.read_gpr(rn, true);
        tail.prepare(ctx, value, bytes <= 4);
        ctx.clobber_host_flags();
        ctx.mark_may_fault();
        a.lock();
        if (rt == 31) {
            a.alu(Alu::add, bits, mem_at(address), value);
            return tail.finish(ctx);
        }
        a.xadd(bits, mem_at(address), value);
        zero_extend(a, bytes, value);
        return tail.finish(ctx);
    }
    if (rt == 31 && opc <= 3) {   // STCLR / STEOR / STSET
        const Gp value = ctx.copy_gpr(rs);
        const Gp address = ctx.read_gpr(rn, true);
        tail.prepare(ctx, value, false);
        ctx.clobber_host_flags();
        if (opc == 1) a.not_(64, value);
        ctx.mark_may_fault();
        a.lock();
        a.alu(opc == 1 ? Alu::and_ : opc == 2 ? Alu::xor_ : Alu::or_, bits, mem_at(address), value);
        return tail.finish(ctx);
    }
    // compare-exchange loop
    FixedGprs fx(ctx, {Gp::rax});
    const Gp fresh = fx.temp();
    Gp operand = ctx.copy_gpr(rs);
    Gp address = ctx.read_gpr(rn, true);
    tail.prepare(ctx, fresh, bytes <= 4);
    ctx.clobber_host_flags();
    const unsigned width = bytes == 8 ? 64 : 32;
    const bool is_signed = opc == 4 || opc == 5;
    // operand: ~Rs for CLR, Rs otherwise; sign/zero-extended to `width` for max/min.
    if (bytes < 4) {
        if (is_signed) a.movsx(32, operand, operand, bits);
        else a.movzx(32, operand, operand, bits);
    }
    if (opc == 1) a.not_(64, operand);
    ctx.mark_may_fault();
    load_zx(a, bytes, fresh, mem_at(address));
    if (fx.clobbers_live()) probe_write(ctx, bytes, mem_at(address));
    Gp a2 = address, o2 = operand;
    fx.save({&a2, &o2});
    a.mov(64, Gp::rax, fresh);
    const Label retry = a.bound_label();
    switch (opc) {
    case 1: a.mov(64, fresh, Gp::rax); a.and_(64, fresh, o2); break;
    case 2: a.mov(64, fresh, Gp::rax); a.xor_(64, fresh, o2); break;
    case 3: a.mov(64, fresh, Gp::rax); a.or_(64, fresh, o2); break;
    default: {
        if (bytes >= 4) a.mov(width, fresh, Gp::rax);
        else if (is_signed) a.movsx(32, fresh, Gp::rax, bits);
        else a.mov(32, fresh, Gp::rax);
        a.cmp(width, fresh, o2);
        const Cond take = opc == 4 ? Cond::l : opc == 5 ? Cond::g : opc == 6 ? Cond::b : Cond::a;
        a.cmov(take, width, fresh, o2);
        break;
    }
    }
    ctx.mark_may_fault();
    a.lock();
    a.cmpxchg(bits, mem_at(a2), fresh);
    a.jcc(Cond::ne, retry);
    a.mov(64, fresh, Gp::rax);
    fx.restore();
    zero_extend(a, bytes, fresh);
    return tail.finish(ctx);
}

} // namespace

unsigned emit_atomic(EmitContext& ctx, std::span<const std::uint32_t> words) {
    const auto w = words[0];
    const unsigned rt = rd_of(w), rn = rn_of(w), rs = rm_of(w);
    if ((w & 0x3f000000u) == 0x08000000u) {
        Excl e;
        if (decode_exclusive(w, e)) {
            if (e.load) {
                if (const auto fused = try_fuse(ctx, words, e)) return fused;
                return emit_load_exclusive(ctx, e);
            }
            return emit_store_exclusive(ctx, e);
        }
        const bool o2 = (w & (1u << 23)) != 0, o1 = (w & (1u << 21)) != 0;
        const unsigned rt2 = (w >> 10) & 31u;
        if (o2 && !o1) {   // LDAR/STLR/LDLAR/STLLR (Rs, Rt2: should-be-one, ignored)
            const unsigned bytes = 1u << (w >> 30);
            if (w & (1u << 22)) return emit_ordered_load(ctx, bytes, rt, rn, 0, 0, true);
            return emit_ordered_store(ctx, words, bytes, rt, rn, 0);
        }
        if (o2 && o1) {    // CAS
            if (rt2 != 31) return 0;
            return emit_cas(ctx, words, 1u << (w >> 30), rs, rt, rn);
        }
        if (!o2 && o1 && (w & 0x80000000u) == 0) {   // CASP
            if (rt2 != 31 || (rs & 1u) || (rt & 1u)) return 0;
            return emit_casp(ctx, (w & 0x40000000u) != 0, rs, rt, rn);
        }
        return 0;
    }
    if ((w & 0x3f200c00u) == 0x38200000u) {
        const unsigned bytes = 1u << (w >> 30);
        const bool o3 = (w & (1u << 15)) != 0;
        const unsigned opc = (w >> 12) & 7u;
        if (!o3) return emit_lse_rmw(ctx, words, bytes, opc, rs, rt, rn);
        if (opc == 0) return emit_lse_rmw(ctx, words, bytes, 8, rs, rt, rn);
        if (opc == 4 && (w & (1u << 23)) && !(w & (1u << 22)))   // LDAPR (Rs: should-be-one)
            return emit_ordered_load(ctx, bytes, rt, rn, 0, 0, false);
        return 0;
    }
    if ((w & 0x3f200c00u) == 0x19000000u) {   // STLUR / LDAPUR / LDAPURS
        const unsigned size = w >> 30, opc = (w >> 22) & 3u;
        const auto disp = sign_extend((w >> 12) & 0x1ffu, 9);
        const unsigned bytes = 1u << size;
        if (opc == 0) return emit_ordered_store(ctx, words, bytes, rt, rn, disp);
        if (opc == 1) return emit_ordered_load(ctx, bytes, rt, rn, disp, 0, false);
        if (size == 3 || (size == 2 && opc == 3)) return 0;
        return emit_ordered_load(ctx, bytes, rt, rn, disp, opc == 2 ? 64 : 32, false);
    }
    return 0;
}

bool describe_atomic(std::uint32_t w, InstructionUse& use) {
    const unsigned rt = rd_of(w), rn = rn_of(w), rs = rm_of(w);
    const auto bit = [](unsigned r) { return r == 31 ? 0u : 1u << r; };
    use = {};
    use.may_fault = true;
    use.gpr_read = 1u << rn;   // rn == 31 means SP (bit 31)
    if ((w & 0x3f000000u) == 0x08000000u) {
        Excl e;
        if (decode_exclusive(w, e)) {
            if (e.load) {
                use.gpr_write = bit(e.rt) | (e.pair ? bit(e.rt2) : 0u);
            } else {
                use.gpr_read |= bit(e.rt) | (e.pair ? bit(e.rt2) : 0u);
                use.gpr_write = bit(e.rs);
            }
            return true;
        }
        const bool o2 = (w & (1u << 23)) != 0, o1 = (w & (1u << 21)) != 0;
        if (o2 && !o1) {
            if (w & (1u << 22)) use.gpr_write = bit(rt);
            else use.gpr_read |= bit(rt);
            return true;
        }
        if (o2 && o1) {
            use.gpr_read |= bit(rs) | bit(rt);
            use.gpr_write = bit(rs);
            return true;
        }
        if (!o2 && o1 && (w & 0x80000000u) == 0) {
            use.gpr_read |= bit(rs) | bit(rs + 1) | bit(rt) | bit(rt + 1);
            use.gpr_write = bit(rs) | bit(rs + 1);
            return true;
        }
        return false;
    }
    if ((w & 0x3f200c00u) == 0x38200000u) {
        const bool o3 = (w & (1u << 15)) != 0;
        const unsigned opc = (w >> 12) & 7u;
        if (o3 && opc == 4) { use.gpr_write = bit(rt); return true; }
        if (o3 && opc != 0) return false;
        use.gpr_read |= bit(rs);
        use.gpr_write = bit(rt);
        return true;
    }
    if ((w & 0x3f200c00u) == 0x19000000u) {
        if (((w >> 22) & 3u) == 0) use.gpr_read |= bit(rt);
        else use.gpr_write = bit(rt);
        return true;
    }
    return false;
}

} // namespace transmute::fx::system_detail
