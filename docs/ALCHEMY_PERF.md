# Alchemy performance: Blender.app orbit frame rate and startup (2026-10-04)

The work covers the official macOS ARM64 Blender 4.5.14 (Metal only) on
`alchemy-fx.exe`. Measurements used an Intel i7-1355U with Iris Xe graphics
(driver 32.0.101.7085) on a 60 Hz display, with a 1600x900 client area and a
FIFO swapchain. Branch `perf/w1` holds the work. "Baseline" means the build of
`gui/m4` at `29c640f`. "New" means `perf/w1`. Both are Release builds with
PDBs. Every timing below gives the machine's total CPU load at the time,
because other agents were compiling in parallel. Counts per frame do not
depend on load.

## Results

### Orbit (continuous middle-button orbit with real `SendInput`, `scripts/m4/perf_orbit.ps1`, 3 x 8 s per session)

| Build | Session 1 (fps) | Session 2 (fps) | Load during the runs |
| --- | --- | --- | --- |
| Baseline | 41.6, 40.5, 37.3 | 33.8, 37.8, 35.0 | 37-76 %, then 100 % |
| New | 54.7, 58.3, 58.0 | 55.3, 55.6, 58.7 | 72 %, then 84-100 % |
| Native x64 Blender | 59.8, 58.0, 59.6 | 59.1, 58.3, 51.0 | 45-100 %, then 64-100 % |

The sessions were interleaved: baseline, new, native, then the same again.
An earlier pair taken at 30-45 % load gave baseline 30.9 and 32.4 fps, new
54.4 and 53.7 fps, and native 58.1 and 57.0 fps. The new build now runs at the
display rate, the same as native. Logs and `perf.json` files are under
`artifacts/alchemy-blender/perf-w1/cmp-*`.

### Input to present

- **Measured inside the process (`ALCHEMY_PERF`).** The clock starts when the
  first input message of a frame reaches the window procedure and stops at the
  next present. While orbiting, the median is 9-13 ms, p90 is 12-15 ms, and the
  maximum is 18 ms. Measured from the message's posting time
  (`GetMessageTime`, 16 ms resolution), the median is 16 ms.
- **Measured by the harness (`perf_orbit` latency samples).** The median fell
  from 47-50 ms (baseline) to 31-36 ms (new). This figure overstates latency.
  PowerShell polls `present-stats.json`, and the presenter's watcher rewrites
  that file at most every 100 ms when frames change. The harness cannot measure
  native Blender at all.

### Per frame while orbiting (`ALCHEMY_PERF=2`, main thread)

| | Before (26.9 fps, 37 ms frame) | After (48-56 fps) |
| --- | --- | --- |
| Main-thread time in host calls (outermost) | 32.3 ms | 15.6 ms, of which 4.2 ms is the idle `usleep` and 1.9 ms is the event wait |
| `-[NSApplication sendEvent:]` | 10.4 ms | 0.1 ms |
| `+[NSScreen screens]` + `-visibleFrame` | 12.8 ms (6.2 calls each, about 1 ms per call) | below 0.05 ms |
| `nextEventMatchingMask:` | 3.5 ms | 1.9 ms |
| `renderCommandEncoderWithDescriptor:` (22 per frame) | 1.8 ms | 1.25 ms |
| `__tlv_bootstrap` (6,780 per frame) | 1.6 ms | 1.0 ms |
| `vsnprintf` (2 per frame) | 1.4 ms | below 0.1 ms |
| `posix_memalign` + `free` (870 + 1,020 per frame) | 2.7 ms | 1.2 ms |

These counts per frame are identical before and after:

