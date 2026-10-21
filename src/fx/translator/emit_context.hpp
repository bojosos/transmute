#pragma once

// Instruction-emitter interface (owner: integration; additive changes only).
//
// The core translator owns region formation, control flow, register pinning
// and lazy flags. Instruction families (fp, neon, atomics/system) implement
// FamilyEmitter::emit against EmitContext and never emit control flow that
// leaves their own code sequence.
//
// RULES FOR EMITTERS
//  1. Guest memory is host memory: address guest data with plain x64 memory
//     operands built from guest register values. Call mark_may_fault()
//     immediately before every x64 instruction that touches guest memory.
//  2. Perform every access that may fault before writing any guest register
//     (or the guest-visible flags). Stores may be partially performed.
//  3. Call clobber_host_flags() before the first x64 instruction that writes
//     EFLAGS, unless you then call set_flags()/set_nzcv() as the result.
//  4. read_gpr()/read_vec() results are read-only. write_gpr()/write_vec()
//     may return the same host register as a read of the same guest register:
//     consume all sources before writing the destination, or compute into a
//     temp and move. Never read a guest register after writing it in the same
//     instruction. Write all 64 (or 128) bits: 32-bit x64 ops zero-extend, and
//     A64 scalar FP/SIMD writes zero the upper vector bits.
//  5. Obtain every operand and temp BEFORE emitting internal labels/branches,
//     and change flags state only in straight-line code (after a merge). The
//     context is not path-sensitive.
//  6. call_helper() invalidates every register obtained earlier in the
//     instruction; request operands again afterwards.
//  7. r15 (context, biased by kContextBias) and rsp are reserved. Use
//     x_disp()/v_disp()/helper_disp() from transmute/fx/context.hpp only for
//     explicit context fields, never for guest registers (they may be pinned).
//  8. Check cpu_features() before using AVX/AVX2/FMA/BMI/etc.; the baseline is
//     SSE4.2 + POPCNT.

#include "fx/common/x64_assembler.hpp"
#include "transmute/fx/context.hpp"

#include <cstdint>
#include <initializer_list>
#include <span>

namespace transmute::fx {

// How the x64 EFLAGS left by the immediately preceding instruction encode the
// ARM NZCV result.
enum class FlagsProducer : std::uint8_t {
    sub,    // x64 SUB/CMP/NEG of (a - b): N=SF Z=ZF C=!CF V=OF
    add,    // x64 ADD (a + b):           N=SF Z=ZF C=CF  V=OF
    logic,  // x64 AND/TEST/OR/XOR:        N=SF Z=ZF C=0   V=0
};

struct HelperArg {
    enum class Kind : std::uint8_t { host, immediate, guest_gpr } kind;
    x64::Gp reg{x64::Gp::none};
    std::uint64_t value{};
    static HelperArg host(x64::Gp r) { return {Kind::host, r, 0}; }
    static HelperArg imm(std::uint64_t v) { return {Kind::immediate, x64::Gp::none, v}; }
    static HelperArg guest(unsigned index) { return {Kind::guest_gpr, x64::Gp::none, index}; }
};

enum class HelperEffects : std::uint8_t {
    pure,                // touches no guest state; only volatile host registers are clobbered
    reads_state,         // reads guest registers/flags from GuestContext
    reads_writes_state,  // may also modify guest registers/flags/pc in GuestContext
};

class EmitContext {
public:
    virtual ~EmitContext() = default;

    virtual x64::Assembler& as() = 0;
    // Guest address of words[0].
    [[nodiscard]] virtual std::uint64_t pc() const = 0;

    // ---- integer registers (index 0..30; 31 = ZR, or SP when sp == true) ----
    virtual x64::Gp read_gpr(unsigned index, bool sp = false) = 0;
    virtual x64::Gp write_gpr(unsigned index, bool sp = false) = 0;
    // Fresh scratch copy of a guest register that the emitter may modify.
    virtual x64::Gp copy_gpr(unsigned index, bool sp = false) = 0;
    // Fresh scratch register. At least 5 are available per instruction in
    // addition to the guest operands.
    virtual x64::Gp temp_gpr() = 0;

