# A Linux host for Alchemy

This page maps each host area to Linux. The Linux backends are in progress.

## State (2026-11-10)

* **Build.** Clang 21 on Ubuntu 26.04 (x86-64) builds `alchemy`, `alchemy-fx`,
  `alchemy_host_tests` and `fx_host_tests`:

  ```sh
  cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_LINKER_TYPE=LLD \
    -DTRANSMUTE_WITH_FX=ON -DALCHEMY_HOST=linux
  ```

  `ALCHEMY_HOST=null` builds too. The Mach-O fixtures build with the
  distribution's clang and `ld64.lld`.
* **fx** has a Linux host backend (`src/fx/host/linux/`). It covers the
  following, and `fx_host_tests` passes:
  * faults through `sigaction` on an alternate signal stack;
  * guarded access through per-thread `sigsetjmp` recovery points;
  * an address-space table behind `vm::query`, with placeholders as
    `PROT_NONE` reservations and memfd views;
  * futex locks;
  * pthreads.
* **The PE guest personality** (`fx/loader`, `fx/abi`) is Windows-only. On
  Linux:
  * `src/fx/runtime/pe_absent.cpp` keeps the external (Mach-O) module
    registry;
  * `fx/seh` builds with its own copies of the ARM64 exception layouts
    (`fx/seh/arm64_nt.hpp`).
* **Calling convention.** Generated code keeps the Win64 convention on every
  host. `FX_GENERATED_ABI` (`transmute/fx/context.hpp`) marks the C++ side of
  each crossing as `ms_abi` on System V hosts: helpers, host callbacks and
  leaves, the enter and abort stubs. `host_call_bridge` takes the System V
  convention on Linux, because system libraries call it.
* **Alchemy** has a Linux host backend (`src/alchemy/host/linux/`). These areas
  are written: files, process (fork + execve spawns, pidfds), sync (eventfds;
  named mutexes and semaphores as FIFOs), time, vm, sections (memfd, POSIX
  shared memory), net (BSD sockets), ipc (abstract UNIX sockets), system,
  debug, and crypto's random bytes. The other areas fail with
  `Error::unsupported`: dns_sd, http, locale, icu and the rest of crypto.