| Area | Count per frame |
| --- | --- |
| `objc_msgSend` | 1,540 |
| Slow lookups through the dispatcher | 0 |
| Host-side cache probes from host-to-guest sends | 271 |
| Native IMP calls | 353 |
| Synthetic host calls | 14,700 (6,780 of them `__tlv_bootstrap`, 1,020 `free`, 970 `strlen`, 870 `posix_memalign`) |
| Objects | 78 allocations, 78 deallocations |
| `ObjectStore` lookups | 8,770 before, 6,500 after (`descriptor_u64` does one lookup instead of two) |
| Autorelease pools | 61 pushes, 61 pops |
| Command buffers | 3 command buffers and 3 commits |
| Encoders | 22 render, 3 compute, 3 blit |
| Draws and dispatches | 74 draws, 5 dispatches |
| `setBytes` | 139 calls, 16.5 KB |
| Descriptor sets | 152 sets, 336 descriptor writes, 151 argument tables |
| Pipeline binds | 55 |
| Vulkan commands | 636 dynamic-state commands, 177 descriptor and vertex binds, 39 barriers |
| Queue submits | 4 |
| Presentation | 0.2 ms present, 0.03 ms `nextDrawable` |
| Mid-frame pipeline creations | 0 |
| Mid-frame MSL compiles | 0 |

GPU waits are 3 per frame, totalling 10.6 ms. They happen on the completion
thread, not the main thread.

The sampling profile (`TRANSMUTE_FX_SAMPLE=...,main,stacks`, 50,864 samples,
before the fixes) splits the main thread as follows:

| Main-thread time | Share |
| --- | --- |
| Translated guest code | 11.2 % |
| Waits (`ntdll`) | 39.9 % |
| Host code | 48.0 % |

Inside the host code, the largest items were:

| Item | Share |
| --- | --- |
| `NSScreen` monitor enumeration (`win32u`, DCs, display configuration) | 6-8 % |
| Guest malloc and `posix_memalign` (`VirtualAlloc`/`Free` per call, `std::map`) | 7 % |
| `invoke_synthetic`, `load_state`, `store_state` | 6 % |
| printf's zero-filled 1 MiB buffer per conversion | 5 % |
| `ObjectStore` and the Objective-C payload lookup | 4 % |
| `ProcessContext::thread` (mutex + `std::map` on every TLV/errno access) | 2.3 % |
| `GetGUIThreadInfo` in every run-loop step | 2.6 % |

### Startup (process creation to the first 3D viewport draw, `scripts/perf/startup.ps1`)

| Build | Warm | Cold (`ALCHEMY_FX_CACHE=0`, fresh MSL and pipeline caches) |
| --- | --- | --- |
| Baseline | 8.2, 9.4, 8.2 s (load 20-55 %) | 31.1, 34.6 s (load 37-47 %); 43.9 s (load 83 %) |
| New | 3.07, 2.98, 2.83, 2.60 s (load 19-20 %); 5.6 s (load 42 %) | 11.7 s (load 28 %), 14.6 s (load 30 %) |
| Native x64 Blender | 3.18, 2.64, 2.49 s (load 17-23 %) | - |

The warm start is now as fast as native. The user's earlier figure was 13-15 s
(taken under load).

Phases of a warm start with the new build (ms since process creation):

| Phase | ms |
| --- | --- |
| alchemy main | 87 |
| Main executable read and parsed (175 MB, one `ReadFile`) | 386 |
| fx runtime initialized | 388 |
| Main image mapped; Metal prefetch starts | about 410 |
| Metal device created (in the background: Vulkan instance and device, 4.5 MB `VkPipelineCache`) | 630 |
| ~200 dylibs mapped | 967 |
| Bound | 1,153 |
| Objective-C prepared, images published | 1,161 |
| Initializers done, guest main | 1,233 |
| `-[NSApplication finishLaunching]` | 1,293 |
| First NSWindow | 1,398 |
| First `newLibraryWithSource:` (an MSL cache hit) and first `VkPipeline` | 1,417 |
| First present | 1,568 |
| First Python extension module | 1,803 |
| The `--python` startup script runs (bpy imported, startup file loaded) | 2,231 |
| First 3D viewport draw | 2,833 |

For comparison, native reaches its startup script at 1,760 ms and its first
viewport draw at 2,487 ms.

Phases of a cold start with the new build:

| Phase | ms |
| --- | --- |
| Loader done | 1,058 |
| Startup script | 2,854 |
| First viewport draw | 11,694 |

About 200 MSL libraries compile cold, using 54 s of CPU over about 9 s of wall
time. Blender compiles 142 of every 205 libraries on its own worker threads,
so the compiler runs about 5 compiles in parallel. fx translates 29,441 regions
(2.76M guest instructions) in 0.9-1.5 s.