    // ---- vector registers (128-bit V0..V31) ---------------------------------
    virtual x64::Xmm read_vec(unsigned index) = 0;
    virtual x64::Xmm write_vec(unsigned index) = 0;
    // At least 4 are available per instruction in addition to operands.
    virtual x64::Xmm temp_vec() = 0;

    // ---- flags ---------------------------------------------------------------
    virtual void clobber_host_flags() = 0;
    virtual void set_flags(FlagsProducer producer) = 0;
    // `value` holds NZCV in ARM layout (bits 31..28); other bits must be zero.
    virtual void set_nzcv(x64::Gp value) = 0;
    // Scratch register with ARM-layout NZCV.
    virtual x64::Gp get_nzcv() = 0;
    // x64 condition that is true iff ARM condition `arm_condition` (0..13)
    // holds, valid for the next x64 jcc/setcc/cmovcc emitted. May emit code
    // and clobber EFLAGS; call it right before that consumer.
    virtual x64::Cond condition(unsigned arm_condition) = 0;

    // ---- faults ----------------------------------------------------------------
    virtual void mark_may_fault() = 0;

    // ---- helpers ---------------------------------------------------------------
    // Calls helpers[id] (Win64 ABI) with rcx = GuestContext* and `args` in
    // rdx, r8, r9. Returns a scratch register holding rax.
    virtual x64::Gp call_helper(unsigned id, std::initializer_list<HelperArg> args,
                                HelperEffects effects) = 0;

    // ---- relocatable guest addresses (added by neon; additive) ---------------
    // Scratch GPR holding the absolute guest address `address` (e.g. a literal
    // pool entry). Implementations that produce relocatable code override this
    // to record a FixupKind::guest_address_imm64 for the immediate. The default
    // emits a plain movabs without a fixup (JIT-only correctness).
    virtual x64::Gp guest_address(std::uint64_t address) {
        const x64::Gp r = temp_gpr();
        as().mov_imm64(r, address);
        return r;
    }

    // ---- implicit-operand registers and direct moves (added by atomics/system;
    // additive) ----------------------------------------------------------------
    // cmpxchg/cmpxchg16b/rdtsc/mul/sha256rnds2 need rax/rdx/rcx/rbx/xmm0. A
    // context whose scratch pool is small should override these so families
    // need no extra scratch for them; the defaults are always correct.
    //
    // Claims `preferred` as a scratch temp of this instruction if it is free
    // scratch and returns it; otherwise returns Gp::none and consumes nothing.
    virtual x64::Gp temp_gpr_prefer(x64::Gp preferred) {
        (void)preferred;
        return x64::Gp::none;
    }
    // Loads guest register `index` (31 = ZR, or SP when sp) into `dst`, a
    // register this instruction owns (temp or claimed implicit register),
    // without consuming another scratch register where possible.
    virtual void load_gpr(x64::Gp dst, unsigned index, bool sp = false) {
        const x64::Gp r = read_gpr(index, sp);
        if (r != dst) as().mov(64, dst, r);
    }
    // Writes all 64 bits of guest register `index` from `src` (rule 2: only
    // after every faulting access of the instruction). Same aliasing rules as
    // write_gpr(): consume every source of `index` first.
    virtual void store_gpr(unsigned index, x64::Gp src, bool sp = false) {
        const x64::Gp d = write_gpr(index, sp);
        if (d != src) as().mov(64, d, src);
    }

    // ---- vector upper-bit knowledge (added by fp/neon; additive) -------------
    // A64 scalar writes zero the bits above the scalar; VEX scalar ops copy
    // them from their first source. A context that remembers which guest V
    // registers hold zeros above bit 32 / 64 lets the families skip the
    // explicit zeroing (vinsertps/movq) after scalar operations.
    //  * vec_zero_above(width), width 32 or 64: bit v set = bits [127:width]
    //    of Vv are known to be zero. Query it before write_vec() of the same
    //    register in this instruction (write_vec forgets that register).
    //  * note_vec_zero_above(index, width): called after write_vec(index) when
    //    the value written has bits [127:width] zero (width 16/32 -> 32, 64).
    // Implementations forget everything at labels, helper calls and exits.
    // The defaults (nothing known) are always correct.
    [[nodiscard]] virtual std::uint32_t vec_zero_above(unsigned width) const {
        (void)width;
        return 0;
    }
    virtual void note_vec_zero_above(unsigned index, unsigned width) {
        (void)index;
        (void)width;
    }

