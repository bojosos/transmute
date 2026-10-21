# Alchemy memory: shared image pages and translation cache

How much private memory each alchemy-fx process holds, where it goes, and what
changed when Mach-O images and the fx translation cache's main files became
shared file views (2026-10-18).

## Summary

* **Before:** every guest process held two private copies of each image.
  * `MachOImage::load` read the whole file into a buffer that lived as long as
    the image. For Chrome's universal framework, it read 496 MB and kept the
    243 MB arm64 slice.
  * `MachOImage::map` committed every segment privately and copied the bytes
    in, `__TEXT` included.
  * Nothing was shared between processes. On macOS these pages are file-backed
    and shared.
* **Now:** `load()` views the file read-only, and `map()` maps each segment
  copy-on-write from the file at its exact guest address. Only the pages that
  binding writes become private. The loader drops the file view once the image
  is mapped.
* **Result:**
  * Chrome renderer peak private: **~1.91 GB → ~1.45 GB** (−460 MB each).
  * Browser: **1.83 → 1.33 GB**.
  * Tree peak private for one strict headless `--dump-dom`: **11.4–13.3 GB →
    9.9–10.1 GB**.
* **Then the cache file:** fx cache main files became shared read-only views
  too (each Chrome process had read the framework's 470 MB cache file into
  452 MB of private memory).
  * Renderer: **~1.45 → ~1.00 GB**.
  * Browser: **1.33 → 0.87 GB**.
  * Tree peak: **9.2 → 6.6 GB**.
* **Then the fx code arena (2026-10-20):** the preloader installed every cached
  region, including code only other process types run, about 290–450 MB per
  process. Alchemy's child processes no longer preload (`TRANSMUTE_FX_PRELOAD`
  set explicitly wins); a top-level process still does.
  * Chrome's tree peak: **12.6 → 3.9 GB** (rAF page; the browser process keeps
    its 1.25 GB, the children drop to 0.2–0.6 GB).
  * Single-process apps (Blender, QLab, the renderer add-on) are unchanged (see "The fx code
    arena").
* **Then arena reclaim (2026-10-21):** dead translations' chunks are freed and
  reused (see "The fx code arena: reclaim").
  * The JIT churn page that killed main's renderer after 50 s now runs 900 s.
  * Its renderer peaks at 610 MB private instead of dying at 1764 MB, with
    about 100 MB of arena.

## Measurements

### Setup

* Chrome 155 strict headless `--dump-dom` of `probe/test.html`, run with
  `run_chrome_s3.ps1`: JIT on, cloaked, `ALCHEMY_NETWORK=loopback`, the
  offline flags.
* **Baseline:** main's code (b330516; the refactor-followups build of
  bd1b542, which has the same alchemy-fx sources).
* **After:** this branch.
* The machine was shared with 2–3 other builds throughout, at 80–100 % CPU
  load. Memory numbers barely depend on load, but wall times do.

### Tools

The tools live outside the repo, in the Chrome sessions' shared notes folder under `memory/` (with every run's samples in `memory/runs/`):

* `mem_probe.ps1` samples the run's alchemy-fx process tree every second.
  * It reads `WorkingSetSize` and `PrivatePageCount` from `Win32_Process`,
    which are WorkingSet64 and PrivateMemorySize64.
  * Every third sample it runs `memmap.exe` on each process.
* `memmap.exe` walks the address space with `VirtualQueryEx` and sums the
  committed bytes by kind:
  * private RWX, which is the fx code arena;
  * private read-only;
  * private in [4 GiB, 16 GiB), where the images are;
  * other private;
  * pages of copy-on-write views, split by the `QueryWorkingSetEx` Shared bit
    into written (private) and still the file's;
  * other mapped views.
* `mem_summary.py` prints the per-process peaks and the breakdown at each
  process's largest private snapshot.

### Per-process peak private (MB)

| Process | base-2 | base-3 | base-4 | new-1 | new-3 | final |
| --- | --- | --- | --- | --- | --- | --- |
| browser | 1827 | 1827 | 1833 | 1348 | 1325 | 1334 |
| renderers (6 busy ones) | 1901–1975 | 1897–2003 | 1900–1959 | 1436–1543 | 1442–1498 | 1440–1497 |
| Helper (Alerts) | 1164 | 1507 | 1531 | 1120 | 1010 | 901 |
| GoogleUpdater (4) | 6–132 | 6–134 | ≤134 | 61–122 | – | 46–122 |
| **tree peak private (GB)** | **12.28** | **13.27** | **11.38** | **9.91** | 8.59¹ | **10.05** |
| sum of per-process peaks (GB) | 14.60 | 15.68 | 15.71 | 12.02 | 10.38¹ | 11.49 |
| exit / `data-js` | 0 / ran | 2² / ran | 0 / ran | 0 / ran | 2² / ran | 0 / ran |

