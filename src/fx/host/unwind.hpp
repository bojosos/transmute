#pragma once

// Host area "unwind": telling the host's unwinder how to walk through
// generated code (docs/FX_HOST_SEAMS.md, "unwind").
//
// The code arena describes its frames with Win64 unwind data: UNWIND_INFO
// blocks inside the arena and RUNTIME_FUNCTION-shaped UnwindEntry records,
// which the runtime finds by host pc. That format is part of the Win64
// calling convention of generated code (stubs.cpp, the translator's frames)
// and stays as it is; a SysV backend would emit DWARF CFI instead and
// register it with __register_frame, which this seam does not do yet.

#include "fx/host/host.hpp"

#include <cstdint>

namespace transmute::fx::host {

// Win64 RUNTIME_FUNCTION: [begin, end) and the unwind data, all relative to
// the registered base.
struct UnwindEntry {
    std::uint32_t begin;
    std::uint32_t end;
    std::uint32_t unwind;
};
static_assert(sizeof(UnwindEntry) == 12);

// Returns the entry covering host pc `pc`, or nullptr.
using UnwindLookup = UnwindEntry* (*)(std::uint64_t pc);

// The host unwinder asks `lookup` for every pc in [base, base + size)
// (RtlInstallFunctionTableCallback). False if it could not be registered.
bool register_unwind_lookup(std::uint64_t base, std::uint32_t size, UnwindLookup lookup) noexcept;

} // namespace transmute::fx::host
