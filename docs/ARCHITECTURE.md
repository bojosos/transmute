# Translator and operating-system boundaries

Transmute's current reference interpreter is an in-house correctness baseline,
not the production execution engine. The production translator will also be
built in-house. A binary translation library is not part of the design.

The engine has four contracts that both Windows and a later macOS personality
will use:

1. **Guest CPU state:** all A64 integer and SIMD registers, PC, SP, NZCV,
   FPCR/FPSR, thread-local registers, and the negotiated ISA features. SVE
   vector length, predicates, and system-register behavior must be explicit.
2. **Guest address space:** mapped pages, permissions, byte-order-correct
   reads and writes, atomic operations, faults, and executable-page change
   notifications. The current `GuestMemory` byte interface is only the first
   slice of this contract.
3. **Execution events:** calls into imported libraries, system calls,
   exceptions, callbacks, and thread transitions leave translated code through
   typed events. Neither the decoder nor generated x64 code calls Win32 or
   Darwin functions directly.
4. **Translation cache:** guest module identity, guest address and page
   generation, translator version, ISA policy, and host CPU features identify
   reusable translations. Relocation-sensitive addresses are rebound on load;
   writes to executable pages invalidate affected blocks before reuse.

The first fast engine decodes reached basic blocks into a typed intermediate
representation, lowers that representation to x64, and retains blocks in a
process-local cache. Hot traces can be optimized after correctness is proven
against the reference interpreter. Persisted per-module cache files and an
optional installer prewarm pass come later. Runtime compilation remains
necessary for plugins, dynamic libraries, and generated code. Persist only
code whose module identity and page contents can be verified on the next run.

## AOT, JIT and reusable translations

Use one in-house decoder, IR and x64 backend for both runtime translation and
an ahead-of-time prewarm pass. AOT and persistent caching reduce repeated
translation work; larger optimized blocks and cheaper memory/API boundaries
reduce execution time. These are separate performance measurements. The first
successful tiny Blender run spent 4.211 of its 718.181 seconds compiling, so
avoiding compilation alone cannot make that workload fast.

Implement the reusable path in this order:

1. Record reached block starts as module-relative offsets. Bind the record to
   a digest of the original module and relevant executable bytes, with a
   versioned translator/IR ABI, guest ISA policy and host feature requirements.
   Keep PE and future Mach-O identity and relocation handling in their loaders.
2. Persist verified block records and relocatable translated code. Code must
   carry explicit fixups for guest addresses, branch targets and runtime
   helpers; the current emitter embeds absolute guest PCs and is not a
   serializable executable format. Never save process pointers or treat a
   previous process's page generation as proof of current bytes. Load a record
   only after validation, apply fixups to writable memory, then publish RX
   code. Reject truncated, stale, incompatible or corrupt records and use JIT.
3. Add an optional installation/background prewarm pass. Seed discovery from
   verified entry points, exports, loader function metadata and previously
   reached blocks, then follow supported control flow. Executable sections can
   contain data; blindly translating every aligned word does not establish
   instruction boundaries or discover every indirect target. Unreached DLLs,
   plugins, generated code and modified pages still use runtime translation.
4. Use observed hot paths to compile larger regions and retain guest registers
   between operations. Preserve fault PCs, callback/import exits, instruction
   budgets, thread fairness and executable-write invalidation. AOT should use
   these same optimizations once their runtime behavior is verified.

Acceptance checks include a second process reusing translations, changed
module bases, changed modules and host features, corrupted/truncated caches,
executable-page writes, concurrent cache publication and instruction-budget
exits. Report cold launch, warm launch and steady-state render time separately,
with identical decoded output. Persistent translations and AOT are planned;
the current implemented cache remains process-local.

