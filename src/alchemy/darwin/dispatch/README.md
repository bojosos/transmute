# libdispatch

Queues (serial, concurrent, main, global; targets, suspension, specifics),
groups, sources (timer, data, read/write, vnode, process, memory pressure, and
Mach send/receive when the Mach provider is installed), semaphores,
`dispatch_apply` and deadlines (`dispatch.cpp`, alchemy_core). Queue work runs
on pooled guest worker threads. `dispatch_once` is
`libsystem/misc/libsystem_misc.cpp`'s.

* **GUI** (alchemy-fx): dispatch data objects (`dispatch_data.cpp`) and
  dispatch objects as Objective-C objects plus I/O channels
  (`dispatch_objects.cpp`). They add reference handlers to the provider
  (`add_dispatch_reference_handler`, `include/alchemy/darwin_dispatch.hpp`)
  instead of registering `dispatch_retain`/`dispatch_release` again: the
  provider owns both exports.
* **Host dependencies:** host threads and condition variables (through the
  guest thread pool), and the steady and system clocks.
* **Unsupported:** other source types (`dispatch_source_create` returns
  NULL). QoS classes are accepted as attributes only.
* **Tests:** `tests/alchemy_dispatch_tests.cpp` (repository root); the GUI
  pieces are exercised by the QLab runtime and export-fill tests.
