#pragma once

// Host diagnostics implemented by the backend (docs/FX_HOST_SEAMS.md,
// "Diagnostics").
//
// Core-type statistics (TRANSMUTE_FX_CORESTATS=<file|1>[,<interval_us>]):
// which kind of core (performance or efficiency) each thread of the process
// runs on, for hybrid-CPU scheduling. win32: win32/corestats.cpp (CPU sets,
// special user APCs). A backend without hybrid-core information provides
// both calls as no-ops.

#include "fx/host/host.hpp"

#include <cstdint>
#include <vector>

namespace transmute::fx::host {

// Starts the statistics if TRANSMUTE_FX_CORESTATS asks for them.
// `guest_threads` (may be nullptr) lists the guest threads' ids, which the
// report tags [guest].
void corestats_start_if_requested(void (*guest_threads)(std::vector<std::uint32_t>& ids));
// Writes the final report (process exit); never blocks.
void corestats_flush();

} // namespace transmute::fx::host
