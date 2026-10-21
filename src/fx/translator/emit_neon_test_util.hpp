#pragma once

// Test support for the NEON family (owner: neon).
//
// Every check runs the instruction through the reference interpreter and
// through the emitters in several configurations:
//   * MemoryEmitContext (no aliasing), constants inline per instruction;
//   * AliasingEmitContext: a pinned-register style context where a write of
//     guest register r returns the same host register as a read of r, with the
//     region-level constant pool;
// each at every host feature level (baseline SSE4.2, AVX, AVX2).

#include "fx/testing/harness.hpp"
#include "fx/testing/test.hpp"
#include "fx/translator/emit_neon.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace transmute::fx::neon_test {

using testing::Buffer;

enum class Outcome { ok, mismatch, oracle_missing, declined, both_declined };

// A note_vec_zero_above() an emitter made (checked against the expected state).
struct ZeroNote {
    unsigned index;
    unsigned width;
};

struct MemoryRange {
    std::uint8_t* data{};
    std::size_t size{};
};

struct Stats {
    unsigned checked{};          // words compared against the oracle
    unsigned oracle_missing{};   // emitted but neither the oracle nor a C++ reference covers them
    unsigned reference_checked{};   // compared against emit_neon_test_reference.cpp
    unsigned declined{};         // the oracle implements them but the family declined
    unsigned reserved{};         // both declined
};
Stats& stats();

// Compares one instruction against the oracle in every configuration.
// `memory` (optional) is snapshotted, restored between runs and compared.
Outcome check(FamilyEmitFn family, const GuestContext& start, std::uint32_t word,
              const std::string& label, MemoryRange memory = {},
              const testing::CompareOptions& options = {});

// Same, for a reference implemented in C++ (when the oracle lacks or gets an
// instruction wrong). `reference` mutates the context (and memory).
Outcome check_with(FamilyEmitFn family, const GuestContext& start, std::uint32_t word,
                   const std::string& label, const std::function<void(GuestContext&)>& reference,
                   MemoryRange memory = {});

// Hand-written references for words the oracle lacks (emit_neon_test_reference.cpp).
// Returns false if `word` is not covered; otherwise updates registers (not pc).
bool reference_step(GuestContext& ctx, std::uint32_t word);

// Runs a straight-line sequence as one region (in every configuration) and
// compares with stepping the oracle through it.
bool check_sequence(FamilyEmitFn family, const GuestContext& start, std::span<const std::uint32_t> words,
                    const std::string& label, MemoryRange memory = {});

// XMM usage of the heaviest instructions seen by the aliasing context.
struct Usage {
    std::uint32_t word;
    unsigned temps;   // temp_vec() calls
    unsigned xmm;     // host XMM registers in use (operands + temps)
};
const std::vector<Usage>& heaviest_instructions();

// Runs words through the emitters only (MemoryEmitContext, inline constants).
bool run(GuestContext& ctx, FamilyEmitFn family, std::span<const std::uint32_t> words);

// Emits `word` with the given feature level and returns the code bytes
// (MemoryEmitContext prologue/epilogue excluded), for instruction counting.
std::vector<std::uint8_t> emit_bytes(FamilyEmitFn family, std::uint32_t word, int level, bool region_pool);

// Random state with vector lanes biased to boundary values of element `size`.
GuestContext special_context(std::uint64_t seed, unsigned size);
std::uint64_t rnd(std::uint64_t& state);

// Feature levels available on this host (subset of {0, 1, 2}).
const std::vector<int>& levels();

// Prints a one-line summary of stats() with a prefix.
void print_stats(const char* name);

} // namespace transmute::fx::neon_test
