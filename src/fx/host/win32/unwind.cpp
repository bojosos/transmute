// win32 backend of fx/host/unwind.hpp: a growable function table callback.

#include "fx/host/unwind.hpp"

#include <windows.h>

#include <cstddef>

namespace transmute::fx::host {

static_assert(sizeof(UnwindEntry) == sizeof(RUNTIME_FUNCTION));
static_assert(offsetof(UnwindEntry, begin) == offsetof(RUNTIME_FUNCTION, BeginAddress) &&
              offsetof(UnwindEntry, end) == offsetof(RUNTIME_FUNCTION, EndAddress) &&
              offsetof(UnwindEntry, unwind) == offsetof(RUNTIME_FUNCTION, UnwindData));

namespace {

PRUNTIME_FUNCTION CALLBACK lookup_callback(DWORD64 control_pc, PVOID context) {
    return reinterpret_cast<PRUNTIME_FUNCTION>(reinterpret_cast<UnwindLookup>(context)(control_pc));
}

} // namespace

bool register_unwind_lookup(std::uint64_t base, std::uint32_t size, UnwindLookup lookup) noexcept {
    // The low two bits of the identifier must be set (RtlInstallFunctionTableCallback).
    const DWORD64 identifier = base | 3;
    return RtlInstallFunctionTableCallback(identifier, base, size, &lookup_callback, reinterpret_cast<PVOID>(lookup),
                                           nullptr) != FALSE;
}

} // namespace transmute::fx::host