## What changed (commits on `perf/w1`)

1. **`ALCHEMY_PERF` instrumentation.** See `include/alchemy/perf.hpp` and
   `src/alchemy/perf.cpp`.
   - `ALCHEMY_PERF=1` counts. `ALCHEMY_PERF=2` also times every synthetic host
     call (rdtsc, main-thread inclusive time). With either value off, each hook
     costs one branch.
   - Counters cover:
     - `objc_msgSend`, through counting entry stubs that increment a guest word
       before the dispatcher;
     - slow lookups by message;
     - `ObjectStore` allocations, frees and lookups;
     - synthetic calls per slot, named `-[Class sel]` or by symbol;
     - Metal encoders, draws, bindings, descriptor sets and writes, argument
       tables, upload bytes, dynamic state, barriers, submits, and CPU waits
       with their time;
     - presentation: slot wait, acquire, queue, and `nextDrawable`;
     - run-loop waits and guest sleeps;
     - MSL compiles, cache hits and lookup time;
     - pipeline creations and their time.
   - Reports:
     - startup phases from process creation, printed at the first frame and at
       exit; a guest line `ALCHEMY_PERF_PHASE <name>` adds a phase;
     - per-frame summaries every 2 s (`ALCHEMY_PERF_PERIOD`), with top-N lists
       of host calls by time and by count and of slow lookups;
     - input-to-present latency;
     - a summary at exit, plus fx translation-cache statistics.
   - `ALCHEMY_PERF_OUT` writes the report to a file.
2. **Loader.**
   - Segments are copied with one `write_bytes` per segment instead of one
     `write8` per byte. This was about 6 s of a warm start: 175 MB, plus about
     200 dylibs.
   - The image file is read with one `ReadFile` into a buffer that the image
     then takes over, instead of being read through an `ifstream` and copied.
   - `run_program` passes its parsed executable to the loader, so the image is
     no longer read twice.
   - `existing()` compares file identities instead of calling
     `std::filesystem::equivalent` against every loaded module.
   - `exported()` looks up a direct export without allocating.
3. **NSScreen data is cached in the window server.** Before, every
   `+screens` call and every property re-enumerated the monitors, and Blender
   asks about 12 times per frame (12.8 ms). The cache is dropped on
   `WM_DISPLAYCHANGE`, `WM_DPICHANGED` and work-area changes, and is checked
   against a generation number. This is the main orbit fix: 31-41 fps became
   54-59 fps.
4. **Guest sleeps** (`usleep`, `sleep`, `nanosleep`) use a per-thread
   high-resolution waitable timer. Before, `Sleep` rounded Blender's 5 ms idle
   sleep up to 14-16 ms.
5. **Guest malloc.**
   - The allocation table is a sharded `alchemy::hash_map`. Before it was a
     `std::map` plus an `unordered_set` under the services mutex.
   - `posix_memalign` (alignment up to 4 KiB, up to 64 KiB) takes an
     over-allocated engine-heap block. Before, it made a guest mapping per call
     (`VirtualAlloc`/`VirtualFree`, about 900 per frame).
   - `calloc` no longer zero-fills twice.
6. **printf family** formats into a 512-byte stack buffer instead of a
   zero-filled 1 MiB vector per conversion. Each call cost 0.3-0.8 ms, and
   startup makes 868 `snprintf_l` calls.
7. **`memcmp`, `memchr`, `strcmp` and `strncmp`** read page-bounded chunks,
   with a byte-wise fallback near the end of a mapping. `strlen` uses
   `string_length`. Before, each made one checked read per byte, and startup
   makes 833k `strcmp` and 347k `strlen` calls.
8. **`ProcessContext::thread`** is a per-thread cache, checked against a
   generation number. Before, every `__tlv_bootstrap`, errno and libSystem call
   took the process mutex and a `std::map` lookup. Synthetic calls dispatch
   through a lock-free handler table, with no shared lock and no `shared_ptr`
   copy per call.
9. **Run loop.** `GetGUIThreadInfo` runs only inside a window procedure,
   because modal loops can only start there (`win32_nesting.hpp`).