    // ---- source operands without a scratch register (added by translator
    // core; additive) ----------------------------------------------------------
    // vsrc(v): a read-only source for V`v`: its host register when it is pinned
    // or already held in a register (block-local cache), otherwise the 16-byte
    // GuestContext slot as a memory operand -- unless V`v` is read again soon
    // in the block, in which case it is loaded once into a cached register.
    // gsrc(n, sp): the same for X`n` (8-byte slot; 31 = SP when sp, else XZR
    // as a zeroed scratch register).
    // Use the memory form wherever x64 accepts a memory source: the second
    // source of VEX ops (any size: scalar ss/sd forms read the low 4/8 bytes),
    // legacy SSE sources (slots are 16-byte aligned), movd/movq/movss loads,
    // GPR ALU/cmp/test/imul second operands. Never write a VecSrc/GprSrc; it
    // stays valid until the end of the instruction or call_helper(). A memory
    // source still holds the value from before this instruction, so it may be
    // consumed after write_vec()/write_gpr() of the same register (unlike a
    // register source of a pinned guest register).
    // Defaults: read_vec()/read_gpr() (always a register).
    struct VecSrc {
        x64::Xmm reg{};
        x64::Mem mem{};
        bool is_mem{};
    };
    struct GprSrc {
        x64::Gp reg{x64::Gp::none};
        x64::Mem mem{};
        bool is_mem{};
    };
    virtual VecSrc vsrc(unsigned index) { return {read_vec(index), {}, false}; }
    virtual GprSrc gsrc(unsigned index, bool sp = false) { return {read_gpr(index, sp), {}, false}; }

    // ---- FCMP flags fusion (added by translator core; additive) ---------------
    // FCMP/FCMPE may hand the raw (u)comiss/(u)comisd EFLAGS (ZF PF CF) to the
    // context instead of building NZCV; the flags' consumers (B.cond, CSEL,
    // CSET, FCSEL, ...) then use one x64 condition where possible.
    //  1. swap = fcmp_swap(): true if the context wants (u)comis(Rm, Rn)
    //     rather than (u)comis(Rn, Rm). Call before emitting the compare.
    //  2. emit the (u)comis with that operand order (Rm = #0.0 for the zero
    //     forms), with no other EFLAGS writer after it.
    //  3. set_fcmp_flags(swap): true = the context took the EFLAGS as the guest
    //     NZCV (do not call set_flags/set_nzcv). false = build NZCV yourself
    //     from the same EFLAGS (flag-preserving code, e.g. mov_imm not xor)
    //     and call set_flags/set_nzcv as before.
    // Contexts returning true from fcmp_swap() accept in set_fcmp_flags().
    // Defaults: no swap, not accepted (the existing lowering).
    virtual bool fcmp_swap() { return false; }
    virtual bool set_fcmp_flags(bool swapped) {
        (void)swapped;
        return false;
    }

    // ---- deferred vector writes (added by translator core; additive) ----------
    // True if a write_vec(index) register becomes guest-visible only when the
    // instruction completes (the context commits it at the end), so a load may
    // target it before a later access of the same instruction that may still
    // fault (rule 2 then holds without a temp and a move). Default: false.
    [[nodiscard]] virtual bool vec_write_deferred(unsigned index) const {
        (void)index;
        return false;
    }

    // ---- current context slots (added by neon/fp; additive) -------------------
    // vec_slot(v, slot): true if V`v`'s 16-byte GuestContext slot holds its
    // current value (not pinned; any register copy is clean), with `slot` that
    // memory operand. Lets an emitter broadcast or extract an element straight
    // from memory (vbroadcastss / vmovddup / movss: one load, no shuffle) even
    // when vsrc() would hand out a register. Valid like a vsrc() memory
    // source: until the end of the instruction or call_helper(). Default:
    // false (no such knowledge).
    virtual bool vec_slot(unsigned index, x64::Mem& slot) {
        (void)index;
        (void)slot;
        return false;
    }

