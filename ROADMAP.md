# Transmute and Alchemy implementation roadmap

The target product runs installed ARM64 Windows applications on x64 Windows 11
through one command such as `transmute installer_arm64.exe`. A release is
complete only when installation, later launch, updating, repair, and removal
work for the certified application suite. Alchemy supplies a separate macOS
application personality over the shared Transmute execution engine.

## Current state

Alchemy's first CLI milestone runs the unmodified user-provided ARM64 Mach-O
hello-world program in interpreter and JIT modes. It adds validated Mach-O
loading, chained fixups, guest dylib exports and initializers, replacement
`printf`/`puts`, Darwin startup vectors, and raw `write`/`exit` system calls.
Project-owned fixtures require no Apple SDK. The existing Windows suite and
the new Alchemy suites pass together. [Alchemy](docs/ALCHEMY.md) records the
supported profile, provenance, and later macOS compatibility milestones.

The repository contains a PE32+ loader with DIR64 base relocations, named and
ordinal guest DLL function and data exports and imports, initial-thread static
TLS setup, TLS callbacks, process-attach calls, and native import resolution;
an in-house ARM64 reference interpreter; a command-line entry point;
malformed-image checks; and tests that compile and run actual ARM64 Windows PEs.
Selected Win32/CRT families cover file I/O, environment, locale, heap,
synchronization, and process queries. The reference engine implements tested
AdvSIMD, floating-point, exclusive-access, and LSE atomic instruction families;
this is not complete ISA or multithreaded correctness coverage. The staged
Blender startup now passes CRT `_beginthreadex`, shell known-folder lookup,
the vectorized CRT string path, and wide CRT file-open/locking. An extended
probe passes CRT numeric parsing, secure random generation, and scalar
fixed-point integer-to-floating-point conversion, file enumeration, and
process DPI setup. Shell application identity calls now have a focused bridge
test. The latest probe passes keyboard layout, OLE apartment setup, and common
controls initialization and Blender's named icon resource. The latest GUI and
headless Classroom probes pass window-class registration, GDI stock-object
setup, an OpenImageIO worker `YIELD` hint, and guest-memory CRT `qsort` with
ARM64 comparator callbacks. The headless probe prints Blender's 4.5.14 banner
and passes special-folder lookup and CRT FP control. A Blender stack-alignment
fault was traced to logical-immediate instructions with SP as destination and
fixed with a regression using the actual instruction sequence. The headless
probe now reaches the `XPACLRI` pointer-authentication hint in
`VCRUNTIME140.dll` after about 147.4 million guest instructions. That hint is
handled under the current PAuth-disabled policy; the headless path now reaches
CRT `_mktemp_s` at 192.4 million instructions. The GUI path reaches
`ole32.dll!CoCreateInstance` at about 206 million. With CRT temporary-name
support added, the headless path reaches `KERNEL32.dll!CreateDirectoryW` at
192.4 million. With directory/path and drive-enumeration support, it reaches
`SHELL32.dll!SHGetDesktopFolder` at about 192.45 million guest instructions.
The desktop-folder COM path now has a typed bridge. A subsequent headless probe
passes it and reaches `ole32.dll!CoCreateInstance` for Shell automation and
Quick Access folder enumeration. A typed automation proxy now passes the
observed chain. A guest-owned CRT environment bridge passes `_wputenv_s`;
the next headless stop was `bcrypt.dll!BCryptGenRandom` at about 193.31 million
guest instructions. A typed CNG random/provider bridge now passes that path;
the next stop was CRT `__fpe_flt_rounds` at about 198.16 million. Guest FPCR
rounding-mode mapping passes that path; a case-insensitive CRT string bridge
passes `_wcsicmp`, the PathCch utility family, and Win32 time-zone queries.
The headless path then passed `GetFileInformationByHandle`, CRT `_dclass`, and
focused `GetFullPathNameW/A` tests. A later run prints the Blender banner and
starts guest workers. The `_wopen` bridge initially caused a host CRT
invalid-parameter fail-fast; after correcting its mode handling, later
stops included UCRT `__sys_nerr` and
`_set_thread_local_invalid_parameter_handler` at about 206.62 million
instructions; their tested bridges passed. Another stop was
`KERNEL32.dll!MoveFileExW` at about 219.54 million instructions; a typed file
operation batch passed it. A later stop was `KERNEL32.dll!GetVersionExW`
at about 239.30 million. Version queries, `DC ZVA`, ninth-argument NLS ABI,
and CRT formatted-output paths now pass further staged runs. The latest
headless run reached a 500-million-instruction limit in `python311.dll` after
printing a `sitecustomize` WinError 127 warning. A typed dynamic
`AddDllDirectory` bridge cleared that warning in a 260-million-instruction
replay and a 500-million-instruction replay; neither produced a frame. A
direct ARM64 fixture using the packaged Embree DLL commits a triangle scene
and verifies scalar and
4/8/16-lane ray hit/miss, occlusion, and clear path, including Blender's
host/device geometry-buffer, traversable, and instance API path. A 64-by-64
triangle grid with a progress callback also passes through the in-house JIT.
This does not prove the Cycles workload. A deeper interpreter probe reached
`KERNEL32.dll!CreatePipe` at 3.14 billion guest instructions and stopped on
that unsupported import. Its tested pipe bridge is now present; a replay took
a different path and stopped at `InitializeProcThreadAttributeList` after
1.22 billion instructions. The GUI COM request is for the Windows audio device
enumerator interface. A subsequent replay passed registry and Winsock startup,
opened `classroom.blend`, and read its linked asset libraries. It stopped at
CRT `feclearexcept` during render setup after 1.888 billion guest instructions.
The tiny factory Cube Cycles probe now saves a 16 × 16, one-sample PNG whose
decoded pixels match native x64 exactly. Its 718-second process time is
unacceptable; file buffering, memory access and block-dispatch performance are
under validation. A full ARM64 Classroom frame remains incomplete. See
`docs/BENCHMARK_STATUS.md` for measured settings and results.
The reference engine can run
initial guest workers with separate CPU state, stacks, TEB, static TLS, and CRT
errno over a synchronized shared address space. Runtime DLL fixture tests now
cover transitive imports, TLS and `DllMain` process attach, and rollback on
failed attach. Worker DLL/TLS thread attach/detach, loader serialization and
resource reclamation now pass focused lifecycle fixtures. Broader reentrant
loading, complete multithreaded bridge safety,
exceptions, and broad system DLL coverage remain open. The command reports
unsupported features.

