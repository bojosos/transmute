// Helper table for HelperId 48..63 (owner: atomics/system). This object is
// only linked by callers of system_helper()/install_system_helpers() (the
// runtime, tests), because the IC helpers reference fx::invalidate_guest_code.
#include "fx/translator/emit_system.hpp"
#include "fx/translator/emit_system_internal.hpp"

#include "transmute/fx/process.hpp"

namespace transmute::fx {
namespace system_detail {

const void* timer_data_address();

void helper_ic_ivau(GuestContext*, std::uint64_t address) {
    invalidate_guest_code(address & ~0xfffULL, 0x1000);
}

void helper_ic_iallu(GuestContext*) {
    invalidate_guest_code(0, ~0ULL);
}

} // namespace system_detail

const void* system_helper(unsigned id) {
    using namespace system_detail;
    switch (id) {
    case helper_system_ic_ivau: return reinterpret_cast<const void*>(&helper_ic_ivau);
    case helper_system_ic_iallu: return reinterpret_cast<const void*>(&helper_ic_iallu);
    case helper_system_counter: return reinterpret_cast<const void*>(&helper_counter);
    case helper_system_crc32: return reinterpret_cast<const void*>(&helper_crc32);
    case helper_system_crypto: return reinterpret_cast<const void*>(&helper_crypto);
    case helper_system_timer_data: return timer_data_address();
    default: return nullptr;
    }
}

void install_system_helpers(GuestContext& ctx) {
    for (unsigned id = helper_system_first; id < helper_abi_first; ++id) ctx.helpers[id] = system_helper(id);
}

} // namespace transmute::fx
