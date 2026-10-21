# Alchemy requirements for Transmute

Alchemy uses the existing format-neutral engine, checked memory, synthetic
calls, callbacks, shared address spaces, and worker creation. This expansion
does not edit Transmute's execution or Windows code. Engine requests go to the
separately owned Transmute task with a reproducing workload.

## Current Python execution gate

The next gate is bundled `_ctypes` callback creation. Its libffi requests
`vm_allocate(self, &address, 32768, VM_FLAGS_ANYWHERE)`, then replaces the
second 16 KiB with `vm_remap` of the loaded `__TEXT` trampoline page, using
`copy=false`, `VM_FLAGS_OVERWRITE` and `VM_INHERIT_SHARE`. Each trampoline
uses a PC-relative address 16 KiB earlier to find its writable configuration.

Alchemy needs a checked shared-backing alias operation with source range,
destination overwrite and subrange splitting. Aliases must retain backing after
source unmap, preserve protections and participate in JIT/write invalidation.
Range protection and unmapping are also required for existing mappings.
The current public engine API cannot express these operations. Copying code
into a new mapping would not implement the requested shared alias semantics.
Ordinary ctypes imports and native function calls are separate gates and can
proceed while callback support waits for the engine contract.

The original-Python callback regression reaches this request at synthetic call
`0x710000002500`, run step 44,260,292 in the 72-group Release suite. The independent
Mach VM unit verifies that a valid request leaves the output words and destination
mappings unchanged before raising the capability diagnostic. The ordinary ctypes
call workload already passes for both launchers, interpreter/JIT and both load bases.

```powershell
ctest --test-dir build/alchemy-expansion -C Release -R '^alchemy_python_standalone_ctypes$' --output-on-failure
```

