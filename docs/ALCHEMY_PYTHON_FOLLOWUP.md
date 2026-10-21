# Python dependency follow-up

This earlier wave continued independently testable Alchemy work while guest
Python initialization was blocked by Transmute's RCpc load and 16 MiB stack
requirements. The [subsequent execution milestone](ALCHEMY_PYTHON.md) now uses
the supplied instruction/stack APIs and runs the original bundled executable.
This follow-up's implementations did not change engine instruction, stack, JIT,
memory or Windows bridge code.

## Parallel work

| Owner | Files and behavior | Gate |
| --- | --- | --- |
| C runtime worker | `darwin_support.cpp`, runtime-helper tests | Fortified bounds, string contracts, guest comparator callbacks and ARM64 float ABI |
| Darwin file worker | `darwin_services.cpp`, file-service tests | Exclusive temporary creation, descriptor lifetime and advisory file locks |
| Foundation worker | `foundation_bridge.cpp`, declarations and separate tests | Process metadata, per-thread autorelease pools and checked logging |
| Integration owner | Process arguments, bounded dispatch module, build/test registration | Worker semaphores, deadlines, interpreter/JIT agreement and existing regression suite |

New process arguments are available before guest module constructors run and
include the guest executable path as `argv[0]`. Foundation returns guest objects
through existing CF ownership and Objective-C dispatch. No host runtime object
or argument list enters guest memory.

## C and file behavior

Fortified memory/string helpers validate declared bounds and guest mappings
before writes. Counted string operations retain their padding, truncation and
NUL rules. Case comparisons and their locale variants use C/POSIX behavior;
freed locale handles fail. Binary search invokes guest comparators without
metadata locks and allows reentrant allocation. Float32 helpers use S0/S1 and
32-bit output cells. Long-double aliases reuse binary64 functions because the
Apple ARM64 ABI makes those types identical. Floating-environment behavior and
the broader complex-math library remain separate work.

Temporary files/directories use exclusive creation, cryptographic names and
0600/0700 guest permissions. Windows creates them with protected owner ACLs.
Guest templates are checked for writing before any host object is created.
Anonymous `tmpfile` storage stays alive through duplicated descriptors and is
removed on last close. Unsupported host deletion facilities fail explicitly.
Directory streams own their descriptors; `dirfd` returns that guest descriptor.

Windows `flock` uses a cooperative Alchemy advisory protocol. Persistent
`%TEMP%/alchemy-flock-v1/<volume-and-file-id>.lock` files hold Windows kernel
range locks, so rename and hardlinks preserve lock identity without making
ordinary data I/O mandatory. Duplicates share a lock owner; independent Alchemy
processes contend on the same identity. Other host programs participate only
if they implement this protocol. Rendezvous files remain resident to prevent
deleting a live lock identity. Blocking waits release descriptor metadata locks
and observe close/shutdown. Sleep helpers observe shutdown too. Actual signal
delivery remains unsupported; cancellation reports `ECANCELED`.

## Foundation behavior

`NSProcessInfo` snapshots launch arguments and environment, keeping the first
duplicate environment value like `getenv`. It shares the Darwin process-ID and
CPU-count backends. Its OS version is explicitly the Alchemy 14.0.0 profile
(`darwin_version.hpp`, shared with sysctl and the availability checks);
24-byte structure returns and indirect structure arguments use the guest ABI.

`NSAutoreleasePool` uses the existing per-thread pool stack. Draining requires
the owning thread and nesting order; retain/autorelease requests fail explicitly.
`NSLog` and `NSLogv` support literals, `%%`, `%@`, `%s`, `%d`, `%ld` and `%lld`.
They decode checked guest stack slots and dispatch object descriptions through
Objective-C. Unsupported conversions fail before output. Logging writes the
formatted bytes to stderr and appends a newline when needed; this subset adds
no system-log timestamps or prefixes. Partial output and backend errors use the
shared output contract.

## Dispatch profile