## Runtime components and required order

1. **CPU contract.** Expand guest registers and status state, memory access,
   faults, interrupts, atomic operations, and instruction-level state recovery.
   Keep the reference interpreter as the correctness oracle. The first opt-in
   in-house x64 block translator compiles a narrow integer subset and falls
   back to the interpreter. It is faster on a small synthetic loop, but has
   not demonstrated acceptable Blender performance. Expand it to reached basic blocks,
   promote hot paths, and store reusable per-module translations. Cache entries must be
   keyed by module content, translator version, guest ISA policy, and host CPU
   features; relocation and writable executable pages must invalidate or
   rebind cached code safely. An optional install-time pass can warm the cache,
   but runtime translation must remain for plugins and generated code. Add
   SVE/SVE2 vectors and predicates before calling the Windows release complete.
2. **Windows process.** Implement the full PE loader: relocations, imports,
   API sets, TLS, resources, delay loading, DLL lifetime, guest PEB/TEB,
   virtual memory, threads, handles, synchronization, and ARM64 exceptions.
   Track executable guest pages and invalidate translated code after writes.
3. **Native bridges.** Generate typed ARM64-to-x64 Windows API calls from
   Win32 metadata and IDL, with reviewed handling for structures, pointers,
   callbacks, variadic calls, COM identity, and asynchronous operations. Guest
   imports resolve to Transmute-owned ARM64 bridge DLLs. Never call a native
   API with guessed argument layout.
4. **Install and launch.** Route child processes and elevation through a
   managed installation session. A first ARM64 parent-to-child fixture now
   passes quoted arguments, Unicode environment, inherited pipe output,
   wait, and exit status; installer orchestration remains unbuilt. Adapt
   Wine's MSI engine as a private x64
   component; run ARM64 custom actions in Transmute workers. Store package
   ownership separately from native MSI. Register x64 launchers for shortcuts,
   services, file associations, scheduled tasks, and removal commands.
5. **Product integration.** Add graphics and multimedia bridges, an application
   manager, repair and uninstall commands, signed runtime releases, and a
   compatibility report that names the failing module/API/instruction.

The Windows system boundary matters: ARM64 drivers cannot run in an x64 kernel.
MSIX package identity and Store deployment are a separate later product track.

## Acceptance suite

Certify real installation and normal workflows for ARM64 builds of 7-Zip,
Python, Node.js, .NET, VS Code, SQLite, FFmpeg, and D3D11/D3D12, OpenGL, and
Vulkan fixtures. Test installer rollback, repair, upgrades, reboot recovery,
and uninstall. Compare results with Windows-on-ARM reference hardware.

Measure the fixed suite against equivalent native x64 workloads on the same
machine. Release targets are 80% CPU throughput across the suite and 90% for
GPU-bound workloads, with per-application results published. Correctness is a
separate mandatory gate. These numbers are targets, not current results.

For Blender, the nearer gates are concrete: load every packaged dependency,
exercise each Embree API imported by `blender.exe` with matching native and
translated fixtures, then load the Classroom scene and finish a full Cycles
frame. Expand the instruction suite by encoding family against the reference
interpreter and real ARM64 results. A static opcode inventory can prioritize
work, but it cannot prove that unexecuted instructions or feature combinations
are correct.

## Future ARM64 macOS application support on x64 Windows

The CPU and guest-memory contracts must contain no Windows-specific types.
Keep binary loading, process services, system calls, and imported libraries
behind an operating-system personality interface. The current Windows PE and
Win32 implementation is one personality; a future macOS personality on the
same x64 Windows host must load Mach-O, implement dyld behavior, and provide
Darwin process and file semantics. It must also bridge libSystem,
Objective-C/Swift runtimes, Apple frameworks, and graphics, without relying
on macOS libraries being present on Windows. This is a separate compatibility
project, not a small extension of the PE loader. First prove a command-line
Mach-O program with file I/O, then a Cocoa window, then package installation
and application workflows. Windows ARM64 and macOS ARM64 have separate release
gates; Blender on Windows remains the current target.
