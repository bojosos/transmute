#pragma once

// Test utilities for the fp family (owner: fp).

#include "fx/testing/harness.hpp"
#include "fx/testing/test.hpp"
#include "fx/translator/emit_fp.hpp"
#include "transmute/fx/context.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace transmute::fx::fptest {

struct Rng {
    std::uint64_t state;
    explicit Rng(std::uint64_t seed) : state(seed * 0x9e3779b97f4a7c15ULL + 0x1234567ULL) {}
    std::uint64_t next() { return testing::random_u64(state); }
    unsigned below(unsigned n) { return static_cast<unsigned>(next() % n); }
    bool coin() { return (next() & 1) != 0; }
};

// ---- values ---------------------------------------------------------------------
const std::vector<std::uint64_t>& specials(unsigned es);
std::uint64_t random_fp(Rng& rng, unsigned es, bool allow_snan = true);
// Fills every V register with lanes of `es` bits from random_fp.
void fill_vectors(GuestContext& ctx, Rng& rng, unsigned es, bool allow_snan = true);
bool is_nan(std::uint64_t v, unsigned es);
bool is_snan(std::uint64_t v, unsigned es);
std::uint64_t f2b(float f);
std::uint64_t d2b(double d);

// FPCR values covering every rounding mode with FZ off and on.
const std::vector<std::uint32_t>& fpcr_modes();

// ---- checking -------------------------------------------------------------------
struct CheckOptions {
    unsigned nan_es = 0;          // NaN-tolerant lane size for V registers (0 = bit exact)
    bool fz_tininess = true;      // tolerate x64's after-rounding tininess when FZ = 1
    bool compare_fpsr = false;    // compare cumulative FPSR bits via a trailing MRS
    std::uint32_t fpsr_mask = 0x1f;
    bool both_contexts = true;    // also run through the aliasing (pinned-style) context
    // Corrects the oracle's expected state for known reference-interpreter
    // bugs (see NOTES_FP.md).
    std::function<void(std::uint32_t word, GuestContext& expected)> fix_expected;
    // Reference used when the reference interpreter lacks the word.
    std::function<bool(std::uint32_t word, GuestContext& expected)> fallback;
    // Reference used instead of the reference interpreter when set.
    std::function<bool(std::uint32_t word, GuestContext& expected)> reference;
};

// Oracle vs translated (MemoryEmitContext and the aliasing context).
// Returns false and reports failures on mismatch. `oracle_missing` is set
// (and nothing is reported) if the reference interpreter lacks the word.
bool check_word(std::uint32_t word, const GuestContext& ctx, const CheckOptions& opt,
                const std::string& label, bool* oracle_missing = nullptr);
// Translated vs a precomputed expected context.
bool check_expected(std::uint32_t word, const GuestContext& ctx, const GuestContext& expected,
                    const CheckOptions& opt, const std::string& label);
// Compares two contexts with the tolerances of `opt`.
bool compare_fp(const GuestContext& expected, const GuestContext& actual, const CheckOptions& opt,
                std::uint32_t fpcr, const std::string& label);

// Compiles `word` once (both contexts) and checks it on every context of
// `contexts` (each with its own fpcr). Stops after `max_failures` failing
// contexts. Returns the number of contexts checked (oracle or fallback).
unsigned check_batch(std::uint32_t word, const std::vector<GuestContext>& contexts, const CheckOptions& opt,
                     const std::string& name, unsigned max_failures = 6);

// Variants of the aliasing context.
struct AliasOptions {
    bool mem_sources = false;   // EmitContext::vsrc hands out GuestContext slots
    bool track_zero = false;    // honour note_vec_zero_above across the words of a run
    std::uint32_t zero32 = 0;   // registers known zero above bit 32 at the start (caller guarantees)
    std::uint32_t zero64 = 0;   // registers known zero above bit 64 at the start
};
// Zeroes the bits the claims of `opt` say are zero.
void apply_zero_claims(GuestContext& ctx, const AliasOptions& opt);
// The variants check_word/check_batch run besides the plain aliasing context:
// memory sources, and known-zero upper bits (on a context sanitized with
// apply_zero_claims) with and without memory sources.
const std::vector<AliasOptions>& alias_variants();

// A note_vec_zero_above() an emitter made.
struct ZeroNote {
    unsigned index;
    unsigned width;
};
// Every note must hold in `expected`: bits [127:width] of the register are zero.
bool check_notes(const std::vector<ZeroNote>& notes, const GuestContext& expected, const std::string& label);

// Runs words through the aliasing context: guest registers live in host
// registers for the whole instruction, write_vec/write_gpr return the same
// host register as a read of the same guest register.
bool run_aliasing(GuestContext& ctx, std::span<const std::uint32_t> words, std::uint32_t mxcsr,
                  const AliasOptions& options = {}, std::vector<ZeroNote>* notes = nullptr);

// ---- code size --------------------------------------------------------------------
// Number of x64 instructions in `code` (length decoder for the forms the
// emitters produce).
unsigned count_x64_instructions(const std::vector<std::uint8_t>& code);
// Instructions on the common path: forward branches are taken (the fp
// emitters place rarely executed fix-up code behind them).
unsigned common_path_instructions(const std::vector<std::uint8_t>& code);
// Instructions the fp family emits for `word` with guest registers pinned
// (operands cost nothing; destination pinned to the same host register).
// common_path: count only the common path, else every emitted instruction.
// zero_upper: every source register is known zero above its scalar.
unsigned emitted_instructions(std::uint32_t word, bool common_path = true, bool zero_upper = false);
// Guest registers and flags the emitter actually reads and writes for `word`.
bool emitted_uses(std::uint32_t word, InstructionUse& use);

// ---- features -----------------------------------------------------------------------
struct FeatureScope {
    explicit FeatureScope(const FpFeatures& f) { fp_set_features_for_testing(&f); }
    ~FeatureScope() { fp_set_features_for_testing(nullptr); }
    FeatureScope(const FeatureScope&) = delete;
    FeatureScope& operator=(const FeatureScope&) = delete;
};
inline FpFeatures sse_only() { return FpFeatures{}; }
inline FpFeatures avx_no_fma() { FpFeatures f = fp_host_features(); f.fma = false; return f; }

// ---- statistics ---------------------------------------------------------------------
struct Tally {
    unsigned checked = 0, oracle_missing = 0, declined = 0;
};

} // namespace transmute::fx::fptest