10. **Startup.**
    - Once the main image is mapped, two background tasks start: creating the
      Vulkan device and loading the `VkPipelineCache`, and preloading the MSL
      result cache. `MTLCreateSystemDefaultDevice` then finds the device ready.
      This does not happen for `-b`, `--version` or `--help`.
      `ALCHEMY_METAL_PREFETCH=0` turns it off.
    - Guest mappings and dylib placement retry if another host thread takes
      their range first.
    - `stat(path)` opens one handle instead of calling `_wstat64` and
      `host_metadata`. Startup makes 1,512 calls, about 0.3 ms each.
    - `stringByAppendingString:` concatenates UTF-16 directly.
    - UTF-8 conversion of ASCII `NSString`s is a narrowing copy. Blender's MSL
      sources are about 100 KB.
    - `gettimeofday`'s scratch memory comes from the guest heap.
11. **`VkPipelineCache` is saved from a submission** 2 s after the last
    pipeline creation. GUI harnesses kill the process, so `~Device` never saved
    it before.
12. **Hash containers.** tsl::robin_map is vendored behind `alchemy::hash_map`
    (next section).

## Cache audit

| Cache | State | Evidence |
| --- | --- | --- |
| (a) fx translation cache for Mach-O images | **Works.** `FxEngine::register_image` registers each image with `fx::register_external_module`, keyed by LC_UUID. It is on by default (`ALCHEMY_FX_CACHE=0` turns it off). Files are under `%LOCALAPPDATA%\transmute\fx-cache\<image>.<key>.fxc` plus `.fxj` journals. The main binary's file is 80 MB, and the directory holds 1.6 GB in all, because older fx builds leave files under old keys. | Warm GUI start: 55 modules, 48 with cache files, 36,366 lookups, 36,360 hits, 6 new translations (0.06 s) and no rejected files or records. Cold: 29,441 translations in 0.9-1.5 s. Warm starts no longer retranslate Blender. `docs/ALCHEMY_FX.md` was out of date on this point; it is now corrected. |
| (b) MSL result cache | **Works.** `%LOCALAPPDATA%\Alchemy\msl\results\<two 64-bit hashes>.bin` holds about 355 results (4.3 MB). The key is the compiler version, which includes the stdlib fingerprint and the clang version, plus every `CompileOptions` field and the source hash and size. The full key is stored in the file and compared on load. A run now preloads every result of the current compiler version into memory on a background thread (352 results). | Warm: 205 of 205 `newLibraryWithSource:` calls hit (`msl_cache_hits=205`, `msl_compiles=0`). Lookup time across the 205 calls was 650 ms, plus 297 ms of source conversion. After the preload and ASCII fast path, the 12 libraries before the first frame take 0.4 ms in all. Cold: about 200 compiles. |
| (c) metal_stdlib PCH and macros | **Works.** `%LOCALAPPDATA%\Alchemy\msl\stdlib-<fingerprint>\metal_stdlib-<lang>-fast-<clang>.pch` is 4.8 MB per language version (1.1, 2.2, 2.3, 2.4, 3.1). The predefined-macro sets sit beside them. | Built once, on 2026-10-03. A cold run with a fresh `ALCHEMY_MSL_CACHE` rebuilt two of them in 1-3 s. Note: `stdlib_pch()` and `predefined_macros()` hold one global mutex while clang builds a missing PCH, which serializes concurrent cold compiles for that time. |
| (d) `VkPipelineCache` | **Works, now saved reliably.** `%LOCALAPPDATA%\Alchemy\metal-pipeline-cache-8086-a7a1.bin` is 4.5 MB. The driver validates the header (vendor, device, UUID). An invalid file falls back to an empty cache. | It loads 4,502,156 bytes. Warm runs create 12 pipelines in 0.7-1.8 ms in all, because the driver cache hits. Cold: 0 bytes loaded; saves at 3.9 KB, 139 KB, 189 KB and 687 KB. Before, it was saved only every 5 s during creation and in `~Device`, which a killed process never reaches. |
| (e) Blender's own caches | **Fine.** The macOS 4.5 Metal backend keeps no shader binary cache; it calls `newLibraryWithSource:` every start, and (b) and (d) cover that. Asset-library indices are under `%TEMP%\Blender\asset-library-indices`. | `-[MTLBinaryArchive serializeToURL:]` saves the `VkPipelineCache`. Note: each killed run leaves a `%TEMP%\blender_XXXX` session directory (306 now). |
| (f) Python `.pyc` | **Works.** The extracted bundle is writable on Windows. Python wrote 852 `cpython-311.pyc` files into the bundle's `__pycache__` directories on its first start and reuses them. | No `.pyc` was written during the 15 measured starts after 15:00. The newest is from 10:46. |

