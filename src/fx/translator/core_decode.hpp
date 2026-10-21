#pragma once

// A64 decoder for the instructions the translator core lowers itself
// (integer data processing, integer loads/stores, branches, exception
// generation). Owner: translator core.

#include <cstdint>

namespace transmute::fx::core {

enum class Kind : std::uint8_t {
    unknown,          // not a core instruction: families, then interpret
    nop,              // architecturally no effect (PRFM*, PAC data-processing with PAC disabled)
    add_sub_imm,      // op: 0 add 1 sub; s
    logic_imm,        // op: 0 and 1 orr 2 eor 3 ands; imm = mask
    move_wide,        // op: 0 movn 2 movz 3 movk; imm = imm16, amount = shift
    adr,              // imm = absolute result (ADR/ADRP)
    bitfield,         // op: 0 sbfm 1 bfm 2 ubfm; amount = immr, imms
    extr,             // amount = lsb
    add_sub_shift,    // op, s, shift = type, amount
    add_sub_ext,      // op, s, shift = option, amount
    adc,              // op: 0 adc 1 sbc; s
    logic_shift,      // op: 0 and 1 orr 2 eor 3 ands; invert; shift, amount
    ccmp,             // op: 0 ccmn 1 ccmp; imm_form (imm = imm5), cond, imms = nzcv
    csel,             // op: 0 csel 1 csinc 2 csinv 3 csneg; cond
    dp1,              // op: Dp1
    dp2,              // op: Dp2
    dp3,              // op: Dp3; ra
    load_store,       // size, mem, mode, imm (offset), shift/amount (register offset)
    load_store_pair,  // size, mem, mode, imm, ra = Rt2
    load_literal,     // size, mem, imm = absolute address
    branch,           // imm = target
    branch_link,      // imm = target
    branch_cond,      // cond, imm = target
    compare_branch,   // op: 0 cbz 1 cbnz; imm = target
    test_branch,      // op: 0 tbz 1 tbnz; amount = bit; imm = target
    branch_reg,       // rn
    branch_link_reg,  // rn
    ret,              // rn
    brk,              // imm = imm16
    hlt,              // imm = imm16
    svc,              // imm = imm16
    udf,
};

enum Dp1 : std::uint8_t { dp1_rbit, dp1_rev16, dp1_rev32, dp1_rev, dp1_clz, dp1_cls };
enum Dp2 : std::uint8_t { dp2_udiv, dp2_sdiv, dp2_lslv, dp2_lsrv, dp2_asrv, dp2_rorv };
enum Dp3 : std::uint8_t {
    dp3_madd, dp3_msub, dp3_smaddl, dp3_smsubl, dp3_umaddl, dp3_umsubl, dp3_smulh, dp3_umulh,
};

enum class MemOp : std::uint8_t {
    store,
    load,            // zero-extending
    load_signed64,   // sign-extend to 64 bits
    load_signed32,   // sign-extend to 32 bits (zero upper)
    prefetch,
};

enum class AddrMode : std::uint8_t { offset, pre, post, reg };

struct Inst {
    std::uint32_t word{};
    Kind kind{Kind::unknown};
    std::uint8_t rd{};      // Rd / Rt
    std::uint8_t rn{};
    std::uint8_t rm{};
    std::uint8_t ra{};      // Ra (dp3) / Rt2 (pairs)
    std::uint8_t op{};
    std::uint8_t cond{};
    std::uint8_t shift{};   // shift type or extend option
    std::uint8_t amount{};  // shift amount / immr / lsb / bit
    std::uint8_t imms{};
    std::uint8_t size{};    // log2 access bytes
    bool sf{};              // 64-bit operation (or 64-bit load destination)
    bool s{};               // sets flags
    bool invert{};          // logical N bit
    bool imm_form{};        // ccmp immediate form
    MemOp mem{};
    AddrMode mode{};
    std::int64_t imm{};
};

// Decodes `word` at guest address `pc`. Returns false (kind = unknown) if
// the instruction is not lowered by the core.
bool decode(std::uint32_t word, std::uint64_t pc, Inst& out);

// Static register usage. Bit n = Xn (n < 31), bit 31 = SP. XZR is omitted.
struct Use {
    std::uint32_t read{};
    std::uint32_t write{};
    bool reads_flags{};
    bool writes_flags{};
    bool may_fault{};
};
Use usage(const Inst& inst);

// DecodeBitMasks for logical immediates. False for reserved encodings.
bool decode_bit_masks(unsigned n, unsigned imms, unsigned immr, bool sf, std::uint64_t& mask);

// ARM condition evaluation (cond 14/15 are always true).
bool condition_holds(unsigned cond, std::uint32_t nzcv);

} // namespace transmute::fx::core
