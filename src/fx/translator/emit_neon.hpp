#pragma once

// NEON family (owner: neon): integer Advanced SIMD data processing, permutes,
// moves and every SIMD&FP load/store. Floating-point arithmetic belongs to the
// fp family; this family declines those encodings.
//
// register_neon_emitters() (translator.hpp) registers two FamilyEmitters:
// "neon" (emit_neon/describe_neon) and "vector_memory"
// (emit_vector_memory/describe_vector_memory).

#include "fx/translator/emit_context.hpp"

#include <cstdint>
#include <span>

namespace transmute::fx {

// Integer SIMD data processing (vector and scalar forms).
unsigned emit_neon(EmitContext& ctx, std::span<const std::uint32_t> words);
bool describe_neon(std::uint32_t word, InstructionUse& use);

// SIMD&FP loads and stores (LDR/STR/LDP/STP/LDNP/STNP B/H/S/D/Q, LD1-LD4,
// ST1-ST4, LD1R-LD4R, single-lane forms).
unsigned emit_vector_memory(EmitContext& ctx, std::span<const std::uint32_t> words);
bool describe_vector_memory(std::uint32_t word, InstructionUse& use);

// Address-folding hook (FamilyEmitter::address_base): LDR/STR (immediate
// offset, register offset), LDP/STP/LDNP/STNP (offset), LD1-LD4/ST1-ST4 and
// LD1R-LD4R / single lane (no post-index). False with TRANSMUTE_FX_NEON=nofold.
bool vector_memory_address_base(std::uint32_t word, unsigned& base, bool& indexed);
// Registered family names, including TRANSMUTE_FX_NEON knob suffixes.
const char* neon_family_name();
// Vector value facts of the neon fusions (FamilyEmitter::facts; emit_neon_fusion.cpp).
const FamilyFacts* neon_family_facts();
const char* vector_memory_family_name();

// ---- constant pool -----------------------------------------------------------
// NEON sequences reference 16-byte constants (pshufb masks, lookup tables,
// saturation bounds) through rip-relative operands. By default each
// instruction that needs constants emits them right after its own code behind
// a short jmp. When the translator core brackets a region with
// neon_begin_region()/neon_end_region(), constants are deduplicated per
// region and emitted once by neon_end_region(), which must be called at a
// point that is never executed (after the region's last unconditional jump)
// and before Assembler::finalize(). The pool is per thread and per assembler.
void neon_begin_region(x64::Assembler& as);
void neon_end_region(x64::Assembler& as);

// ---- testing ------------------------------------------------------------------
// Restricts the host features the emitters use: 0 = baseline (SSE4.2), 1 = up
// to AVX, 2 = up to AVX2/BMI2, -1 = everything the host supports (default).
// Levels the host lacks are never enabled.
void neon_set_feature_level(int level);

} // namespace transmute::fx
