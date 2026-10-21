// register_system_emitters (owner: atomics/system). Kept in its own object so
// linking the family library does not require the translator core.
#include "fx/translator/emit_system.hpp"
#include "fx/translator/translator.hpp"

namespace transmute::fx {

void register_system_emitters() {
    static const bool registered = [] {
        register_family_emitter(system_family());
        return true;
    }();
    (void)registered;
}

} // namespace transmute::fx