¹ This run ended early (the teardown watchdog), with fewer processes
sampled.

² Chrome's own "Teardown watchdog expired" at shutdown. It is a known stop
that is sensitive to load, owned by stage 2's w4, and it occurs on the
baseline too.

Every run's `network denied` lines named only `[2001:4860:4860::8888]:443`
and `https://update.googleapis.com`.

One more run, new-2, timed out: renderers died at
`message_pump_kqueue.cc:553 Check failed: kevent64, set timer: Invalid
argument (22)`.

* This is the negative `EVFILT_TIMER` interval under load.
* It is fixed on chrome-s4-g1 (2a3f643) and is not in main.
* It is unrelated to this change. Its memory numbers matched new-1 (renderers
  1433–1448 MB).

### Breakdown of one renderer (MB committed, base-4 → final)

| Kind | Before | After | What it is |
| --- | --- | --- | --- |
| Private in [4, 16) GiB | 496 | 256 | Before: the framework's 240 MB of segments plus 256 MB of other guest memory. After: the 256 only. |
| … of which read-only | 236 | 0 | `__TEXT`, `__DATA_CONST`, `__LINKEDIT` after `protect()` |
| Copy-on-write image pages, written | – | 13 | pages that binding wrote (mostly `__DATA_CONST`, `__DATA`) |
| Copy-on-write image pages, still the file's | – | 224 | shared with every process, not private |
| Other private | 1110 | 877 | −231: the 243 MB file buffer is gone. The rest is listed below. |
| … fx translation cache file (private read-only) | 452 | 453 | see the next section |
| … guest heap, V8, PartitionAlloc, stacks, host | ~658 | ~424 | the image buffer was in here |
| fx code arena (private RWX) | 285 | 286 | see the next section |
| Mapped views (sections, files) | 75 | 77 | |

### Blender headless check

Blender headless `-b --factory-startup` with a monkey, run with
`blender_headless_cpu.ps1`, 3 interleaved rounds:

* every run exits 0;
* process CPU is 8.45–8.58 s on the baseline and 7.31–8.11 s after (no
  segment copies).
* The load was 93–100 %, so the CPU numbers are only indicative.

## How it works

### `MachOImage::load` (src/alchemy/macho.cpp)

* It creates a read-only file section (`host::sections::create_file_section`)
  and a view of the whole file (`host::sections::map`).
* It parses the selected slice in place: `bytes_` is a span into that view,
  and `file_offset_` is the slice's offset in the file.
* Two cases still read the file as before:
  * a file whose huge `LC_SYMTAB` string table is omitted (the reserved-tail
    path);
  * a host without sections (the null backend).

### `MachOImage::map(memory, base, FileMapper)`

* **When it maps from the file:** when the image came from a file view and
  every segment is 4 KiB aligned in memory, in size and in the file (Mach-O
  arm64 segments are 16 KiB aligned).
* **How:** each segment goes to the engine's `FileMapper`:
  `FxEngine::map_file_copy(base, size, section, file offset, file size)`.
  It is all segments or none.
* **Fallback:** an engine without the capability (the reference engine) gets
  copied segments, as does any refusal, which falls back to the copy path.

### `fx::map_file_copy_guest_memory` (src/fx/runtime/shared_memory.cpp)

* **File bytes:** a `PAGE_WRITECOPY` view (`MapViewOfFile3`
  `MEM_REPLACE_PLACEHOLDER`) in fx placeholders.
  * Windows accepts 4 KiB file offsets for placeholder views; measured.
  * The rest of the last file page is cleared. Only a page that holds other
    file bytes is written, and so copied.
* **Zero-fill tail:** a pagefile section view.
* **Region:** one guest region, `Region::copy_on_write`.

### Protections

* A copy-on-write view refuses `PAGE_READWRITE`, even on a page already
  copied (measured).
* So the fx host's `vm::protect` retries writable protections as
  `PAGE_WRITECOPY` / `PAGE_EXECUTE_WRITECOPY` on a view whose
  `AllocationProtect` is write-copy.