* **The window server** (`src/alchemy/host/linux/gui/window_server.cpp`)
  speaks X11 through XCB, with xkbcommon for keys. `apt install
  libxcb1-dev libxcb-xkb-dev libxcb-cursor-dev libxcb-icccm4-dev
  libxkbcommon-dev libxkbcommon-x11-dev`. It covers:
  * top-level windows;
  * expose, move, resize, focus and close notifications;
  * mouse buttons, click counts, the wheel and motion;
  * keys with macOS key codes and NSEvent function-key characters;
  * `present_bitmap` through PutImage;
  * a run-loop wait that polls the X socket with the handles.

  Not written yet: XRandR screens and HiDPI, cursors, the X clipboard (the
  clipboard is process-local), drag and drop, IME. The other GUI areas
  (gpu's Vulkan half, audio, media codecs, ...) still fail with
  `Error::unsupported`.
* **2D drawing** (`gui/canvas.cpp`). Surfaces are Cairo image surfaces of
  premultiplied BGRA. They cover:
  * paths, clips, the three blends and the brushes (solid, tiles, linear
    gradients);
  * bitmaps, at the three interpolations;
  * glyphs, drawn through cairo-ft with vertical-only hinting, as
    DirectWrite's natural mode draws them.

  `Geometry::widened` is its own polygon stroker (Cairo has no public
  widening): flattening to 0.05, dashes, joins and caps. Its result fills
  with the non-zero rule. Compared with Direct2D on a fixed scene
  (`ALCHEMY_HOST_TEST_CANVAS_REPORT=<file>`, with
  `ALCHEMY_HOST_TEST_FONT_FILE` for glyphs), pixels differ by 0.6 per
  channel on average. The differences are antialiased edges, aliased
  diagonals and glyph coverage.
* **System text rasterizing** (`gui/raster_text.cpp`), GDI's counterpart.
  fontconfig picks the face, which FreeType hints. Ascent, descent and
  advances come from the face's VDMX and hdmx tables where it has them, as
  GDI reads them. With arial.ttf registered, the metrics and advances equal
  GDI's at 12, 16 and 64 px, with two exceptions:
  * cap and x heights are the face's real ones; win32 reports em/2 and
    em/4, because GDI documents those fields as unsupported;
  * one advance at 64 px differs.
* **Image files** (`gui/images.cpp`). PNG uses libpng, JPEG uses TurboJPEG,
  TIFF uses libtiff with pages as frames, and GIF uses giflib with frames as
  stored. BMP and ICO are read here. Frame facts follow WIC's, including
  96 dpi when a file names none.
  * Checked (`ALCHEMY_HOST_TEST_IMAGE_REPORT`, `ALCHEMY_HOST_TEST_IMAGE_OUT`):
    JPEG pixels are identical to WIC's. Each host reads the other's PNG,
    TIFF, BMP, JPEG and GIF output.
  * Where WIC differs:
    * it makes a GIF's transparent index opaque (in its palette colour);
      Linux keeps it clear;
    * it reports 10 bits for its own BMP;
    * it orders ICO frames differently.
* **Fonts and text layout** (`gui/fonts.cpp`, `gui/text_layout.cpp`).
  `apt install libpango1.0-dev` (it brings FreeType, fontconfig, HarfBuzz
  and FriBidi). They work as follows:
  * The system fonts are fontconfig's system font set.
  * `register_font_file` adds a file to its application font set. The bytes
    go into a memfd, kept open and added by its `/proc/self/fd` path.
  * Faces are FreeType faces. Names come from fontconfig. Weight, width and
    slope come from OS/2, and vertical metrics follow Windows (usWin*, or
    the OS/2 typographic values under USE_TYPO_METRICS).
  * Families match in the CSS order (stretch, style, weight).
  * Fallback is fontconfig's sorted list for the family.
  * `layout_text` is a Pango layout over the same fontconfig configuration:
    HarfBuzz shaping, FriBidi, fontconfig fallback, word and character
    wrapping without inserted hyphens, justification and tabs. Each span's
    tag travels as a foreground colour.

  Checked against DirectWrite on the same `arial.ttf`
  (`ALCHEMY_HOST_TEST_FONT_REPORT=<file> alchemy_gui_host` prints both
  hosts' readings). Names, metrics, glyph metrics, glyphs, tables, kerned
  advances and line breaks are equal. Outlines agree to 0.003 px.
  Remaining differences:
  * run baselines: DirectWrite adds the line gap above each line (CoreText
    reads only x);
  * letter spacing: Pango puts half before each character;
  * a tab that ends a wrapped line is a zero-width space glyph rather than
    no glyph.

  `alchemy_gui_host` checks fonts with DejaVu Sans on Linux and Arial on
  Windows. `ALCHEMY_HOST_TEST_SKIP_AREAS` (images, fonts, canvas,
  raster_text, midi, audio, platform, media_codecs) leaves out the areas
  that aren't written yet.
* **What runs** (cloaked, `ALCHEMY_NETWORK=loopback`):
  * the console Mach-O fixtures (hello, dylib_app, weak_import, syscalls);
  * Blender's CPython 3.11 (`--version`, `-c` with json/hashlib/re/os,
    `site`, exit codes);
  * LLVM 23's `clang --version`, `ld64.lld --version` and `clang -c`, with
    the LLVM and libxml2 runtimes from `--runtime-dir`;
  * `macos/gui_window_events`, on a private display: `Xvfb :99` and
    `DISPLAY=:99`, not the desktop. Driven with real X input through
    `xdotool`, it reports clicks, double clicks, the wheel, shifted keys,
    arrows, function keys and modifier changes.
* **Not done yet:**
  * DWARF CFI for the code arena, so C++ exceptions crossing translated frames
    fail on Linux;
  * thread sampling;
  * passing descriptors to other processes;
  * case-insensitive lookups under the Alchemy root (WSL's kernel has no ext4
    casefold);
  * ICU and locale;
  * TLS;
  * Vulkan and the rest of the GUI areas.

The macOS layer (`src/alchemy/darwin/`, the loader, the Objective-C
runtime) talks to the operating system only through the host API in
`include/alchemy/host/` (see [ALCHEMY_PROVIDERS.md](ALCHEMY_PROVIDERS.md)).
Windows is `src/alchemy/host/win32/`. A Linux port is
`src/alchemy/host/linux/` plus the fx engine's own host seams, listed
below. Nothing in `src/alchemy/darwin/` changes.

* **Starting.** Copy `src/alchemy/host/null/`: it implements every call as
  a failure. Replace the calls one by one, and add `linux` to the
  `ALCHEMY_HOST` values in `cmake/AlchemyHost.cmake`.
* **Building.** The null backend's Windows build already proves that the
  macOS layer compiles without OS headers. In that build, CMake places
  poison copies of the OS headers first on the macOS layer's include path,
  so any stray OS include fails the build.
  * `alchemy-fx`, `alchemy` and `alchemy_host_tests` build and link against
    `host/null` (`ALCHEMY_HOST=null`) with 56 OS headers poisoned. This was
    first verified at the end of the refactor wave, on 2026-10-18.
  * The null `alchemy-fx` runs: it prints its usage and fails explicitly at
    its first host call.

## Host API areas

| Area | Windows backend today | Linux calls |
| --- | --- | --- |
| `vm` | VirtualQuery, VirtualLock, GetSystemInfo | `/proc/self/maps` (or `mincore`) for queries, `mlock`/`munlock`, `sysconf(_SC_PAGESIZE)`. Page size 4 KiB; on arm64 hosts often 16 or 64 KiB (see "Linux arm64"). |
| `sections` | CreateFileMapping, MapViewOfFile, DuplicateHandle across processes | `memfd_create` + `ftruncate` + `mmap(MAP_SHARED)`. Passing a section to another process is passing its descriptor (`SCM_RIGHTS`). |
| `files` | CreateFileW, Get/SetFileInformationByHandle, LockFileEx, MoveFileExW, reparse points, NTFS streams for xattrs, SDDL for modes | `openat2`/`openat`, `pread`/`pwrite`, `statx`, `renameat2`, `linkat`/`symlinkat`, `getdents64`, `fcntl(F_OFD_SETLK)` (POSIX record locks), `getxattr` family (`user.` namespace), native modes and owners, `inotify`/`fanotify` for change watching. Darwin semantics stay in libsystem; most of them map more directly than on Windows. |
| `net` | WinSock (incl. AF_UNIX), WSAPoll, GetAdaptersAddresses | BSD sockets (AF_UNIX native), `poll`/`epoll`, `getaddrinfo`, `getifaddrs` + netlink for adapters. |
| `dns_sd` | the windns DNS-SD client (DnsServiceBrowse, DnsServiceResolve, DnsServiceRegister), GetAddrInfoW | Avahi over D-Bus (`ServiceBrowser`, `ServiceResolver`, `EntryGroup`, `HostNameResolver`), or a built-in mDNS responder; `getaddrinfo` (nss-mdns) for `.local` names. |
| `http` | WinHTTP (NSURLSession, CFNetwork, PAC) | libcurl, or no host stack at all: NSURLSession implemented over the `net` area and the `crypto` TLS stack. Proxy settings from the environment (`http_proxy`) or GSettings. |
| `process` | CreateProcessW + handle lists, job objects, Toolhelp, LoadLibrary | `posix_spawn`/`clone3` with an explicit descriptor list, `pidfd_open` + `poll` for exit watching, `/proc` for process lists, `dlopen`/`dlsym`. A process tree that dies together: `PR_SET_PDEATHSIG` or a cgroup instead of a job object. |
| `ipc` | Named pipes (Mach transport), DuplicateHandle, WSADuplicateSocket | UNIX domain sockets (`SOCK_SEQPACKET`) with `SCM_RIGHTS` to pass descriptors. Simpler than the Windows transport: rights travel in-band with messages. |
| `sync` | Events, named semaphores, WaitForMultipleObjects, thread-pool timers | `eventfd`, `timerfd`, `epoll` as the multi-wait; futexes for in-process waits; cross-process semaphores in a shared `memfd` (`sem_init(pshared)`). |
| `time` | QueryUnbiasedInterruptTimePrecise, GetSystemTimePreciseAsFileTime | `clock_gettime(CLOCK_MONOTONIC)` (mach_absolute_time excludes sleep, like MONOTONIC), `CLOCK_REALTIME`, `clock_nanosleep`. |
| `locale` | NLS (LCMapStringEx, CompareStringEx, NormalizeString), ICU time zones | ICU (the system `libicu`) for collation, normalization, case mapping and time zones; `/etc/localtime` and `TZ` for the zone. |
| `crypto` | BCrypt, NCrypt, Cert*, DPAPI, SChannel | `getrandom`; OpenSSL (libcrypto/libssl) or a bundled library for digests, ciphers, keys, X.509 and TLS; the keychain in a file store encrypted with a key from the Secret Service (libsecret) or a local key. |
| `system` | GlobalMemoryStatusEx, power APIs, registry, adapters | `/proc/meminfo`, `sysinfo`, PSI (`/proc/pressure/memory`) for memory pressure, `/sys/class/power_supply` for battery, UPower or logind (D-Bus) for sleep/wake and power assertions (inhibitors), `uname`, `gethostname`. |
| `ui` | Win32 windows, messages, DPI, clipboard, OLE drag and drop, IMM, DWM cloak, TaskDialog, shell | Wayland (xdg-shell, xdg-decoration, fractional-scale, text-input-v3, data-device for clipboard and drag and drop, xdg-desktop-portal for file dialogs) with X11 (XCB) as a fallback; "cloaked" runs as an off-screen or headless compositor (e.g. `weston --backend=headless`). |
| `audio` | WASAPI device enumeration, output and capture streams, the microphone consent store | PipeWire (or ALSA directly) for devices and streams. |
| `midi` | WinMM `midiIn*`/`midiOut*` | ALSA sequencer, or PipeWire MIDI. |
| `speech` | SAPI 5 `ISpVoice` speaking into a memory stream | speech-dispatcher or eSpeak NG into a buffer. |
| `camera` | Media Foundation source readers (`MFEnumDeviceSources`, NV12 output) and the webcam consent store | V4L2 (or PipeWire camera portals), converting to NV12. |
| `media_codec` | Media Foundation encoder transforms (`MFTEnumEx`: H.264, HEVC and AAC; the system's own and the drivers' hardware encoders) | VA-API or V4L2 memory-to-memory encoders, and GStreamer's system plug-ins where installed; none listed otherwise. |
| `gpu` | Vulkan loader + `vkCreateWin32SurfaceKHR`; D2D, DirectWrite and WIC under CoreGraphics, CoreText and ImageIO | Vulkan with `VK_KHR_wayland_surface`/`xcb_surface` (Vulkan code is unchanged). The 2D stack needs a replacement for D2D/DirectWrite/WIC: Skia, or Cairo + FreeType + HarfBuzz + libpng/libjpeg, behind the same `gpu` area calls. This is the largest single piece of a port. |
| `debug` | SetUnhandledExceptionFilter and SetErrorMode, RtlLookupFunctionEntry/RtlVirtualUnwind, RtlCaptureStackBackTrace, GetModuleHandleEx | `sigaction` for SIGSEGV/SIGBUS/SIGILL/SIGFPE on an alternate stack, `backtrace()` or libunwind, `dladdr` for module and offset. |
| network policy (`ALCHEMY_NETWORK`) | in `src/alchemy/host/common/`, above the backends | unchanged: every backend inherits it. |

## fx engine host seams

The engine has its own seams under `src/fx/host/`. Their exact list and
state are in [FX_HOST_SEAMS.md](FX_HOST_SEAMS.md).

| Seam | Windows today | Linux |
| --- | --- | --- |
| Faults (guest memory faults, JIT traps) | vectored exception handler (VEH) | `sigaction(SIGSEGV/SIGBUS/SIGILL)` with `SA_SIGINFO` on an alternate signal stack; `ucontext_t` instead of `CONTEXT` |
| Calling convention of generated host code | Win64 (`jit_x64_windows.cpp`: shadow space, `rcx/rdx/r8/r9`, callee-saved `xmm6-15`) | System V AMD64 (`rdi/rsi/rdx/rcx/r8/r9`, no shadow space, no callee-saved xmm) on x64 hosts; AAPCS64 on arm64 hosts |
| Guest memory placeholders and views (VA = guest VA) | `VirtualAlloc2(MEM_RESERVE_PLACEHOLDER)` + `MapViewOfFile3(MEM_REPLACE_PLACEHOLDER)` | `mmap(PROT_NONE, MAP_NORESERVE)` reservations, `mmap(MAP_FIXED_NOREPLACE)` / `MAP_FIXED` over them, `memfd` for shared and file-backed views, `mremap` |
| Thread context and TLS | `GetThreadContext`, TEB-based TLS | `pthread_getspecific`/`__thread`, `ucontext_t` from the signal handler |
| Code cache file mapping | file mapping of the code cache | `mmap` of the cache file (`MAP_SHARED`), `memfd` + dual mapping (RW and RX) if W^X is enforced |
| PE-specific pieces | unwind tables for generated code (`RtlAddFunctionTable`), SEH | `.eh_frame` registration (`__register_frame`) for generated code |

## What a Linux arm64 host could skip or simplify

On a Linux arm64 host, the guest's arm64 code runs on an arm64 CPU, and
POSIX is closer to Darwin than Win32 is.

* **Translation.** fx translates arm64 to x64. An arm64 host could run
  guest code natively, and keep fx only for the entry, exit and
  system-call trampolines and for the few instructions Linux disallows
  (none in EL0 user code, in practice).
  * The JIT, the code cache and the calling-convention seam would shrink
    to a small trampoline layer.
  * The fault seam would remain, for guest-visible memory faults.
* **Page size.** Darwin arm64 uses 16 KiB pages.
  * A Linux arm64 kernel built with 16 KiB pages (`CONFIG_ARM64_16K_PAGES`,
    as on Asahi) matches Darwin exactly. 4 KiB kernels work as they do on
    x64 today. 64 KiB kernels cannot honour 16 KiB-granular protections.
* **libSystem semantics.** Many of them become thin wrappers: POSIX file
  modes and owners, record locks, AF_UNIX, `fork`-like spawning, signals
  and `mmap`.
  * Most of the emulation in `darwin/libsystem/` is not needed on Linux:
    NTFS streams standing in for xattrs, the symlink records,
    SDDL-derived modes, and named pipes standing in for Mach ports.
* **Mach IPC.** It still needs Alchemy's own implementation; Linux has no
  Mach ports. UNIX sockets with `SCM_RIGHTS` carry it more directly than
  named pipes with handle duplication.
* **Things that stay as large as on Windows:**
  * the UI (Wayland, with X11 as fallback);
  * the 2D, text and imaging stack under CoreGraphics and CoreText;
  * audio and MIDI;
  * TLS and the keychain.