## Hash containers

`include/alchemy/hash_map.hpp` provides `alchemy::hash_map` and
`alchemy::hash_set`.
- They are `tsl::robin_map` and `tsl::robin_set` from
  `third_party/robin-map`: v1.4.1, commit `bd14e683`, MIT license, vendored
  unmodified. Provenance and SHA-256 hashes are in
  `third_party/robin-map/PROVENANCE.md` and `runtime/THIRD_PARTY_NOTICES.md`.
- `-DALCHEMY_ROBIN_MAP=OFF` switches them back to `std::unordered_map` and
  `std::unordered_set`.
- The header also provides `address_hash` (a 64-bit mix), `string_hash` and
  `string_equal` (heterogeneous `string_view` lookup).
- robin_map does not keep elements in place. Inserts and erases invalidate
  references, so only tables that store values were switched.

| Table | Before | Profile share (main thread, before) | Now |
| --- | --- | --- | --- |
| `ObjectStore` shards | `unordered_map<u64, shared_ptr>` + mutex | 2.3 % (`find`/`require`, under `host_payload` 3.8 %) | robin_map |
| Guest malloc table | `std::map` + `unordered_set` under the services mutex | 7.3 % (`allocate` 4.3 %, `release` 3.0 %, including `VirtualAlloc`) | 32 shards of robin_map (plus the heap `posix_memalign`) |
| Per-thread TLV cache (`ThreadContext::tlv_addresses`) | `unordered_map<u64, u64>` | 0.7 % | robin_map |
| `ProcessContext::threads_` | `std::map` + process mutex per call | 2.3 % | per-thread cache in front (the map stays) |
| pthread mutex, condition, rwlock, once and guard tables | `std::map<u64, shared_ptr>` | 0.3 % (under `pthread_mutex_lock`) | robin_map |
| Metal descriptor property index and class table | `std::map<std::string>` | 0.4 % (`Resolved::find`) | robin_map with `string_hash` |
| Synthetic call handlers | `vector<shared_ptr>` + `shared_mutex` | about 2 % (`RtlAcquireSRWLockShared` + refcount) | lock-free pointer table |
| fx engine region map (`FxEngine::Shared::regions`) | `std::map` (ordered, needs `upper_bound`) | 0.6 % | kept: needs ordering |
| Objective-C class, selector and IMP maps | custom lock-free `AddressMap` (open addressing) | below 0.1 % | kept |
| Loader export index | `unordered_map<string_view, size_t>` | startup only: 7 % of `--version` samples | `string_view_hash_map` with stored hash (2026-10-11), plus the earlier fix: lookups that do not allocate |
| `ProcessContext::exports_` | `std::map<pair<string, string>>` | startup, below 1 % | `string_hash_map` with a combined key (2026-10-11) |
| Objective-C selector and type-string tables | `std::map<string>` | below 0.1 % | `string_hash_map` (2026-10-11) |

`tools/hash_map_bench.cpp` measures each container on the same access
patterns. Load was 45 %, and each figure is the best of 5 runs:

| Pattern | `unordered_map` | robin_map | Speed-up |
| --- | --- | --- | --- |
| ObjectStore (9 finds + erase + insert, 64k live) x200k | 199 ms | 57 ms | 3.5x |
| Malloc table (find + erase + insert, 200k live) x1M | 932 ms (`std::map`: 3,168 ms) | 379 ms | 2.5x (8.4x over the old `std::map`) |
| TLV cache (find, 300 keys) x5M | 93 ms | 53 ms | 1.8x |
| Export index (`string_view` find, 25 % misses, 100k keys) x1M | 728 ms | 855 ms | 0.85x |