* Engine protection changes, guest `mprotect` (Chrome's `PROTECTED_MEMORY`
  segment toggles RW/RO) and the SMC restore of a written page all go through
  it.

### Not shared memory

* The written pages exist only in this view.
* So `is_shared_memory`, `alias_memory` (`vm_remap` with `copy = FALSE`) and
  `duplicate_section` (Mach memory entries) treat image pages as before, when
  they were private.
* A partial munmap of an image segment fails explicitly. Mapping the parts
  again would drop the written pages.

### Placement

* Dylib bases are 64 KiB aligned (`granule_aligned` in module_loader.cpp). An
  image mapped from its file owns the host allocation granules it touches, so
  the next image must not start inside its first granule.
* 64 KiB is a multiple of Darwin's 16 KiB page, so it is a valid slide.

### Lifetime

* The loader calls `release_file()` after mapping. The engine holds its own
  duplicate of the section handle.
* An image file stays open while mapped, as on macOS. Windows then refuses
  writes to it. The test roots are read-only anyway.

## The fx translation cache file: shared too

### Before

* `pe::MappedFile::read` read every cache main file up to 1 GiB into private
  memory and made it read-only.
* Chrome Framework's `.fxc` is 470 MB, so each of the 9 framework processes
  in a run held its own 452 MB copy, about 4 GB of the tree.
* The reason given in src/fx/cache/cache.cpp: a warm process uses nearly
  every record, and one sequential read beats faulting a mapped view in page
  by page on the lookup path.

### Now

* Main files are opened with `MappedFile::open`, the path files over 1 GiB
  already took. It is a read-only view, so every process shares the file's
  pages.
* The open task prefetches the whole view on its pool thread
  (`host::vm::prefetch`, `PrefetchVirtualMemory`). The preloader and lookups
  find the pages resident instead of faulting them in one at a time.
* **Integrity and version checks are unchanged:**
  * `open_main` validates the header, the key (image hash, file size,
    translator and CPU features) and the index.
  * Lookups verify each record's checksum, the guest-code hash and the base
    rules, on the view as they did on the copy.
* **Merges:** a merge renames a new main file over a mapped one
  (FILE_SHARE_DELETE). A running process keeps its old view, and new
  processes map the new file. Checked by a Win32 probe and by
  `cache_main_file_is_a_shared_view`.
* `MappedFile::read` had no other caller and is removed.

### Measured

Strict headless `--dump-dom` again, part 1 (file-backed images) against part
2 (shared cache file too), interleaved, at 80–100 % load. Per-process peak
private in MB:

| | part 1 (c2) | part 2 (c1) | part 2 (c3) |
| --- | --- | --- | --- |
| Renderers (6 busy ones) | 1442–1517 | 988–1060 | 991–1053 |
| Browser | 1323 | 871 | 871 |
| Helper (Alerts) | 787 | 558 | 676 |
| **Tree peak private (GB)** | **9.23** | **6.56** | **6.64** |
| Sum of per-process peaks (GB) | 11.25 | 7.79 | 8.09 |
| Exit / `data-js` | 2³ / ran | 0 / ran | 0 / ran |
| Wall (s) | 44.1 | 57.0 | 46.8 |

³ The teardown watchdog again (load).

* **Where it went:** the cache file moved from private read-only (about 455
  MB per process) to a shared mapped view. It shows as about 530 MB "mapped",
  which is not private.
* **Blender headless** (`blender_headless_cpu.ps1`), 5 interleaved rounds,
  all exit 0:
  * process CPU, part 1: 11.08, 10.28, 10.64, 9.17, 9.53 s (median 10.28);
  * part 2: 9.52, 9.75, 8.42, 8.75, 10.89 s (median 9.52).
  * There is no slowdown. Load was 73–100 % in all but one round, so the
    numbers are indicative.

### From the start

From main's code to this branch, a renderer's peak private went from **~1.91
GB to ~1.00 GB**, the browser from **1.83 to 0.87 GB**, and the tree peak
from **11.4–13.3 GB to 6.6 GB**.

## The fx code arena: eager preloading (off in Alchemy child processes since 2026-10-20)

**What it costs:** about 288 MB private RWX per renderer, plus about 200 MB of
runtime bookkeeping for the regions installed into it.

### Can cached code execute from the shared view instead of being copied?

Not as the cache is laid out:

