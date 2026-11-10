// linux backend of fx/host/unwind.hpp.
//
// The code arena describes its frames with Win64 unwind data, which the
// Linux unwinder does not read. Until the arena also emits DWARF CFI
// (registered with __register_frame), a C++ exception cannot cross
// translated frames on this host: the registration is accepted and nothing
// is registered.
#include "fx/host/unwind.hpp"

namespace transmute::fx::host {

bool register_unwind_lookup(std::uint64_t, std::uint32_t, UnwindLookup) noexcept { return true; }

} // namespace transmute::fx::host