Apple documents both JIT page translation and stored AOT artifacts in
[Rosetta](https://support.apple.com/guide/security/rosetta-2-on-a-mac-with-apple-silicon-secebb113be1/web).
Microsoft documents optimizing JIT blocks and a service that caches translations
per module in
[Windows emulation](https://learn.microsoft.com/en-us/windows/arm/apps-on-arm-x86-emulation).
Neither description implies that caching alone supplies native execution speed.

## Execution and operating-system services

The execution engine must preserve guest-visible instruction boundaries for
faults and asynchronous events. Memory ordering, exclusive accesses, atomics,
signals/exceptions, and self-modifying code are correctness gates, not optional
speed features. Performance work follows differential tests against real
ARM64 hardware and application workloads, with native x64 runs on the same
machine as the comparison point.

The Windows personality owns PE loading, DLL and API-set resolution, TLS,
Win32/COM/graphics bridges, handles, threads, process creation, and EXE/MSI
installation. It translates guest calling conventions and structures at each
native boundary. The bridge may access guest memory only through checked
guest-memory operations. Guest addresses that a host API would dereference
require validated host backing or a converted buffer.

Eligible scalar Win32 imports can bind a native wrapper once at module
registration. The initial implementation covers eleven ID, clock, error-mode,
code-page and opaque pointer encoding APIs. The wrapper reads ARM argument
registers and writes the return register; the C++ compiler supplies the host
calling convention and unwind information. `EncodePointer` and `DecodePointer`
operate on opaque bits without dereferencing them. Memory, callback and
guest-TEB APIs retain their typed bridge. These bindings bypass the general
Win32 dispatcher, but guest calls still leave translated execution through
the engine's import boundary. Inlining a host call into translated code is
separate work with its own state, callback and exception requirements.

Guest workers already run as native Windows threads. An execution gate pins
guest backing while translated instructions use host addresses. Concurrent
paths permit cached scalar native memory regions, single-element
LSE atomics and pure register blocks to share that gate. Guest and host
addresses must meet each operation's natural alignment; stores and atomics
require owned readable/writable non-executable backing. Dirty journals are
published before the shared lease ends. Checked operations, pair/vector
memory owners and LL/SC retain exclusive ownership. Longer shared chains
stop admitting cached invocations when a writer requests ownership; each
native invocation remains bounded. This
protects unmapping, executable invalidation and exclusive-monitor epochs
without introducing a guest scheduler. Ineligible shared accesses exit
without performing the rejected operation and retry under exclusive ownership.

Windows synchronization objects need per-process lifetime tracking. A guest
critical-section pointer cannot be passed to a host API: it addresses guest
memory, while the host object contains host-only pointers. Windows requires
applications to treat the structure as opaque and use its lifecycle functions.
The personality therefore owns a host object for each guest address and
mirrors guest-visible fields after API calls. Thread exit, DLL detach, and
direct `ntdll` entry points need the same ownership model as support expands.

Alchemy targets **ARM64 macOS applications on x64 Windows**. Its first CLI
milestone implements Mach-O loading and initial dyld behavior. It will expand
Darwin-style process services, libSystem and framework compatibility,
Objective-C/Swift runtime
integration, application bundles, DMG/PKG installation, and a graphics path
for Metal workloads. The x64 Windows host cannot supply Apple's system
libraries, so these services require replacement implementations or explicit
translation to host facilities. Kernel extensions and hardware drivers are
outside a user-space compatibility layer. macOS support has its own
compatibility and performance gates and does not delay the current Windows
Blender target.

The engine retains its PE registration interface and also accepts a
format-neutral `ExecutionStart` with a guest entry address. Alchemy maps Mach-O
segments through checked guest memory, uses zero-backing reservations for
`__PAGEZERO`, and binds replacement library functions through synthetic guest
calls. Generic supervisor-call events leave execution leases before dispatch.
Darwin numbers, structures, errors, and loader policy remain in Alchemy. The
existing JIT can execute these mapped pages without a synthetic PE image.
Broader format-neutral module identity and persistent translations remain
future work.