* `cachefmt::rebuild_region` copies a record's code and writes absolute guest
  addresses (base + rva) into it (`guest_address_imm64` fixups).
* Exits are rel32 branches that the runtime resolves into the arena when it
  installs a region. Direct-branch linking later patches installed code.
* The view is outside the arena's 1 GiB reservation, which keeps every stub
  and region in rel32 reach. It is also not executable.
* **What executing in place would need:** a code layout that is independent
  of the base and the arena, with guest addresses and exits reached through
  per-process tables, mapped copy-on-write inside the arena reservation. That
  is a translator and cache format change, not a contained one.

### Why the arena is this large: the preloader installs every record

* The preloader (src/fx/runtime/regions.cpp) installs every record of each
  module's main file ahead of the guest.
* The framework's main file holds what every Chrome process type has ever
  run: browser, renderers, utilities, the GPU process.
* So each process installs code it never runs. With `-Stats`, renderers that
  translate on demand use only 6–60 MB of arena; the browser uses 135 MB.

### Measured with `TRANSMUTE_FX_PRELOAD=0`

Same binary (part 2), interleaved, preload on and off:

| | on (pre-on-1) | off (pre-off-2) | on (pre-on-3) | off (pre-off-4) |
| --- | --- | --- | --- | --- |
| Tree peak private (GB) | 5.33 | 3.33 | 6.67 | 4.03 |
| Busy renderers (MB) | | | 988–1091 | 492–742 |
| Renderer fx arena (MB) | | | 287–298 | 22–109 |
| Browser (MB) | | | 874 | 611 |
| Wall (s) | 59.3 | 66.5 | 57.0 | 55.0 |
| Sum of "translate" over processes (s) | 7.8 | 36.5 | 12.6 | 15.9 |
| Exit / `data-js` | 2³ / ran | 2³ / ran | 0 / ran | 0 / ran |

* Without preloading, a renderer drops to about half (−450 MB each), and the
  tree to about 60 %.
* The cost moves onto guest threads: lookups and installs on demand, which
  is the "translate" time.
* Wall time didn't change measurably here, but the machine was at 100 % load.

### Changed: off in Alchemy child processes (2026-10-20)

* **What changed:** an Alchemy child process (`--alchemy-child-spec`, a guest's
  posix_spawn or fork/exec) sets `TRANSMUTE_FX_PRELOAD=0` before its engine
  starts, unless the variable is already set (`alchemy::child_preload_setting`).
  * `TRANSMUTE_FX_PRELOAD=<threads>` now decides either way
    (`fx::rt::preload_threads`).
  * A top-level process keeps preloading. Only the children of a multi-process
    app pay for code their app's other processes ran, so they are the ones that
    skip it.
* **Why not every process:** the first version turned preloading off for every
  process.
  * Against main, Blender's windowed first viewport draw was then 67 ms (3 %)
    later in all three pairs (2268 → 2335 ms median).
  * Its headless wall time was 1.9 → 2.0 s.
  * With children only, Blender matches main: 2387 / 2391 ms, and 1.9 / 1.9 s.
* **How it was measured:** one binary, preloading on (`TRANSMUTE_FX_PRELOAD=2`)
  against off for every process, interleaved, cloaked, on a quieter machine
  (5–30 % load before each run).

  | Check | On | Off |
  | --- | --- | --- |
  | Chrome (3 rounds): tree peak private | 12.6 / 12.7 / 12.6 GB | 3.86 / 3.86 / 3.86 GB |
  | Chrome: startup to the page target | 2.1 / 2.2 / 2.2 s | 2.2 / 2.2 / 2.2 s |
  | Chrome: load event, example.com | 0.74 / 0.95 / 0.92 s | 1.15 / 0.88 / 1.09 s |
  | Chrome: load event, Wikipedia article | 2.18 / 2.90 / 2.10 s | 2.17 / 2.09 / 2.14 s |
  | Chrome: load event, Hacker News | 1.34 / 1.61 / 1.37 s | 1.65 / 1.59 / 1.62 s |
  | Chrome: Speedometer 3, 1 iteration, JIT | 1.63 / 1.73 / 1.87 | 1.90 / 1.73 / 1.90 |
  | QLab (3 rounds): first window backing | 1.0 / 1.0 / 1.3 s | 0.9 / 1.3 / 1.0 s |
  | Renderer add-on cube (3 rounds): add-on enabled / render | 28.5 / 6.4 s (means) | 27.7 / 6.4 s |
  | Blender windowed (3 rounds), median: first frame / first viewport draw | 1740 / 3070 ms | 1716 / 3120 ms |
  | Blender headless (7 rounds), median: process CPU / wall | 2.80 / 2.5 s | 2.39 / 2.6 s |

