# Conformance suites

Established open-source test suites, built as arm64 macOS guest programs and run under `alchemy-fx`.
They check Alchemy against what the real system specifies, not against Alchemy's previous build.
Each suite has an expected-results baseline in `baseline/<suite>.tsv`. `report` exits non-zero when
a test that passed in the baseline no longer passes.

| suite | upstream | licence | what it covers |
|---|---|---|---|
| `dispatch` | swift-corelibs-libdispatch `tests/` (swift-6.2-RELEASE) | Apache-2.0 | queues, groups, apply, timers, sources, I/O, data, semaphores |
| `libc_test` | musl libc-test (functional, regression, math) | MIT | libc functions, regressions, libm exact vectors and FP flags |
| `objc4` | objc4-951.7 `test/` | APSL-2.0 | the Objective-C runtime |
| `libc_apple` | Libc-1752.120.2 `tests/` | APSL-2.0 | Darwin libc's own expectations |
| `libpthread` | libpthread-539.100.4 `tests/` | APSL-2.0 | pthreads, TSD, cancellation, stacks, QoS |
| `posix` | Open POSIX Test Suite, LTP 20260930 | GPL-2.0 | POSIX interfaces: threads, signals, timers, semaphores, mmap |

## Rules

- Upstream sources are downloaded only from the official upstreams listed in `sources.json`. Each
  goes into its own new directory under `C:\dev\conformance\` (`<name>\<archive>`, `SHA256`,
  `SOURCE_URL`, `src\`), never into this repository. They are untrusted data.
- Pins are reproducible. Archives that a host generates from a tag or commit (GitHub tag archives)
  are pinned by a digest of their extracted content (`tree_sha256`), because their compressed bytes
  can change. libc-test is fetched with git at its pinned commit: repo.or.cz serves web snapshots
  only behind a bot check. LTP's uploaded release asset keeps a byte SHA-256. `fetch_sources.py
  --digest <name>` prints a source's content digest.
- GPL code (the Open POSIX suite) is never copied into this repository. It is built from the
  extracted release into the output directory, and `suites/posix.py` only names those paths.
- When built programs leave the machine (the public artifact of the macOS workflow), their source goes
  with them. `fetch_sources.py --copy-copyleft` adds the pinned archive and licence text of every
  source marked `ship_with_binaries` in `sources.json`. That covers Open POSIX (GPL-2.0) and Apple's
  objc4, Libc and libpthread tests (APSL-2.0).
- Guest programs run only through `alchemy-fx`. Every run has `ALCHEMY_GUI_CLOAK=1`,
  `ALCHEMY_GUI_NO_ACTIVATE=1`, `ALCHEMY_GUI_NO_CRASH_DIALOG=1` and `ALCHEMY_NETWORK=loopback`. It
  also gets an Alchemy root of its own (`out\root`), so `/usr`, `/etc`, `/var` and `/private` never
  reach the host drive. Output goes to `out\logs`. A timeout kills only the `alchemy-fx` process the
  runner started, together with its child processes.
- Check the fx perf measurement window before building or running.

## Usage

```
python -I scripts/conformance/fetch_sources.py --sources C:\dev\conformance
python -I scripts/conformance/build_sysroot.py --sources C:\dev\conformance --out C:\dev\conformance\out\sysroot
python -I scripts/conformance/conformance.py build all
python -I scripts/conformance/conformance.py run all --jobs 6
python -I scripts/conformance/conformance.py report all
```

`run <suite> --filter <regex>` reruns a subset and merges it into the stored results.
`report <suite> --write-baseline` records the current results as the new baseline. It keeps
existing owners, notes and XFAIL labels, and labels the expected failures each suite knows about.
The default `alchemy-fx` is `build-ninja\Release\alchemy-fx.exe` in this checkout. `--alchemy`
selects another. `repro <file.c>` builds one source against the sysroot and runs it the same way.

## Ground truth on real macOS

`.github/workflows/conformance-macos.yml` builds the suites on a Windows runner and runs the same
programs natively on an Apple-silicon runner (`run all --native`). It runs by hand or weekly, and
never gates CI. Download its `conformance-macos-results` artifact, then compare:

```
python -I scripts/conformance/conformance.py compare all --macos-results <artifact>\results
```

`compare` lists Alchemy-only failures (the program passes on macOS, so these are Alchemy bugs),
failures on both, and programs that pass only under Alchemy. It classifies the macOS results again
from their exit codes and output with the current harness, so an older run still reads correctly
after a harness fix. It also records the macOS status in
`baseline/macos/<suite>.tsv`. From then on, `report --write-baseline` marks a failure that also
fails on macOS as XFAIL.

## The sysroot

The tests are compiled against Darwin's real declarations, never against `runtime/include`. A wrong
constant in Alchemy's own headers would otherwise agree with the same wrong constant in a provider.
`build_sysroot.py` assembles `usr/include` from Apple's open-source releases (no SDK). It follows
their installs:

- xnu's `bsd/`, `osfmk/`, `libkern/`, `libsyscall/` and `EXTERNAL_HEADERS/` headers;
- Libc's `include/`, with `//Begin-Libc` blocks stripped as `strip-header.ed` does;
- libpthread, libplatform, libmalloc, Libinfo, libclosure (`Block.h`), libdispatch and objc4's public
  headers;
