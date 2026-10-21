#pragma once

// Test utilities for the atomics/system family (owner: atomics/system):
// A64 encoders, a mini translator for multi-instruction programs with
// branches (system family + a test-only integer/branch subset), an oracle
// program runner and a multi-thread driver.

#include "fx/testing/harness.hpp"
#include "fx/translator/emit_system.hpp"
#include "fx/translator/emit_system_core_like.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace transmute::fx::systest {

// ---- A64 encoders -------------------------------------------------------------
namespace a64 {
constexpr unsigned XZR = 31, SP = 31;
inline std::uint32_t sz(unsigned bytes) { return bytes == 1 ? 0u : bytes == 2 ? 1u : bytes == 4 ? 2u : 3u; }
inline std::uint32_t ldxr(unsigned bytes, unsigned rt, unsigned rn, bool acquire = false) {
    return (sz(bytes) << 30) | 0x085f7c00u | (acquire ? 0x8000u : 0u) | (rn << 5) | rt;
}
inline std::uint32_t stxr(unsigned bytes, unsigned rs, unsigned rt, unsigned rn, bool release = false) {
    return (sz(bytes) << 30) | 0x08007c00u | (rs << 16) | (release ? 0x8000u : 0u) | (rn << 5) | rt;
}
inline std::uint32_t ldxp(bool wide, unsigned rt, unsigned rt2, unsigned rn, bool acquire = false) {
    return 0x887f0000u | (wide ? 0x40000000u : 0u) | (acquire ? 0x8000u : 0u) | (rt2 << 10) | (rn << 5) | rt;
}
inline std::uint32_t stxp(bool wide, unsigned rs, unsigned rt, unsigned rt2, unsigned rn, bool release = false) {
    return 0x88200000u | (wide ? 0x40000000u : 0u) | (rs << 16) | (release ? 0x8000u : 0u) | (rt2 << 10) |
           (rn << 5) | rt;
}
inline std::uint32_t ldar(unsigned bytes, unsigned rt, unsigned rn) { return (sz(bytes) << 30) | 0x08dffc00u | (rn << 5) | rt; }
inline std::uint32_t stlr(unsigned bytes, unsigned rt, unsigned rn) { return (sz(bytes) << 30) | 0x089ffc00u | (rn << 5) | rt; }
inline std::uint32_t ldlar(unsigned bytes, unsigned rt, unsigned rn) { return (sz(bytes) << 30) | 0x08df7c00u | (rn << 5) | rt; }
inline std::uint32_t stllr(unsigned bytes, unsigned rt, unsigned rn) { return (sz(bytes) << 30) | 0x089f7c00u | (rn << 5) | rt; }
inline std::uint32_t ldapr(unsigned bytes, unsigned rt, unsigned rn) { return (sz(bytes) << 30) | 0x38bfc000u | (rn << 5) | rt; }
inline std::uint32_t rcpc2(unsigned bytes, unsigned opc, int imm9, unsigned rt, unsigned rn) {
    return (sz(bytes) << 30) | 0x19000000u | (opc << 22) | ((static_cast<unsigned>(imm9) & 0x1ffu) << 12) |
           (rn << 5) | rt;
}
inline std::uint32_t cas(unsigned bytes, bool acq, bool rel, unsigned rs, unsigned rt, unsigned rn) {
    return (sz(bytes) << 30) | 0x08a07c00u | (acq ? 1u << 22 : 0u) | (rs << 16) | (rel ? 0x8000u : 0u) |
           (rn << 5) | rt;
}
inline std::uint32_t casp(bool wide, bool acq, bool rel, unsigned rs, unsigned rt, unsigned rn) {
    return (wide ? 0x40000000u : 0u) | 0x08207c00u | (acq ? 1u << 22 : 0u) | (rs << 16) |
           (rel ? 0x8000u : 0u) | (rn << 5) | rt;
}
// opc: 0 ADD 1 CLR 2 EOR 3 SET 4 SMAX 5 SMIN 6 UMAX 7 UMIN, 8 = SWP
inline std::uint32_t lse(unsigned opc, unsigned bytes, bool acq, bool rel, unsigned rs, unsigned rt, unsigned rn) {
    const unsigned o3 = opc == 8 ? 1u : 0u, op = opc == 8 ? 0u : opc;
    return (sz(bytes) << 30) | 0x38200000u | (acq ? 1u << 23 : 0u) | (rel ? 1u << 22 : 0u) | (rs << 16) |
           (o3 << 15) | (op << 12) | (rn << 5) | rt;
}
inline std::uint32_t add_imm(bool x, unsigned rd, unsigned rn, unsigned imm, bool shift12 = false) {
    return (x ? 0x80000000u : 0u) | 0x11000000u | (shift12 ? 1u << 22 : 0u) | (imm << 10) | (rn << 5) | rd;
}
inline std::uint32_t sub_imm(bool x, unsigned rd, unsigned rn, unsigned imm) {
    return (x ? 0x80000000u : 0u) | 0x51000000u | (imm << 10) | (rn << 5) | rd;
}
inline std::uint32_t subs_imm(bool x, unsigned rd, unsigned rn, unsigned imm) {
    return (x ? 0x80000000u : 0u) | 0x71000000u | (imm << 10) | (rn << 5) | rd;
}
inline std::uint32_t cmp_imm(bool x, unsigned rn, unsigned imm) { return subs_imm(x, 31, rn, imm); }
// base: 0x0b ADD, 0x4b SUB, 0x6b SUBS, 0x0a AND, 0x2a ORR, 0x4a EOR (N via base | 0x200000)
inline std::uint32_t alu_reg(std::uint32_t base, bool x, unsigned rd, unsigned rn, unsigned rm,
                             unsigned shift = 0, unsigned amount = 0) {
    return (x ? 0x80000000u : 0u) | (base << 24) | (shift << 22) | (rm << 16) | (amount << 10) | (rn << 5) | rd;
}
inline std::uint32_t mov_reg(bool x, unsigned rd, unsigned rm) { return alu_reg(0x2a, x, rd, 31, rm); }
inline std::uint32_t cmp_reg(bool x, unsigned rn, unsigned rm) { return alu_reg(0x6b, x, 31, rn, rm); }
inline std::uint32_t cmp_ext(bool x, unsigned rn, unsigned rm, unsigned option, unsigned amount = 0) {
    return (x ? 0x80000000u : 0u) | 0x6b200000u | (rm << 16) | (option << 13) | (amount << 10) | (rn << 5) | 31u;
}
inline std::uint32_t movz(bool x, unsigned rd, unsigned imm16, unsigned hw = 0) {
    return (x ? 0x80000000u : 0u) | 0x52800000u | (hw << 21) | (imm16 << 5) | rd;
}
inline std::uint32_t ldr_x(unsigned rt, unsigned rn, unsigned offset = 0) { return 0xf9400000u | ((offset / 8) << 10) | (rn << 5) | rt; }
inline std::uint32_t str_x(unsigned rt, unsigned rn, unsigned offset = 0) { return 0xf9000000u | ((offset / 8) << 10) | (rn << 5) | rt; }
inline std::uint32_t b(int offset_words) { return 0x14000000u | (static_cast<unsigned>(offset_words) & 0x3ffffffu); }
inline std::uint32_t bcond(unsigned cond, int offset_words) {
    return 0x54000000u | ((static_cast<unsigned>(offset_words) & 0x7ffffu) << 5) | cond;
}
inline std::uint32_t cbz(bool x, unsigned rt, int offset_words) {
    return (x ? 0x80000000u : 0u) | 0x34000000u | ((static_cast<unsigned>(offset_words) & 0x7ffffu) << 5) | rt;
}
inline std::uint32_t cbnz(bool x, unsigned rt, int offset_words) { return cbz(x, rt, offset_words) | 0x01000000u; }
constexpr std::uint32_t kDmbIsh = 0xd5033bbfu, kDmbSy = 0xd5033fbfu, kDmbIshld = 0xd50339bfu,
                        kDmbIshst = 0xd5033abfu, kDsbIsh = 0xd5033b9fu, kIsb = 0xd5033fdfu,
                        kClrex = 0xd5033f5fu, kNop = 0xd503201fu, kYield = 0xd503203fu;
constexpr unsigned NE = 1, EQ = 0;
} // namespace a64

