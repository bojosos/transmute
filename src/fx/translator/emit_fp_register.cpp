// Registers the fp family with the translator core (owner: fp). Kept in its
// own object file so tests that only use fp_emit() do not need the core's
// register_family_emitter() at link time.
#include "fx/translator/emit_fp.hpp"
#include "fx/translator/translator.hpp"

namespace transmute::fx {

void register_fp_emitters() { register_family_emitter(fp_family()); }

} // namespace transmute::fx