    // ---- guest address operands (added by neon; additive) ---------------------
    // address(g): the memory operand of an access of this instruction to
    //   [Xbase|SP + extend(Xindex) << shift + disp]
    // (index 32 = none; option 2 UXTW, 3 LSL/UXTX, 6 SXTW, 7 SXTX; shift
    // 0..4). For accesses without base writeback only: the base register is
    // not available to the caller. Offsets of further chunks may be added to
    // the result's disp (up to +-64 KiB stays encodable).
    // A context may fold the address computation of a preceding ADD/SUB whose
    // result only feeds this access into the operand (the integer side does
    // the same for GPR loads/stores; see core_isel.hpp), and may skip a
    // UXTW/SXTW the index's producer already did. The default reads the
    // registers (read_gpr) and extends the index into a temp.
    struct GuestAddress {
        unsigned base{};          // 0..31, 31 = SP
        unsigned index{32};       // 0..31 (31 = XZR), 32 = none
        unsigned option{3};
        unsigned shift{};
        std::int32_t disp{};
    };
    virtual x64::Mem address(const GuestAddress& g) {
        auto& a = as();
        const x64::Gp base = read_gpr(g.base, true);
        if (g.index >= 32) return x64::ptr(base, g.disp);
        const x64::Gp index = read_gpr(g.index);
        x64::Gp effective = index;
        if (g.option == 2 || g.option == 6) {   // UXTW / SXTW
            effective = temp_gpr();
            if (g.option == 2) a.mov(32, effective, index);
            else a.movsx(64, effective, index, 32);
        }
        if (g.shift <= 3) return x64::ptr(base, effective, 1u << g.shift, g.disp);
        // shift 4 (Q): index * 16 via two lea.
        const x64::Gp t = temp_gpr();
        a.lea(64, t, x64::ptr_index(effective, 8));
        a.lea(64, t, x64::ptr(base, t, 2));
        return x64::ptr(t, g.disp);
    }

