# Alchemy command-line runtime

Alchemy is an independent macOS application compatibility layer on x64
Windows. Transmute executes ARM64 instructions; Alchemy loads Mach-O programs,
resolves their dependencies, implements Darwin startup and library conventions,
and translates supported operating-system requests to host facilities.

## Run and inspect

Build with the normal Transmute CMake commands. `alchemy.exe` is placed beside
`transmute.exe` in the selected configuration directory.

```powershell
build\Release\alchemy.exe doctor C:\path\to\program
build\Release\alchemy.exe C:\path\to\program argument
build\Release\alchemy.exe --jit --load-base 0x180000000 C:\path\to\program
build\Release\alchemy.exe --jit-stats --dylib-dir C:\guest\libraries C:\path\to\program
build\Release\alchemy.exe --runtime-dir artifacts\alchemy-llvm-arm64 C:\path\to\cpp_program
build\Release\alchemy.exe doctor --deps --scan-bundle --json C:\path\to\Blender.app
build\Release\alchemy.exe install --root C:\Alchemy C:\extracted\QLab.app
scripts\alchemy_install_dmg.ps1 -Dmg C:\Downloads\QLab.dmg -Root C:\Alchemy
```

`doctor` parses and validates the image without executing guest instructions.
It lists the entry, declared dependencies, imports, pointer formats,
and minimum OS/SDK metadata. `--deps` walks the dependency graph;
`--scan-bundle` also inspects packaged plugins and Python extensions. JSON
reports explicitly state that execution has not been verified. That metadata does not establish compatibility
with every application targeting the named macOS version.