The independently authored dispatch subset provides counting semaphores,
retain/release, `dispatch_time` and `dispatch_walltime`. It also recognizes the
`/usr/lib/system/libdispatch.dylib` provider identity. Existing `dispatch_once_f`
continues to use Darwin thread initialization. Queues, groups, sources, Blocks,
Objective-C dispatch-object bridging and dispatch finalizers are deferred.

Deadlines are opaque guest values. Monotonic time uses the same nanosecond clock
as Alchemy's `mach_absolute_time`; wall deadlines use the Unix wall clock.
Timeouts do not consume permits. A signal hands a permit to one queued waiter
or increments the available count. Metadata locks are released before waits.
Invalid handles, reference/count overflow and releasing a semaphore with active
waiters produce explicit diagnostics. Retained handles remain live until their
last release. Host condition variables never appear in guest object storage.

Waiting observes process shutdown. If it needs to interrupt guest execution,
it reports the existing Transmute cooperative-stop requirement. It does not
silently convert shutdown into a successful wait.

The authored C semaphore workload starts a guest pthread, transfers a value
through semaphore synchronization and joins the worker. Acceptance runs in
interpreter/JIT modes at both load bases and requires translated instructions.
Unit tests check reference lifetime, permit accounting, finite/wall deadlines,
overflow, concurrent waiters and shutdown diagnostics.

## Verification

The six new/expanded groups passed after the integrated Release build. The
threaded semaphore and Foundation workloads pass interpreter/JIT execution at
both load bases, with exact output, empty guest stderr and exit zero. Direct
JIT runs executed 143 and 1,785 native guest instructions respectively.

The full audit with the existing LLVM runtime directory inspected all 163
native images without parsing failures. Eighty-three graphs have resolvable
static imports. The previous 75-graph audit omitted that runtime directory, so
the counts describe different provider configurations. All 376 previously
flagged C++ symbol names are available in the supplied guest LLVM runtimes.
These are import checks, not ABI or execution certification. Reports are in
`artifacts/alchemy-python/audit-continuation-full`; the focused nine-image
report is in `artifacts/alchemy-python/audit-continuation`.

The focused report identifies six Mach VM/dyld names still required by
`_ctypes`, 39 networking names for `_socket`, terminal libraries for `readline`
and Accelerate/math dependencies for NumPy. Frameworks, networking, terminal
editing and Mach remapping are not claimed by this wave.

The final Release build passed after the file-lifetime corrections. The complete
suite ran all 46 groups in 32.66 seconds: 43 passed, including all four Windows
shards, the external sample, existing C++/object workloads and the real CPython
version gate. The three real Python script/extension/compression tests remain
enabled and fail at opcode `0xb8bfc108`, PC `0x100122a7c`. Their Python assertions
have not executed. No failing gate was disabled or marked as an expected failure.

Final file tests exhaust the declared descriptor limit, check that failed
temporary/stream/directory creation leaves no host objects, verify recovery,
and preserve ownership through `dup2` and failed `fdopen`. Descriptor removal
precedes backend close, so an old close cannot erase a replacement descriptor.
Failed stream construction releases its guest allocation; `fopen` closes the
new descriptor on failure while `fdopen` keeps the caller's descriptor alive.

The original Python executable/archive hashes remain unchanged. Their stack
and RCpc requirements remain separately owned engine gates.

## Provenance

All new replacements, declarations and fixtures are independently authored
under the project MIT license. No Apple SDK payloads or implementations are
copied. Public interface references for the bounded dispatch behavior are
[counting semaphore contracts](https://github.com/swiftlang/swift-corelibs-libdispatch/blob/main/dispatch/semaphore.h),
[opaque deadline contracts](https://github.com/swiftlang/swift-corelibs-libdispatch/blob/main/dispatch/time.h)
and [dispatch ownership](https://github.com/swiftlang/swift-corelibs-libdispatch/blob/main/dispatch/object.h).
The local CPython inputs and existing LLVM guest runtimes retain their own
license notices and remain outside redistributed fixtures.
