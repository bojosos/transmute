// register_neon_emitters() lives in its own object file so that linking the
// emitters (e.g. into fx_emit_neon_tests) does not require the translator
// core's register_family_emitter (owner: neon).
#include "fx/translator/emit_neon.hpp"
#include "fx/translator/translator.hpp"

namespace transmute::fx {

void register_neon_emitters() {
    // Knob suffixes are part of the names, so translator_family_fingerprint()
    // (and the persistent cache key) changes with TRANSMUTE_FX_NEON.
    register_family_emitter(FamilyEmitter{neon_family_name(), &emit_neon, &describe_neon, nullptr, neon_family_facts()});
    register_family_emitter(FamilyEmitter{vector_memory_family_name(), &emit_vector_memory, &describe_vector_memory,
                                          &vector_memory_address_base});
}

} // namespace transmute::fx