    // ---- vector value facts (added by neon fusion; additive) ------------------
    // A fusion may be valid only if a guest vector register holds a particular
    // value (the {0,1,2,3} shift vector of a movemask idiom) or holds lanes
    // that are each all ones or all zeros (a vector compare result). Families
    // ask for such facts before code generation (FamilyFacts::want, below);
    // the context answers at emission for the current instruction:
    //  * exact: proven from the region's dataflow: the register's only
    //    reaching definition (on every path from every region entry) is an
    //    instruction that produces the value (MOVI) or lane masks (FCMxx/CMxx).
    //  * flag: its only reaching definition is a 128-bit load that runs on
    //    every path to this instruction. Right after that load the context
    //    stored a byte at `flag` that is zero iff the loaded value was the
    //    requested one (FamilyFacts::check). The emitter tests the byte and
    //    keeps an exact general path for a nonzero byte.
    // Never inferred from register numbers, addresses or memory contents seen
    // at translation time. Defaults: nothing known (always correct).
    struct VecValue {
        std::uint64_t lo{}, hi{};
        bool operator==(const VecValue&) const = default;
    };
    struct VecFact {
        enum class K : std::uint8_t { none, exact, flag } k{K::none};
        x64::Mem flag{};
    };
    virtual VecFact vec_fact(unsigned index, const VecValue& value) {
        (void)index;
        (void)value;
        return {};
    }
    // True if every 32-bit lane of V`index` is 0 or 0xffffffff here (exact).
    virtual bool vec_lane_masks(unsigned index) {
        (void)index;
        return false;
    }
    // ---- derived vector values (tier 2; additive) -----------------------------
    // A value computed from a guest vector register by a pure transform of
    // the family's (FamilyFacts::derive, `kind`), e.g. TBL's adjusted
    // indices. If the family asked for it (FamilyFactWant::K::derived), the
    // context may have computed it into `slot` (16 bytes, unaligned) right
    // after every definition of V`index` that reaches this instruction (all
    // known, in less deeply nested loops): the family reads it from there
    // instead of computing it. Default: unavailable.
    virtual bool vec_derived(unsigned index, unsigned kind, x64::Mem& slot) {
        (void)index;
        (void)kind;
        (void)slot;
        return false;
    }
};

// Static register usage of one instruction, used for pinning decisions.
struct InstructionUse {
    std::uint32_t gpr_read{};    // bit n = Xn read (bit 31 = SP)
    std::uint32_t gpr_write{};
    std::uint32_t vec_read{};
    std::uint32_t vec_write{};
    bool reads_flags{};
    bool writes_flags{};
    bool may_fault{};
};

// Returns the number of guest instructions consumed (>= 1), or 0 if the family
// does not handle words[0]. `words` holds words[0] and the following words up
// to (not including) the next instruction that is a branch target inside the
// region, so a fusion may consume a self-contained loop whose only internal
// target is words[0].
using FamilyEmitFn = unsigned (*)(EmitContext& ctx, std::span<const std::uint32_t> words);
// Optional. Returns false if the family does not handle `word`.
using FamilyDescribeFn = bool (*)(std::uint32_t word, InstructionUse& use);

// Optional (added by neon; additive). True if `word` is an access without
// base writeback whose emitter reads its base register Xbase (0..30) only as
// EmitContext::GuestAddress::base of address(), never through read_gpr()
// or as an index; `indexed`: the access has an index register too. The
// translator core may then fold an ADD/SUB that writes Xbase right before
// the access, and is dead after it, into the access's operand.
using FamilyAddressBaseFn = bool (*)(std::uint32_t word, unsigned& base, bool& indexed);

// Optional (added by neon fusion; additive): vector value facts, see
// EmitContext::vec_fact. A context that supports them calls, before code
// generation:
//  * want(words, out): the facts a fusion starting at words[0] would use
//    (about register values before words[0]); returns how many (<= 4).
//  * def(word, vec, value): what `word` leaves in V`vec` when it writes it:
//    a constant (`value` set), lane masks, a full 128-bit load (checkable),
//    or unknown. Asked of every family, whichever family emits the word.
// and at emission, right after the load chosen as a flag definition (its
// value in host `reg`, or in its context slot `slot` when reg is not
// valid), check(): EFLAGS-preserving code that stores to the byte `flag` a
// value that is zero iff the 128 bits equal `value`.
struct FamilyFactWant {
    enum class K : std::uint8_t { value, lane_masks, derived } k{K::value};   // derived: value.lo = kind
    std::uint8_t vec{};
    EmitContext::VecValue value{};
};
enum class FamilyFactDef : std::uint8_t { unknown, constant, lane_masks, load128 };
struct FamilyFacts {
    unsigned (*want)(std::span<const std::uint32_t> words, FamilyFactWant* out);   // may be nullptr
    FamilyFactDef (*def)(std::uint32_t word, unsigned vec, EmitContext::VecValue& value);   // may be nullptr
    void (*check)(EmitContext& ctx, x64::Xmm reg, bool in_reg, const x64::Mem& slot,
                  const EmitContext::VecValue& value, const x64::Mem& flag);   // may be nullptr
    // (additive, tier 2) Computes derived value `kind` of `src` (a guest
    // vector register's value) into `out` (EmitContext::vec_derived).
    void (*derive)(EmitContext& ctx, x64::Xmm src, unsigned kind, const x64::Mem& out){nullptr};   // may be nullptr
};

struct FamilyEmitter {
    const char* name;
    FamilyEmitFn emit;
    FamilyDescribeFn describe;   // may be nullptr
    FamilyAddressBaseFn address_base{nullptr};   // additive (neon)
    const FamilyFacts* facts{nullptr};           // additive (neon fusion)
};

// Owner: translator core. Families registered later are tried later.
void register_family_emitter(const FamilyEmitter& family);

} // namespace transmute::fx
