#pragma once

// Internal utilities shared by the atomics/system family sources
// (owner: atomics/system). Not part of the public interface.

#include "fx/translator/emit_context.hpp"
#include "fx/translator/emit_system.hpp"
#include "transmute/fx/context.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace transmute::fx::system_detail {

using x64::Gp;
using x64::Mem;
using x64::Xmm;

constexpr Gp kCtx = Gp::r15;

inline Mem ctx_mem(std::size_t offset) { return x64::ptr(kCtx, ctx_disp(offset)); }
inline Mem excl_address_mem() { return ctx_mem(offsetof(GuestContext, excl_address)); }
inline Mem excl_value_mem(unsigned half = 0) {
    return ctx_mem(offsetof(GuestContext, excl_value) + half * 8u);
}
inline Mem excl_size_mem() { return ctx_mem(offsetof(GuestContext, excl_size)); }

// Host features the emitters may use (cpu_features() filtered by the testing
// override in emit_system.cpp).
struct HostFeatures {
    bool aes{}, pclmul{}, sha{}, rdrand{}, rdseed{}, invariant_tsc{}, bmi2{};
};
const HostFeatures& host();
bool fusion_enabled();
// Flag-setting LL/SC bodies and CMP/CMN/AND between a loop and its barrier.
bool extended_fusion();
// TRANSMUTE_FX_SYSTEM=oldorder / system_set_legacy_ordering(true).
bool legacy_ordering();

// ---- memory ordering (emit_system.cpp; see NOTES_SYSTEM.md "Ordering") -------
// Full fence (DMB/DSB): `lock or [rsp], 0`, then clears release_pending.
void full_barrier(EmitContext& ctx);
// After a store-release (STLR/STLLR/STLUR): sets release_pending unless
// words[1..] reach a full barrier through stores/register-only code.
void note_release(EmitContext& ctx, std::span<const std::uint32_t> words);
// Before an RCsc acquire load (LDAR/LDLAR/unfused LDAXR/LDAXP): fences iff
// release_pending is set. Clobbers EFLAGS.
void acquire_fence(EmitContext& ctx);
// DMB full / DSB (not SSBB/PSSBB): emitted as full_barrier by this family.
bool is_fence_word(std::uint32_t word);
// True if words[from..] (straight-line, same region) reach a fence word
// through plain stores and instructions that read no guest memory only.
bool reaches_full_barrier(std::span<const std::uint32_t> words, std::size_t from);
// True if words[from..] (straight-line) reach an RCsc acquire load
// (LDAR/LDLAR/LDAXR/LDAXP) before any fence or possible branch.
bool reaches_rcsc_acquire(std::span<const std::uint32_t> words, std::size_t from);

// ---- fixed-register reservation ---------------------------------------------
//
// cmpxchg/cmpxchg16b/rdtsc/mul use implicit registers. FixedGprs claims each
// through EmitContext::temp_gpr_prefer(); contexts without that hook get one
// plain temp_gpr() probe (pool orders such as MemoryEmitContext's hand out
// rax/rcx/rdx/rbx first). An implicit register that could not be claimed may
// hold a live value (a pinned guest register): save() copies it into an xmm
// temp, redirects operands that live in it, and restore() puts it back.
// clobbers_live() tells callers to probe faulting accesses before the clobber
// (rule 2: a fault must observe every pinned guest register intact).
//
// Budget: the core's RegionEmitter has 7 scratch GPRs shared with unpinned
// operands, so every emitter here is written to stay within 7 even when
// nothing is pinned and the hooks are not overridden (see NOTES_SYSTEM.md).
class FixedGprs {
public:
    FixedGprs(EmitContext& ctx, std::initializer_list<Gp> fixed);

    // A scratch temp (a leftover probe register first). Call before save().
    Gp temp();
    [[nodiscard]] bool clobbers_live() const;
    [[nodiscard]] bool claimed(Gp r) const;
    // True when the context implements temp_gpr_prefer (and, by contract, the
    // other register hooks), so load_gpr/store_gpr cost no scratch.
    [[nodiscard]] bool hooks() const { return hooks_; }