* **What it costs:**
  * Small pages load up to about 0.2 s later in Chrome (example.com, Hacker
    News), because their renderers now install cached regions on demand.
  * Blender's first viewport draw is 50 ms (1.6 %) later, inside the run-to-run
    spread here but consistent against main (above). That is why only child
    processes skip preloading.
  * Elsewhere the difference is within the noise or in favour of off: less
    process CPU (no preloader threads), and Speedometer was not slower (mean
    1.84 against 1.74).
* **What it saves:** about 450 MB per Chrome process and two thirds of the
  tree. Each process's 1 GiB arena also starts nearly empty instead of 40 %
  full.

## The fx code arena: reclaim (2026-10-21)

### The problem

* The arena was a 1 GiB bump allocator that never freed code. V8 rewrites and
  discards its JIT code all the time. Stage 3's invalidation (SMC write faults,
  `sys_icache_invalidate`) killed the translations but never gave their space
  back.
* Long benchmarks therefore ended with `bad allocation`: Speedometer 3 after
  about 8 minutes, JetStream after about a minute.
* **Reproducer:** `scripts/chrome/pages/jit_churn.html`, a local page, so the
  offline flags apply.
  * Every round compiles 64 new functions with `new Function`, runs them hot
    enough for V8's optimizing tiers, and drops the oldest. Only 512 stay
    alive.
  * `?seconds=N` sets the run time. The title ends in `PASS ...` or
    `FAIL ...`, and every function's result is checked.
  * Run windowed with `run_windowed.ps1 -Jit` and
    `ALCHEMY_FX_STATS_PERIOD=15`.
* **On main (ebcedb1)**, the page's renderer died after about 50 s with
  `stopping after: bad allocation`.

  | Renderer at | 16 s | 31 s | 46 s | death |
  | --- | --- | --- | --- | --- |
  | fx arena used (MB) | 328 | 642 | 978 | 1048, the whole reservation |
  | process private (MB) | 739 | 1249 | 1689 | 1764 |

  At death 948 MB of the 1048 MB arena was dead code (90 %), with 43,791
  regions installed and 20,082 invalidated. Region metadata was never freed
  either.

### The design

The full note is in the wave notes folder, `fx-arena-reclaim.md`; the
implementation notes are in src/fx/runtime/NOTES.md, "Code arena" and "Arena
reclaim".

* **Chunks.** The arena is cut into 2 MiB chunks.
  * Permanent chunks hold the stubs, thunks, trampolines and adapters. They
    are never freed.
  * Code chunks hold translated regions, in two streams: *volatile* for JIT
    code of registered guest code memory, *stable* for image and cache code.
  * A region's metadata and its tier-0 counter belong to its chunk and are
    freed with it.
  * Free chunks are decommitted.
* **Reclaim.** A background thread runs passes:
  1. **Pick victims.** A victim is a code chunk with at most a quarter of its
     code still live. The live rest is evicted: killed like an invalidation,
     but the guest code is unchanged, so it comes back from the JIT's kept
     translations or the cache on its next miss. When free chunks run low,
     the passes evict more.
  2. **Scan.** Every guest thread is suspended. Its registers, host stack and
     context are scanned; any word inside a victim pins it.
  3. **Free.** Unpinned victims are freed. Pinned ones are tried again in a
     later pass.
* **Retired regions.** A tier-0 region that a tier-1 region fully replaced
  counts as dead for its chunk.
* **Rejected designs.**
  * *A full flush at a high-water mark* still needs the same reachability
    check, because threads block in host calls for minutes. It throws away
    all hot code at once, and one pinned region blocks the whole reset.
  * *A free list of dead regions* fragments, puts a variable-size allocator
    on the install path, and doesn't bound live code.
* **What pinned chunks.** The first build saw stale words pin chunks. Two
  sources were removed:
  * `ctx->exit_arg`, a link exit's field, is now cleared after the exit;
  * the stack below the dispatcher is zeroed after translations, and below
    a reclaim wait before it.

  After that the pins came from real references (a thread inside the code).
  `TRANSMUTE_FX_RECLAIM_TRACE=1` names each pin's thread and where the word
  was.

