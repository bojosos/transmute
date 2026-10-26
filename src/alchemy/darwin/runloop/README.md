# Run loop

**Covers:** CFRunLoop and NSRunLoop over one run-loop engine (`runloop.cpp`):
timers, sources, observers, the main pump and the completion thread
(`include/alchemy/gui/runloop.hpp`). `wait_trace.cpp` is the
`ALCHEMY_WAIT_DUMP` blocking-wait diagnostic. The four CF type IDs
(`CFRunLoopGetTypeID` and the source, observer and timer forms) are
CFGetTypeID's answers for those objects (`cf_runloop.cpp`).

**Headless:** NSRunLoop's headless shell is `run_loop_headless.cpp`
(alchemy.exe only), with an empty `run_loop_fx.cpp`
(`darwin/foundation/headless_shells.hpp`).

**Host dependencies:** `host::ui` (the step's wait on handles and window
input, the modal-loop checks, the dialog-loop hook, the hang-dump stack
sampler), `host::sync` (the wake event), `host::time`, `host::process`.

**Behaviour note:** NSEventTrackingRunLoopMode / NSModalPanelRunLoopMode are
AppKit's string constants, not the run loop's mode objects, so guests
compare them with the current mode by value only.