Expected gain: the integer-keyed tables Alchemy hits per frame were about 3-5 %
of main-thread time. Most of that is gone now, and the remainder is the
mutexes. Vendoring tsl::robin_map was the right choice: it is header-only, has
a maintained upstream and has the iterator semantics described above. A
project-authored robin-hood map would duplicate it with no gain.

**Correction, 2026-10-11: string keys.** The 0.85x "export index" figure above
was noise from a run under heavy contention. Three reruns of
`tools/hash_map_bench.cpp` (best of 5 per pattern):

| Run (load) | `unordered_map` | robin_map | robin_map, `StoreHash=true` |
| --- | --- | --- | --- |
| 1 (42 %) | 233 ms | 162 ms (1.44x) | 141 ms (1.66x) |
| 2 (22 %) | 228 ms | 158 ms (1.44x) | 145 ms (1.57x) |
| 3 (14 %) | 320 ms | 265 ms (1.21x) | 239 ms (1.34x) |

This agrees with the coordinator's own rerun: 1.29-1.68x for `StoreHash=true`.

`include/alchemy/hash_map.hpp` therefore adds three containers for string keys,
behind the same `ALCHEMY_ROBIN_MAP` switch:
- `alchemy::string_hash_map<T>` and `string_hash_set`: robin_map/set with
  `string_hash`, `string_equal` and `StoreHash=true`;
- `string_view_hash_map<T>`: the same, for keys owned elsewhere.

Tables moved to them:
- the loader export index (`string_view` keys into the image's export list,
  which never changes after parsing);
- `ProcessContext::exports_`: it was a `std::map<pair<string, string>>` that
  built two strings per lookup; it is now one `"<library>\x1f<symbol>"` key,
  and lookups reuse a per-thread key buffer;
- the Objective-C selector table and type-string table.

The class-name and protocol-name tables stay ordered, because
`objc_copyClassList` and `objc_copyProtocolList` enumerate them. The MSL cache
map stays as it is: about 200 lookups per run.

Startup effect: none measurable. The bind phase of `--version`, measured 5
times each and interleaved, had a median of 219 ms before and 220 ms after
(load 18-42 %). Binding time is dominated by fixups and by the
dependency-scope walk, not by the string lookups themselves.

## Requests to the fx owner

These are in `docs/ALCHEMY_FX.md`:
- Mach-O cache keying is done and working.
- New request: a profiler measurement window (start delay, or
  `fx::profiler_reset`) and an atomic report rewrite.
- New, low priority: background AOT translation for registered external
  modules, which would remove 0.9-1.5 s of translation from cold starts.

## Second round (2026-10-11, commit `ec14182`)

The second round worked through the ranked list from the first round. Per-frame
figures come from `ALCHEMY_PERF=2` while orbiting at 60 fps (display rate),
with 21-27 % load:

| Item | Before | After |
| --- | --- | --- |
| `__tlv_bootstrap` | 6,780 host calls per frame, 1.0 ms | A guest fast path reads the thread's TLS block from a table at TPIDR_EL0, indexed by module id. It calls the host handler only on a miss, which then fills the table. Calls in steady state: about 0 |
| Synthetic calls per frame | 14,700 | 7,900 |
| Integer-only entries | Every call copied 32 V registers in and out | malloc, free, `posix_memalign`, memcpy, the `str*` functions, pthread mutexes, errno and `objc_retain`/`release` copy only the integer state. `posix_memalign`: 0.76 → 0.17 µs per call. `free`: 0.49 → 0.14 µs |
| `renderCommandEncoderWithDescriptor:` | 57 µs (descriptor read 0.23 ms per frame) | 12 µs. One descriptor lookup per attachment and one for the pass (`descriptor_read`), 0.10 ms per frame. `ObjectStore` lookups: 6,500 → 4,300 per frame |
| `-[MTLBuffer setLabel:]` | 63 µs (it copied the `NSString`) | below the top-30 cutoff. Copying an immutable CF string is a retain |
| Main-thread host time per frame | 15.6 ms at 48-56 fps | 15.3 ms at 60 fps. Most of it now waits: about 10.5 ms in `vkAcquireNextImageKHR` (FIFO) inside `commit` → `present` |

