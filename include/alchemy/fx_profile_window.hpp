#pragma once

// alchemy-fx: measurement window of the fx sampling profiler
// (TRANSMUTE_FX_SAMPLE, docs/FX_ENGINE.md "Profiling") tied to presented
// frames, so a profile of an interactive phase leaves out startup.
//
//   ALCHEMY_FX_PROFILE_WINDOW=first-frame[,<seconds>]
//       at the first presented frame: drop the samples so far and sample
//       (fx::profiler_reset + profiler_set_enabled(true)); with <seconds>,
//       stop sampling at the first frame presented that long afterwards.
//   ALCHEMY_FX_PROFILE_WINDOW=period=<seconds>
//       restart the window at the first frame and then every <seconds>
//       (at a presented frame), so the report covers the latest period.
//
// Combine with TRANSMUTE_FX_SAMPLE=<file>,...,paused to sample nothing
// before the first frame. Implemented in src/alchemy/fx/fx_profile_window.cpp
// (alchemy-fx only; no-ops elsewhere and when the variable is unset).
// ALCHEMY_PERF (src/alchemy/perf.cpp) may call frame_presented from its own
// frame hook instead of the presenter.

namespace alchemy::fx_profile_window {

// Called for every presented frame (any thread). One relaxed load when off.
void frame_presented() noexcept;

} // namespace alchemy::fx_profile_window
