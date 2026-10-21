# src/fx/host: the fx engine's host seams

The only fx code that talks to the operating system. `host.hpp` has the
rules; `docs/FX_HOST_SEAMS.md` lists every host dependency of fx, which
ones go through here and which are left in place.

* **Areas:** `sync` (RwLock, CondVar), `time`, `thread` (threads, TEB,
  activation contexts, the thread exit hook, the thread pool), `process`,
  `vm` (reservations, protection, placeholders and sections, the guest heap),
  `fault` (the first-chance fault handler and the interrupted CPU context),
  `guard` (memory accesses that may fault), `unwind` (unwind lookup for the
  code arena), `env`, `file` (the translation cache's files), `text`,
  `sample` (diagnostic thread sampling) and `diagnostics` (backend-owned
  diagnostics: core-type statistics).
* **Backends:** `win32/` only. `TRANSMUTE_FX_HOST` (CMake, `component.cmake`)
  selects one; `TRANSMUTE_FX_HOST_<BACKEND>=1` picks its inline headers.
* **Hot calls are inline** in `<backend>/*.hpp`; `win32/kernel32.hpp`
  declares the few kernel32 functions they need exactly as the SDK does, so
  fx headers stay free of `windows.h`.
* **Adding a call:** put it in its area's header, implement it in every
  backend, and test it in `tests/host_tests.cpp` (behaviour every backend
  has) or `tests/host_win32_tests.cpp` (agreement with the Win32 API).
* **Not here:** the Win64 calling convention of generated code and the PE
  guest personality (`loader/`, `abi/`, `seh/` layouts); see the document.