The interface references are public XNU
[Mach VM declarations](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/mach/vm_map.defs),
[return codes](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/mach/kern_return.h)
and [VM flags](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/mach/vm_statistics.h).
Upstream [libffi closure code](https://github.com/libffi/libffi/blob/v3.4.4/src/closures.c)
documents the public alias scheme without identifying the bundle's libffi
version. Alchemy's implementation is independently authored from these
interfaces.

The RCpc instruction and configurable-stack APIs are now supplied by
Transmute. Alchemy passes the original `LC_MAIN` request to `ExecutionStart`
and pthread stack attributes to `fork_thread`. The unmodified bundled
`python3.11` prints its version and executes Python commands. The embedding
thread workload passes interpreter/JIT and both load bases, including isolated
Python thread-local values and GIL transitions. Separate Alchemy tests check
16 MiB stacks and default worker attributes independently of the main stack.

The compression workload next exposed `crc32x w8, w8, x13`, opcode
`0x9acd4d08`. The bundled zlib extension reaches it at guest PC `0x2000561ec`
in the embedding compression workload, corresponding to file address `0x61ec`
in `_crc32_z`. Transmute now supplies the CRC instructions. No guest instruction
or external binary is patched.

The authored dependency-free `python_crc_probe.s` isolates that exact word.
It processes the eight bytes `12345678`, complements the result and compares
against IEEE CRC-32 `0x9ae0daaf`. Both commands now return 0:

```powershell
cmake --build build/alchemy-expansion --config Release --target alchemy_python_engine_probe
build/alchemy-expansion/Release/alchemy.exe build/alchemy-expansion/macos/python_crc_probe
build/alchemy-expansion/Release/alchemy.exe --jit build/alchemy-expansion/macos/python_crc_probe
```

The reproducer initially stopped at step 6, PC `0x1000002a4`. The passing JIT
probe executes 11 translated guest instructions in two blocks with one fallback
instruction. Complete compression and Python worker workloads now pass for both
the embedding launcher and original bundled executable, in interpreter/JIT and
at both load bases. The final Release suite passed all 58 groups in 100.30
seconds before the networking/ctypes/NumPy wave. The expanded suite passes 70 of
72 groups; its only failures are the two enabled ctypes callback gates awaiting
shared aliases. Guest `_longjmp`, also reported by doctor, needs an engine
context-restoration contract before Alchemy can support that operation. The
historical observations below retain their original evidence.

| Capability | Evidence and current treatment |
| --- | --- |
| ARM64 RCpc acquire loads | The first independently built LLVM ABI used `LDAPR x0,[x8]`, opcode `0xf8bfc100`, in `std::get_unexpected`. The interpreter rejected it at guest PC `0x20002a9b8`. The authored LLVM build now explicitly selects generic ARMv8-A and emits supported `LDAR`; stock macOS binaries still need RCpc instruction coverage. |
| Partial range protection/unmapping | Whole 16 KiB-rounded private or anonymous mappings work. Darwin partial `mprotect`/`munmap` cannot be expressed by the current whole-region API and fail with named adapter requirements. |
| Fault-backed file mappings and shared mappings | Current file maps are checked private copies with EOF-tail zero filling. Shared coherence and access faults beyond valid file pages need an engine memory contract before Alchemy can implement them. |
| Thread termination through nested callbacks | Direct `pthread_exit` preserves a 64-bit result and runs cleanup. Exiting from a guest initializer or once callback requires terminating the enclosing execution without resuming the callback caller. The adapter rejects that case explicitly. |
| Cooperative worker stopping | Alchemy closes its creation gate and wakes managed waits at process exit. A CPU-bound worker stops only at its instruction limit. Process exit must eventually be able to interrupt arbitrary guest execution safely. |
| Thread stack attributes | Default workers use existing engine stack allocation. Nondefault Darwin pthread stack attributes fail explicitly until stack ownership and bounds are configurable through an engine contract. |
| Configurable main stack | Blender's bundled CPython 3.11 executable requests 16 MiB in `LC_MAIN`; the current format-neutral engine provides 1 MiB. `alchemy .../python/bin/python3.11 --version` fails before startup with `LC_MAIN requested stack size 16777216`. Add a public startup stack-size contract, bounds that callbacks honor, and checked allocation/lifetime behavior. Alchemy must continue validating the original request. |
| CPython worker stack | The bundled non-Apple `include/python3.11/pyconfig.h` defines `THREAD_STACK_SIZE` as `0x1000000`. Python-created pthread workers therefore also require configurable 16 MiB stacks. Default Alchemy pthread adapter tests do not certify this guest configuration. |
| CPython RCpc acquire load | The authored embedding launcher linked to bundled CPython 3.11.15 reaches `ldapr w8, [x8]`, opcode `0xb8bfc108`, at guest PC `0x100122a7c` in `_PyEval_FiniGIL`; interpreter execution rejects it. This is the 32-bit form of the existing LLVM RCpc requirement. `tests/fixtures/macos/python_rcpc_probe.s` reproduces it without CPython or system imports. Preserve acquire ordering in both engines; do not patch guest code or relax it to an unordered load. |

The official Blender executable requests a 1 MiB `LC_MAIN` stack. The current
engine accommodates it. Doctor reports larger requested stacks as requirements;
the loader rejects a request that exceeds the actual engine bounds.

Guest exceptions remain inside ARM64 frames. The cross-dylib test uses real
LLVM libunwind save/restore assembly and compact unwind information. Unwinding
across Windows service frames is outside this profile. Thread-pointer register
support has not been requested because the selected runtime uses Mach-O TLV
descriptors and pthread keys.

Instruction limits currently apply to each engine invocation. Callback APIs
return a guest value without a complete interpreted-instruction total, so
Alchemy reports the engine's actual JIT counters and does not invent callback
counts. A future process-wide instruction budget needs an engine accounting
contract; it is not required for the passing execution gates in this milestone.

Earlier native JIT crashes occurred during concurrent Transmute source/header
changes. A fresh core rebuild cleared them; they are not an open Alchemy engine
requirement. The exception bring-up also exposed duplicate dependency ordinal
handling and an invalid guest unwind build, both repaired inside Alchemy.

## Regression gate during object-profile integration

The October 1 Objective-C/CoreFoundation integration passed all 31 Alchemy
groups, including interpreter/JIT and both load bases. An earlier complete
snapshot passed all 35 groups. On the final shared-checkout build, after
concurrent Transmute source updates, the Windows shard `transmute_core_tests_2`
failed `test_guest_opengl_builtin_window`:

```text
native helper pointers escaped into a guest message
```

The isolated reproduction also fails:

```powershell
build\alchemy-expansion\Release\transmute_tests.exe --test test_guest_opengl_builtin_window
```

This executable links `transmute_core`, without Alchemy, and tests native
Windows helper-message filtering in a built-in OpenGL window. The Alchemy
workers did not edit its implementation or assertions. At observation time,
`win32_bridge.cpp`, `jit_x64_windows.cpp`, and `tests/win32_gui_tests.inc` had
concurrent edits in the shared checkout; the latter two continued changing
after the tested binary was built. The cause is not assigned from timestamps.
The Transmute owner needs to resolve and rerun this gate. No Alchemy behavior
depends on Windows windowing, and the failing test remains enabled.

During the subsequent Python-support integration, a fresh shared-checkout
Release build passed all four Windows shards along with the 34 runnable
Alchemy groups. The previous OpenGL helper-message failure no longer reproduces
in that tested snapshot. CPython's configurable-stack and RCpc requirements
remain separate open execution gates.

The final Python integration ran 42 groups: 39 passed, including all four
Windows shards and the four-mode real CPython archive version gate. The three
optional Python script/extension/compression groups remain enabled and fail at
the recorded `0xb8bfc108` instruction. Their deeper workload assertions have
not run.

The dependency follow-up ran 46 groups in 32.66 seconds: 43 passed, including
all Windows shards and the new threaded Foundation/dispatch workloads. The same
three Python gates remain blocked at the recorded RCpc load. No Transmute
source or engine tests were changed by the Alchemy workers.
