#pragma once

// fx host seams (owner: r4 / runtime): the engine's only way to the host OS.
// docs/FX_HOST_SEAMS.md lists every host dependency of fx, which of them go
// through these headers and which are left in place (with the reason).
//
// Rules:
// - Only src/fx/host/<backend>/ includes OS headers. Every other fx file uses
//   the headers in src/fx/host/.
// - One backend is compiled in, chosen by CMake (TRANSMUTE_FX_HOST=win32,
//   src/fx/host/component.cmake). There are no virtual calls: a header
//   declares free functions or small concrete classes, and the backend
//   defines them. Calls on hot paths (locks, the tick counter, guarded
//   reads, the thread environment block) are inline in the backend's header
//   (<backend>/*.hpp), so they cost what the OS call costs.
// - The seams speak fx's own vocabulary. Where fx already models the guest
//   on Windows (fault records with NTSTATUS codes, the Win32 PAGE_*
//   protection codes of process.hpp, Win64 unwind entries), the seam keeps
//   that encoding and a non-Windows backend translates it.
//
// Areas (one header each): sync, time, thread, process, vm, fault, guard,
// unwind, env, file, text, sample, diagnostics.

#if defined(TRANSMUTE_FX_HOST_WIN32)
#define TRANSMUTE_FX_HOST_BACKEND "win32"
#else
#error "fx: no host backend selected (TRANSMUTE_FX_HOST, src/fx/host/component.cmake)"
#endif

namespace transmute::fx::host {

// The backend compiled in ("win32").
inline constexpr const char* kBackend = TRANSMUTE_FX_HOST_BACKEND;

} // namespace transmute::fx::host
