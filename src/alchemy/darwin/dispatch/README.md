# libdispatch

Queues (serial, concurrent, main, global; suspension, specifics,
initially-inactive and QoS attributes, workloops as serial queues), target-queue
hierarchies (a queue targeting a private queue runs its work as work items of
that queue, so a serial target serializes everything that targets it;
`dispatch_get_specific` searches the hierarchy), groups, sources (timer, data,
read/write, vnode, process, signal, memory pressure, and Mach send/receive when
the Mach provider is installed), semaphores, `dispatch_apply`, deadlines,
dispatch blocks (`dispatch_block_create`, `_wait`, `_notify`, `_cancel`,
`DISPATCH_BLOCK_BARRIER`), `dispatch_assert_queue*`, `dispatch_async_and_wait*`
and finalizers (`dispatch.cpp`, alchemy_core). Queue work runs on pooled guest
worker threads. `dispatch_once` is `libsystem/misc/libsystem_misc.cpp`'s.

* **GUI** (alchemy-fx): dispatch data objects (`dispatch_data.cpp`) and
  dispatch objects as Objective-C objects plus I/O channels
  (`dispatch_objects.cpp`). They add reference handlers to the provider
  (`add_dispatch_reference_handler`, `include/alchemy/darwin_dispatch.hpp`)
  instead of registering `dispatch_retain`/`dispatch_release` again: the
  provider owns both exports.
* **Host dependencies:** host threads and condition variables (through the
  guest thread pool), and the steady and system clocks.
* **Unsupported:** other source types (`dispatch_source_create` returns
  NULL). QoS classes, autorelease frequencies, workloop workgroups and QoS
  floors are accepted as scheduling hints only. `dispatch_async_and_wait` runs
  its work as `dispatch_sync` does. Signal sources see the signals libSystem
  delivers to the process (kill/raise to itself). I/O channels hand data over
  as it is read (`dispatch_io_set_interval` does not pace deliveries).
* **Tests:** `tests/alchemy_dispatch_tests.cpp` (repository root);
  `tests/dispatch_api_tests.cpp` (ctest `alchemy_gui_dispatch_api`), and the
  GUI pieces in the QLab runtime and export-fill tests.