// ---- helpers ------------------------------------------------------------------------

// Knob scopes (restore the defaults).
struct LegacyOrderingScope {
    explicit LegacyOrderingScope(bool on) { system_set_legacy_ordering(on); }
    ~LegacyOrderingScope() { system_set_legacy_ordering(false); }
};
struct ExtendedFusionScope {
    explicit ExtendedFusionScope(bool on) { system_set_extended_fusion(on); }
    ~ExtendedFusionScope() { system_set_extended_fusion(true); }
};

// Installs the family's helpers into ctx (random_context leaves them null).
void prepare(GuestContext& ctx);
// Fresh context with helpers installed.
GuestContext make_context(std::uint64_t seed);
// Clears the oracle's exclusive monitor on this thread.
void oracle_clear_monitor();

// Executes `words` (at ctx.pc) with the reference interpreter until pc leaves
// the program. Returns false if the oracle declines an instruction.
bool oracle_run(GuestContext& ctx, const std::vector<std::uint32_t>& words, unsigned max_steps = 100000);

// How Program emits code: MemoryEmitContext (default) or CoreLikeContext.
struct ProgramMode {
    bool core_like = false;
    CoreLikeOptions core;
    const char* name = "memory";
};
// Mode used by Programs constructed while the scope is alive.
class ProgramModeScope {
public:
    explicit ProgramModeScope(const ProgramMode& mode);
    ~ProgramModeScope();
private:
    ProgramMode saved_;
};
const ProgramMode& current_program_mode();