### Measured: the churn page, 900 s (windowed, `-Jit`, cloaked, offline, machine at 80–100 % load)

| | main ebcedb1 | first policy¹ | this branch |
| --- | --- | --- | --- |
| Result | renderer killed at ~50 s (`bad allocation`) | PASS, 196 rounds in 903 s | **PASS, 283 rounds in 907 s**² |
| Renderer peak private | 1764 MB at death | 1673 MB | **610 MB** |
| Renderer fx arena | 1048 MB at death | 770–870 MB late in the run | **93 MB at 64 s, 83 MB at 305 s, 122 MB at 911 s** |
| Region metadata | (not counted; never freed) | 368–407 MB | 72 MB at 911 s |
| Reclaim passes | – | 8, 0.39 s | 164, 5.4 s (0.6 % of the run) |
| Threads stopped (all passes) | – | 0.018 s | 1.23 s (0.14 %) |
| Reclaimed | – | 993 MB | 2.7 GB |
| Evicted / retired regions | – | 218 / – | 20,233 / 14,472 |
| Pinned victims (kept for a later pass) | – | 0 | 138 |

¹ The first build reclaimed only chunks that were *entirely* dead, or evicted
when free space ran low. In Chrome few chunks die entirely: long-lived code,
such as V8's copied builtins and hot functions, sits among the dead. So it
held about 800 MB until the low-space path started.

² An intermediate build ran 388 rounds and peaked at 589 MB. It killed a
retired tier-0 region at once and interrupted every thread instead.
Throughput varies with the machine's load, which was 80–100 % throughout.

* The renderer's arena stays near 100 MB. Its private memory grows from 397
  MB at 64 s to 570 MB at 911 s. That growth is the host heap (150 → 223 MB)
  and V8's own memory, not the arena.
* JIT translation, not reclaim, is the renderer's fx time: 389 s of JIT and
  41 s in the SMC fault path, against 5.4 s in reclaim passes.
* Evicted regions come back as JIT translations or reused kept ones:
  re-translations 84,014, of them 6,828 reused unchanged.

### Measured: no churn (strict headless `--dump-dom probe/test.html`, jitless)

| Per-process peak private (MB) | main round 1 | new round 1 | main round 2 | new round 2 |
| --- | --- | --- | --- | --- |
| browser | 1112 | 1287 | 1267 | 1285 |
| busiest renderers | 390, 390 | 383, 390 | 417, 390 | 383, 389 |
| tree peak (GB) | 2.20 | 2.55 | 2.80 | 2.78 |

* Equal within the spread. Short runs never reclaim; the browser holds up to
  about 20 MB more, from a partly filled metadata block per chunk.
* Root process CPU over 3 interleaved rounds: main 25.9 / 25.8 / 24.9 s, new
  24.3 / 27.4 / 22.6 s.

### Tests and regression (main ebcedb1 against this branch, interleaved, cloaked, loopback, 76–100 % load)

* **Units:** `fx_runtime_tests` 97/97. The new `runtime_reclaim_tests`
  cover:
  * dead chunks freed, decommitted and reused;
  * a thread spinning in a dead region keeping its chunk until it leaves,
    then finishing in that code;
  * eviction under an arena budget, after which every function returns its
    value again;
  * a 4-thread stress (at least 4 s, load-tolerant): calls, SMC rewrites of the called code, and
    explicit passes on top of the budget's. Every call returned the
    function's old or new value. About 260 passes reclaimed 2.5 GB.

  The tier-up test also checks that the replaced tier-0 region is retired.
* **ctest, all 216 tests** (`-j12`, on this branch merged with main 99de0f4):
  213 pass. The 3 failures are load-sensitive:
  * `transmute_core_tests_2` times out; it is the older transmute core and
    doesn't link fx;
  * `fx_abi_tests` fails only `bench_thunk_overhead`, a timing threshold;
  * `fx_runtime_tests` failed only the reclaim stress test's activity floor:
    fewer than 100 rewrites in 4 s at this load. Its correctness check held.
    The test now runs until it has enough activity (at most 60 s).

  Earlier runs also failed `transmute_core_tests_1/3`, which fail on a
  serial rerun too (concurrency-overlap checks of the older core, no fx).
  These failed too and pass when rerun one at a time: `fx_translator_tests` (only
  `core_perf_translation_speed`), `alchemy_gui_metal_triangle_capture`,
  `alchemy_gui_network`, `alchemy_gui_bonjour_fixture`,
  `alchemy_gui_quartzcore_compositing`, `alchemy_host`,
  `alchemy_gui_security`, `alchemy_services(_process)` and
  `alchemy_gui_appkit_windows`.