    void save(std::initializer_list<Gp*> operands);
    void restore();

private:
    EmitContext& ctx_;
    std::array<Gp, 4> fixed_{};
    std::array<bool, 4> claimed_{};
    std::array<Xmm, 4> copy_{};
    unsigned count_{};
    std::array<Gp, 2> spare_{};
    unsigned spares_{};
    bool hooks_{};
    bool saved_{};
};

// Same idea for xmm0 (SHA256RNDS2 implicit operand).
class FixedXmm0 {
public:
    FixedXmm0(EmitContext& ctx, unsigned extra_temps);
    Xmm temp(unsigned index) const { return spare_[index]; }
    void save(std::initializer_list<Xmm*> operands);
    void restore();

private:
    EmitContext& ctx_;
    std::array<Xmm, 8> spare_{};
    unsigned spare_count_{};
    bool need_save_{};
    Xmm copy_{Xmm::xmm0};
};

// ---- encodings / small helpers ------------------------------------------------

inline unsigned rd_of(std::uint32_t w) { return w & 31u; }
inline unsigned rn_of(std::uint32_t w) { return (w >> 5) & 31u; }
inline unsigned rm_of(std::uint32_t w) { return (w >> 16) & 31u; }

// Decodes an A64 logical (bitmask) immediate. False if reserved.
bool decode_bit_mask(unsigned n, unsigned imms, unsigned immr, bool is64, std::uint64_t& value);

// Emits a sized mov from a register into memory (8/16/32/64).
inline void store_sized(x64::Assembler& a, unsigned bytes, const Mem& m, Gp value) {
    a.mov(bytes * 8u, m, value);
}
// Zero-extending sized load (8/16/32/64).
inline void load_zx(x64::Assembler& a, unsigned bytes, Gp dst, const Mem& m) {
    if (bytes == 1) a.movzx(32, dst, m, 8);
    else if (bytes == 2) a.movzx(32, dst, m, 16);
    else a.mov(bytes == 8 ? 64 : 32, dst, m);
}
// Zero-extends the low `bytes` of a register in place.
inline void zero_extend(x64::Assembler& a, unsigned bytes, Gp r) {
    if (bytes == 1) a.movzx(32, r, r, 8);
    else if (bytes == 2) a.movzx(32, r, r, 16);
    else if (bytes == 4) a.mov(32, r, r);
}

// ---- sub-family entry points ----------------------------------------------------
unsigned emit_atomic(EmitContext& ctx, std::span<const std::uint32_t> words);
bool describe_atomic(std::uint32_t word, InstructionUse& use);
unsigned emit_sysop(EmitContext& ctx, std::span<const std::uint32_t> words);
bool describe_sysop(std::uint32_t word, InstructionUse& use);
unsigned emit_crypto(EmitContext& ctx, std::span<const std::uint32_t> words);
bool describe_crypto(std::uint32_t word, InstructionUse& use);

// ---- helpers (emit_system_helpers.cpp) ------------------------------------------
std::uint64_t timer_multiplier();          // (nominal << 32) / tsc_hz, 0 if rdtsc unusable
std::uint64_t helper_counter(GuestContext* ctx);
void helper_ic_ivau(GuestContext* ctx, std::uint64_t address);
void helper_ic_iallu(GuestContext* ctx);
std::uint64_t helper_crc32(GuestContext* ctx, std::uint64_t acc, std::uint64_t data,
                           std::uint64_t kind);
void helper_crypto(GuestContext* ctx, std::uint64_t word);

// Software CRC32 (ISO-HDLC, reflected 0xEDB88320) / CRC32C (0x82F63B78) of
// the low `bytes` bytes of `data`, as the A64 CRC32* instructions define them.
std::uint32_t crc32_update(std::uint32_t acc, std::uint64_t data, unsigned bytes, bool castagnoli);

// Executes a vector crypto instruction word on ctx (portable implementation
// shared by the helper and the tests' reference).
bool crypto_reference(GuestContext& ctx, std::uint32_t word);

} // namespace transmute::fx::system_detail