- `Availability*.h` and `os/availability.h`, generated by AvailabilityVersions' own script;
- `sys/_symbol_aliasing.h` and `sys/_posix_availability.h`, generated after xnu's scripts;
- `unifdef.py`, a small unifdef that runs xnu's `DATA_UNIFDEF` set for macOS and Libc's feature set
  (it selects the MacOSX branches, so arm64 gets `__DARWIN_ONLY_UNIX_CONFORMANCE`);
- `mig_headers.py`, which writes the MIG user headers (`mach/task.h`, `mach/mach_port.h`, ...) from
  xnu's `.defs` files with MIG's user-side calling convention. MIG itself is not among the
  downloads.

Gaps and substitutes:

- Libm's last open-source release predates arm64. `math.h` is its unified Intel header, which
  declares the same C99 interface. `fenv.h` is `include/fenv.h`, written from AArch64's FPSR and FPCR
  bit layout.
- `TargetConditionals.h` belongs to no open-source project, and `dlfcn.h` is dyld's. Both come from
  Alchemy's `runtime/include`.
- The usr/lib `.tbd` files are link-only interfaces with no exports. Programs link with
  `-undefined dynamic_lookup`, and Alchemy's providers supply every name at run time.

## Suite notes

- `libc_test` is built with `-D_DARWIN_C_SOURCE`, Darwin's equivalent of the `_GNU_SOURCE` and
  `_BSD_SOURCE` the tests ask for. It is also built with `-D_FORTIFY_SOURCE=0`: the suite checks libc
  behaviour, while the `__*_chk` entry points that optimized builds call are a separate finding.
- `objc4` follows `test.pl`. A test's output must be exactly `OK: <name>` or match `TEST_RUN_OUTPUT`,
  with no `BAD:` line. The configuration is MEM=arc unless the test allows only mrc. Swift tests are
  skipped, and so are `TEST_BUILD` recipes that need the shell. Tests run in test.pl's environment:
  `OBJC_DEBUG_DONT_CRASH=YES` for `TEST_CRASHES` tests (the runtime prints `objc[pid]: HALTED` and
  exits) and `MallocScribble=1`.
- `libc_apple` and `libpthread` use `darwintest/`, a darwintest-compatible harness. darwintest is not
  open source; the harness implements the macros the way the open-source tests use them. Each
  `T_DECL` is one test, run as `<program> <case>`.
- `dispatch` copies the tests out of the Swift runtime build's existing checkout and applies the
  small portability patches listed in the suite (test-side declarations only).

## Results

Baselines are in `baseline/`. Each row is test, expected (PASS, FAIL, XFAIL, BUILD, SKIP), owner, and
a note. XFAIL marks a failure that is not an Alchemy bug:

- a feature Alchemy honestly refuses by policy (sandbox, launchd/XPC services today);
- an interface Darwin does not have (Linux or musl behaviour, POSIX options macOS lacks);
- a test that needs root, entitlements or kernel features.