Orbit frame rate with the second-round build:

| Run | Alchemy (fps) | Comparison (fps) | Load |
| --- | --- | --- | --- |
| Instrumented runs `step2`/`step3` | 58.2-60.2 | - | 21-27 % |
| Interleaved runs, 2026-10-11 (`compare2.txt`) | new 44-58 | baseline 28-41, native 38-62 | 55-100 %, with up to 8 compilers running |

During the interleaved runs the harness itself was starved: it sent 280-390
moves instead of about 490.

Measured but not changed:
- **Descriptor-set and argument-table writes** total 0.125 ms per frame (152
  sets). Caching sets whose bindings did not change would save at most that.
- **Encoder begin**, including its full barrier and dynamic-state resets,
  costs 0.13 ms per frame on the CPU. Removing the barriers would change GPU
  synchronization, so it needs GPU timing before it is worth the risk.

### Cold MSL

- `stdlib_pch()` and `predefined_macros()` now lock each language
  configuration separately. A cold PCH build no longer blocks compiles of
  other configurations.
- **Seed sets.**
  - `alchemy-fx msl-seed <corpus> <executable> <root> [threads]` compiles an
    `ALCHEMY_MSL_DUMP` corpus. It writes the results, in the cache's own file
    format, to `<root>/<LC_UUID of the executable>/results`.
  - Metal consults that directory after a cache miss
    (`ALCHEMY_MSL_SEED=<root>`, default `%LOCALAPPDATA%\Alchemy\msl-seed`,
    `0` disables it) and copies each hit into the user cache.
  - Dumps now record `subgroup_size`.
  - A seed holds Blender's GPL shaders compiled, so it is never committed. It
    is generated locally under `artifacts/alchemy-blender/msl-seed`.
  - For the pinned Blender, 257 corpus libraries compile in 50 s on 6 threads.
- **Cold start with a fresh MSL cache, the seed, and no fx or pipeline cache.**
  195 of 199 libraries come from the seed and 4 compile. The first viewport
  draw came at 11.1 s (load 64 %) and 19.4 s (load 93 %). Without the seed it
  came at 21.1 s (load 51 %) and 28.0 s (load 100 %). What remains of the cold
  path is fx translation (29,440 regions) and pipeline creation without a
  `VkPipelineCache`.

## Remaining hotspots (ranked)

1. **Vsync-bound orbit.** The display rate is reached. Further gains need a
   heavier scene and GPU timing. Candidates: the full barrier per encoder,
   and the present blit plus capture copy (`ALCHEMY_PRESENT_CAPTURE` is on in
   the harness).
2. **The rest of a cold start.**
   - fx translation of 29k regions, 0.9-1.5 s. The fx request is filed.
   - Pipeline creations with an empty `VkPipelineCache`.
   - The 4 libraries the corpus lacks; re-dump with `ALCHEMY_MSL_DUMP` to
     extend it.
3. **Startup file I/O.**
   - `stat`, `open` and `read` cost 0.3-0.7 ms per call here, because of the
     endpoint-security hooks.
   - `dlopen` takes 6-10 ms per Python extension.
4. **Descriptor sets and the encoder begin** (0.25 ms per frame together;
   see above).
5. **Environment.** An endpoint-security DLL (`InProcessClient64.dll`) hooks
   `RtlAllocateHeap` and file opens. It accounts for 10-12 % of startup
   samples, for native Blender as much as for Alchemy.

## Tools

- `ALCHEMY_PERF=1|2`, `ALCHEMY_PERF_OUT`, `ALCHEMY_PERF_PERIOD`: see above.
- `scripts/perf/startup.ps1 -Target alchemy|native [-Cold] [-Repeat N]`, with
  `scripts/perf/startup_probe.py`: startup timeline as JSON.
- `scripts/m4/perf_orbit.ps1`: orbit frame rate. This branch did not change
  it.
- `TRANSMUTE_FX_SAMPLE=<file>,1000,main,stacks`: fx sampling profiler, which
  needs the PDB beside `alchemy-fx.exe`. Copy the report while it runs,
  because a killed process can leave it empty.
- `tools/hash_map_bench.cpp`: container micro-benchmark.