`install` copies an extracted `.app` bundle into an Alchemy root's
`Applications` directory. It recreates the bundle's symbolic links without
needing any privilege: directory links become junctions and file links
become hard links, unless the host allows native symlinks. Each link's
original target is recorded in a manifest, and the loader and guest file
services resolve links through it. Guest `/Applications` and `/Library`
map into the root. See the [QLab audit](ALCHEMY_QLAB_AUDIT.md#installing-with-alchemy-install).

The default execution mode is the reference interpreter. `--jit` uses the
existing x64 translator with interpreter fallback. `--jit-stats` enables JIT
and prints actual translated-instruction counts on stderr. Guest output remains
on its chosen stdout/stderr channel. Successful execution returns the guest
exit status; compatibility failures return a nonzero status and a diagnostic.
`--instruction-limit` sets the existing per-invocation instruction budget,
including initializer calls. Options precede the program path; subsequent
arguments belong to the guest.

The expanded host profile requires x64 Windows 10 or Windows Server 2016.

## Supported behavior

- Little-endian ARM64 executables using `LC_MAIN`, dylibs, bundles, and
  ARM64 slices selected from universal files.
- Validated segment mapping, zero-filled tails, inaccessible `__PAGEZERO`,
  and final read/write/execute permissions.
- Chained 64-bit and 64-bit-offset fixups, conventional dyld rebasing/binding,
  eagerly resolved lazy imports, exported-symbol tries, dependency reexports,
  weak definitions, and null unresolved weak imports.
- Guest dependencies addressed through `@rpath`, `@loader_path`, `@executable_path`, or
  explicit host dylib directories. System library identities use Alchemy
  replacements. The exceptions are the allow-listed guest runtime libraries
  from `--runtime-dir`, which must have verified provenance.
- Conventional `__mod_init_func` pointers and modern `__init_offsets`
  initializers, once per image after all binding,
  before main. An initializer's raw exit stops process startup.
- Guest `argc`, `argv`, inherited UTF-8 environment strings, and an `apple`
  vector containing `executable_path=PATH`, with aligned guest stack storage.
- Persistent module handles, `dlopen`/`dlsym`/`dlclose`/`dlerror`/`dladdr`, and
  basic dyld image queries. Closed modules remain mapped until shutdown.
- Independent libSystem services for memory, allocation, strings, numeric
  formatting, opaque stdio objects, files/directories, environment, clocks,
  entropy, C/POSIX locale, and checked whole-region VM operations. Darwin
  values and guest structures are translated explicitly.
- Darwin pthread workers, mutexes, conditions, rwlocks, once, keys,
  thread finalizers, isolated errno, and Mach-O thread-local storage, using
  Transmute's existing shared-address-space workers.
- Bounded Objective-C class/category registration, ABI-preserving message
  dispatch, strong ARC and per-thread autorelease pools; CoreFoundation value
  types and a small Foundation value bridge. See the
  [exact object profile](ALCHEMY_OBJC_CF.md) for supported behavior and limits.
- Raw BSD file calls and `exit=1` through `svc #0x80`, including
  Darwin errors and carry flags. Unknown BSD calls return `ENOSYS`.
- Guest child processes (fork/exec, popen, system) under a launcher; see
  [Installers and child processes](#installers-and-child-processes).

`--runtime-dir` selects independently built ARM64 guest runtime libraries.
These are LLVM's libc++, libc++abi and libunwind, and Expat's
`/usr/lib/libexpat.1.dylib` (`scripts/build_alchemy_expat.ps1`). Only the
allow-listed Apple identities are accepted, and only with a matching
`provenance.json` beside them. See [runtime build instructions](../runtime/README.md#runtime-identity-allow-list).
Without those libraries, `/usr/lib/libc++.1.dylib` remains a declaration-only
identity for programs importing no C++ exports. See [runtime build instructions](../runtime/README.md)
and [expansion validation](ALCHEMY_EXPANSION.md) for the exact C++ profile and gates.
Unsupported strong symbols fail before execution. Authenticated pointers/arm64e, unsupported formatting,
Mach traps, custom dynamic linkers, and encrypted code fail explicitly.
Code-signature blobs are retained as bounded metadata; the starter does not
implement macOS trust enforcement. Strings and formatted output have a 1 MiB
limit; files and mapped images are limited to 512 MiB each, with up to 256 loaded
modules. Locale support is currently C/POSIX with ASCII conversion; UTF-8
locale requests fail. Private file mappings are copies. Under alchemy-fx,
`mmap(MAP_SHARED)` and `shm_open` objects are host file sections shared
between Alchemy processes (ALCHEMY_PROCESS.md, "Shared memory"). Other shared
or fixed-address VM behavior, partial region protection/unmapping, signal delivery,
and custom pthread stack addresses or unsupported scheduling attributes remain
unsupported. Explicit pthread stack sizes use Transmute's configurable stacks;
the original Python executable's 16 MiB main stack is allocated by the engine.

## Installers and child processes

These behaviors were added for an add-on installer for Blender (a wxWidgets/Cocoa
program run in its command-line mode), which installs headless as root
(`ALCHEMY_SUPERUSER=1`).

- **Guest file system.** `translate_path` (`darwin/libsystem/services_impl.hpp`) maps an
  absolute guest path as follows. `/tmp` is the host temporary directory.
  `/Volumes/<letter>/...` is that host drive. With an Alchemy root configured
  (`--root`, `ALCHEMY_ROOT`, or a `.alchemy-root` marker above the program),
  `/Applications`, `/Library`, `/Users`, `/etc`, `/opt`, `/private`, `/usr` and
  `/var` live under the root, and `/Volumes` itself is an empty directory of
  the root. Every other absolute path, and all paths when no root is
  configured, is relative to the drive root of the working directory. That
  drive root is the `root` member, set in `DarwinServices::Impl`'s
  constructor. An installer that writes system locations therefore needs a
  root, or it writes into the host drive.
- **Superuser.** The guest identity is uid/gid 1000. `ALCHEMY_SUPERUSER=1` in
  the environment starts the process as root, the explicit equivalent of
  `sudo`. A root process may then change its real and effective ids between 0
  and 1000 under setuid(2) rules. Host files stay owned by the host user.
  `AuthorizationExecuteWithPrivileges` fails explicitly, because no
  privileged helper process can be launched.
- **Child processes.** The Darwin process model of
  [ALCHEMY_PROCESS.md](ALCHEMY_PROCESS.md): every guest child is a new
  instance of the running Alchemy executable with the parent's runtime
  options and `--root`; pipes, `fork`/`vfork` (emulated up to `exec`),
  `posix_spawn`, `exec*`, `wait*`, `kill`, `popen` and `system` with a
  built-in POSIX shell subset. A fork child's `setenv` and credential changes
  are its own and reach its exec'd image. A child of a root process is root.
  `socketfilterfw` is a built-in tool: no application firewall exists under
  Alchemy, so adding an exception is a no-op.
- **Links without privilege.** Inside the root, `symlink(2)` falls back to a
  recorded run-time link: a junction for a directory, or a hard link (else a
  copy) for a file. The link is recorded in `.alchemy/links/runtime.tsv`.
  `unlink` removes the link and its record. A dangling link cannot be
  represented and fails.
- **Large images.** An LC_SYMTAB string table of at least 16 MiB inside
  `__LINKEDIT` (the installer keeps a 1.85 GB payload there) is not read
  into memory. Only the names the symbol table uses are mapped; the rest of
  `__LINKEDIT` is reserved address space, so a guest read of it faults. A
  guest `malloc` block may be up to 4 GiB, because the installer reads its own
  1.9 GB executable.
- **Bind-time surface.** Every strong import of the installer resolves under
  `alchemy-fx`. The values are headless-correct: geometry, `CGColor`, no
  displays, no Carbon events or hot keys, no printers, UTType, `Gestalt`,
  Launch Services without applications, `NSUserDefaults` (volatile),
  `NSAppleEventManager`, and the process manager. Some AppKit classes are
  shells with their real superclasses. Drawing, text layout, image codecs,
  HIShape, HITheme, `CFSocket` and `CFDateFormatter` fail with "unsupported
  in a headless process" when called (`src/alchemy/frameworks/installer_frameworks.cpp`).
  GUI providers replace these shells by registering over them.
- **Diagnostics.** `ALCHEMY_TRACE_CALLS=_open,_fork` (symbol prefixes, or
  `*`) logs provider calls with their first arguments and results to stderr.

## Fixtures and verification

With Clang and `ld64.lld` available, CMake builds project-owned Mach-O fixtures
under `build/macos`. Every source includes its own required declarations; none
needs an Apple SDK. The `link_system.dylib` and `link_cpp.dylib` outputs are
project-authored link-only inputs. They are never runtime replacements and
their placeholder functions must never supply successful application output.

The integration suite checks exact hello-world output, relocation, guest dylib
exports and initialization, raw syscall output and exit, startup argument
vectors, constructor exit, unsupported formatting, and actual x64 translation.
Standalone engine, loader, and Darwin suites test malformed inputs, mapping
permissions, unavailable reservations, trap accounting, syscall errors, partial
output, and checked guest pointers. The original Windows suite remains enabled.

The Release build passed all 11 configured CTest groups on September 30, 2026,
including the optional external smoke tests. The supplied executable's SHA-256
remained `63097a1ee99e51840230c98692bade514ed5bbdfe4ef5005f0b4a4f9ad8d6602`.
It printed exactly `Hello World from MacOS: 27\n`, returned 0, and executed 22
native guest instructions in JIT mode with zero interpreter fallback
instructions. Interpreter and relocated runs produced the same result.

The expanded Release suite passed all 20 configured groups on October 1, 2026,
including the existing Windows shards and guest LLVM C++17, cross-dylib
exception, plugin and late-loaded TLS workloads. See the
[verification results](ALCHEMY_EXPANSION.md#verified-result) and
[official Blender bundle audit](ALCHEMY_BLENDER_AUDIT.md).

The bounded object-profile integration passed all 31 Alchemy groups on its
final Release build, including 15 new object-profile groups. The final full
shared-checkout run also exposed a separately owned Windows OpenGL regression.
See [object-profile results](ALCHEMY_OBJC_CF.md#verified-result) for the exact
workloads, JIT evidence, and remaining engine gate.

Use this configuration option to test a user-owned binary without copying it:

```powershell
cmake -S . -B build -DALCHEMY_EXTERNAL_SMOKE_BINARY=C:/path/to/program
cmake --build build --config Release
ctest --test-dir build -C Release --parallel 4 --output-on-failure
```

## Ownership and provenance

The engine owns CPU execution, checked memory, generic SVC events, and JIT.
The loader owns Mach-O formats, mappings, exports, and fixups. The Darwin
runtime owns guest ABI decoding, startup vectors, output, and syscall policy.
`ProcessContext` owns process services and `ThreadContext` owns errno, dynamic
loader errors, TLS, and thread cleanup. `ModuleLoader` owns dependency discovery,
linking, handles, symbol scopes, and initialization. The Alchemy engine adapter
uses existing engine APIs; engine requirements are documented separately.
Shared build
configuration and cross-module tests are maintained by the integration owner.

All new compatibility implementations are project-authored under the existing
MIT license. No Apple implementation code, system-library binaries, SDK
payloads, dyld shared cache, frameworks, fonts, or other platform assets are
included. The optional guest C++ profile builds LLVM runtime dependencies from
their pinned permissively licensed source and preserves their notices.

Interface references used in the design include
[Apple's ARM64 ABI](https://developer.apple.com/documentation/xcode/writing-arm64-code-for-apple-platforms),
[LLVM 19.1.5 Mach-O format definitions](https://github.com/llvm/llvm-project/blob/llvmorg-19.1.5/llvm/include/llvm/BinaryFormat/MachO.h),
[published dyld startup contracts](https://github.com/apple-oss-distributions/dyld/blob/main/dyld/dyldStartup.s),
and [published Darwin syscall contracts](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/dev/arm/systemcalls.c).
The initial design investigation inspected published implementation files to
confirm interface facts. This is an independent, specification-driven
implementation; it does not claim a formal clean-room process.

Clang and LLD are external build tools. Any later non-Apple dependency requires
license review and preservation of its own notices. LLVM's terms are documented
in its [licensing policy](https://llvm.org/docs/DeveloperPolicy.html#copyright-license-and-patents).
An external sample remains a local test input until its redistribution rights
are established. These engineering controls do not give blanket legal clearance.
Public distribution requires review of applicable licenses, interoperability
rules, and branding; Apple's software licenses can be reviewed at its
[license index](https://www.apple.com/legal/sla/).

## Next milestones

The [bundled Python milestone](ALCHEMY_PYTHON.md) records direct execution of
the original bundled interpreter, real scripts and native-extension callbacks,
alongside the SDK-free embedding launcher and measured instruction progression.
The [Python audit](ALCHEMY_PYTHON_AUDIT.md)
preserves the bundle's interpreter, library and extension inventory.
The [Python dependency follow-up](ALCHEMY_PYTHON_FOLLOWUP.md) records continued
C runtime, file, Foundation and bounded dispatch work from the earlier wave.

1. Expand Python standard-library/native-module coverage and stock C++ ABI
   validation through named workloads, adding measured libc/POSIX requirements.
2. Expand the general Objective-C/CoreFoundation profile through named
   command-line workloads, then audit-selected non-windowing framework services.
3. App bundles, AppKit windowing, graphics, input, audio, and Metal translation.
4. Installation and launch tools with application-specific compatibility and
   performance certification.

Each milestone needs named workloads and acceptance tests. OS boot and kernel
drivers are outside this user-space application runtime.