// Mini translator: family first, then a test-only subset (ADD/SUB/ADDS/SUBS
// imm and shifted reg, AND/ORR/EOR reg, MOVZ, LDR/STR X imm, B, B.cond,
// CBZ/CBNZ). Spans passed to the family stop before branch targets, exactly
// as the core's `words` contract says.
class Program {
public:
    explicit Program(std::vector<std::uint32_t> words, std::uint64_t base_pc = 0x0000'0001'4000'1000ULL);
    // Runs the program on ctx (ctx.pc is set to the base and ends at the end).
    void run(GuestContext& ctx) const;
    [[nodiscard]] std::uint64_t base() const { return base_; }
    [[nodiscard]] const std::vector<std::uint32_t>& words() const { return words_; }
    [[nodiscard]] std::size_t code_size() const { return code_size_; }
    // Number of words the family consumed per call, for fusion assertions.
    [[nodiscard]] const std::vector<unsigned>& family_spans() const { return spans_; }

private:
    std::vector<std::uint32_t> words_;
    std::uint64_t base_;
    std::unique_ptr<testing::Snippet> snippet_;
    std::size_t code_size_{};
    std::vector<unsigned> spans_;
};

// Runs `words` from ctx with the oracle and with Program (memory restored in
// between) and compares registers, flags and the first `memory_bytes` of
// `buffer`. `actual` (optional) receives the translated result. Returns
// false (and reports) on any difference.
bool differential(const std::vector<std::uint32_t>& words, const GuestContext& ctx,
                  testing::Buffer& buffer, const std::string& label, std::size_t memory_bytes = 4096,
                  GuestContext* actual = nullptr, const testing::CompareOptions& options = {});

// Same, but the oracle runs `reference` (an equivalent program it supports).
bool differential(const std::vector<std::uint32_t>& reference, const std::vector<std::uint32_t>& words,
                  const GuestContext& ctx, testing::Buffer& buffer, const std::string& label,
                  std::size_t memory_bytes = 4096, GuestContext* actual = nullptr,
                  const testing::CompareOptions& options = {});

// Fills `buffer` with deterministic pseudo-random bytes.
void fill_random(testing::Buffer& buffer, std::uint64_t seed);

// Runs `body(thread_index)` on `threads` host threads released together.
// Returns the wall time in seconds from release to the last thread finishing.
double run_threads(unsigned threads, const std::function<void(unsigned)>& body);

// Median of `runs` measurements.
double median_of(unsigned runs, const std::function<double()>& measure);

// IC IVAU stub bookkeeping (process.hpp invalidate_guest_code is stubbed).
struct InvalidateCall { std::uint64_t address, size; };
std::vector<InvalidateCall>& invalidate_calls();

} // namespace transmute::fx::systest