* **Regression:**

  | Check | main ebcedb1 | new | Equal |
  | --- | --- | --- | --- |
  | Chrome strict headless `--dump-dom`, jitless, 3 runs | exit 0, `data-js="ran"` ×3; root CPU 25.9 / 25.8 / 24.9 s | exit 0, `data-js="ran"` ×3; root CPU 24.3 / 27.4 / 22.6 s | yes |
  | Chrome windowed (`run_windowed.ps1 -Frames 2 -Settle 20`), 3 runs | PASS, `Browser.close`, exit 0 ×3 | PASS, exit 0 ×3 | yes |
  | QLab to its welcome window, 2 runs | welcome window `backing-5.bmp` 3B0D89F748905800…, ×2 | byte-identical ×2 | yes |
  | Renderer add-on headless cube, 2 runs | exit 0; renderer created; render 9.7 / 8.3 s | exit 0; renderer created; render 9.3 / 7.9 s | yes |
  | Blender windowed (`scripts/perf/startup.ps1`), 3 runs: first frame presented | 6111 / 4862 / 3971 ms, exit 0 ×3 | 5651 / 4747 / 5316 ms, exit 0 ×3 | within the spread at this load |
  | Blender headless, 7 rounds: process CPU, median | 6.88 s | 6.91 s | yes |

* **After merging main 99de0f4:** Chrome headless 3/3 (exit 0, `data-js="ran"`)
  and windowed 3/3 (PASS, exit 0) again.
* **`network denied` lines** named only the known targets:
  * Chrome: `[2001:4860:4860::8888]:443` and `https://update.googleapis.com`;
  * QLab: `https://accounts.figure53.com`;
  * The renderer add-on: its license server's address.
* **Not run here:** Speedometer and JetStream. They are public sites, so the
  real-web session runs them.

### Startup commit (the ~460 MB per process)

* **Children:** their arena is now 6–45 MB at startup. They no longer preload
  (6db5f0d), and chunks commit lazily, in 256 KiB steps.
* **The top-level process** (Chrome's browser) still preloads every cached
  region: 557 MB of arena plus 357 MB of region metadata in these runs. That
  is the "preloaded regions' bookkeeping" below, now counted.
* **Not changed.** Bounding the browser's preload is a startup-speed trade
  (the 2026-10-20 measurements above). It must be measured on a quiet
  machine, and this one was at 80–100 % load throughout. Two options:
  * a preload budget per process;
  * evicting stable chunks the guest never entered once the preload queue
    drains. This needs an entry bit per region, so it's a translator change.

## What remains

Per renderer, with preloading on:

* about 430 MB of other private memory: the preloaded regions' bookkeeping,
  the guest heap, V8 and PartitionAlloc, stacks, the host runtime;
* 256 MB committed in [4, 16) GiB that is not images: unattributed, probably
  a guest reservation committed up front.

## Tests

* **Units:**
  * `fx_runtime_tests`: 83/83, including the new
    `shared_memory_file_copy_views`.
  * `alchemy_macho_tests`, including the new `test_file_mapping`.
  * `fx_host_tests`, `fx_loader_tests`.
  * `fx_cache_tests`: 29/29, including the new
    `cache_main_file_is_a_shared_view`.
  * `fx_cache_aot_tests`.
* **ctest** passes for the following:
  * `alchemy_macho`, `alchemy_mach_vm`, `alchemy_loader_expansion`,
    `alchemy_host_boundary`;
  * `alchemy_fx_engine`, `alchemy_process_model_fx`,
    `alchemy_shared_memory_fx`, `alchemy_process_tree_fx` and its
    `_rebased` variant;
  * all 14 `alchemy_gui_objc_fx_*`.
* **Registration audit** (`ALCHEMY_REGISTRATION_AUDIT=1 alchemy-fx doctor` on
  Chrome): identical to main. There are 2 pre-existing runtime duplicates,
  `_printf` and `_puts`, and 0 skipped.
* **Not run here:**
  * the full `ctest -R "^alchemy_"`. Most of its executables and fixtures
    were not built in this worktree;
  * QLab, the renderer add-on and Blender windowed;
  * the Linux host backend.
