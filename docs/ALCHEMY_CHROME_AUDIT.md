# Google Chrome 155 (macOS arm64) audit

This is a first measurement of what the official macOS Google Chrome needs
from Alchemy. It is a probe and an inventory, and nothing was implemented for
Chrome. [ALCHEMY_CHROME_GAPS.md](ALCHEMY_CHROME_GAPS.md) has the complete
import and Objective-C inventory, and the numbers below come from it.
Statements about Chrome's internals come from Chromium's public source
design. They are marked *(design)* where no run confirmed them. Chrome's
code was not disassembled, and no guest binary was changed.

## Real web and performance (2026-10-20)

Chrome 155 under Alchemy on 20 public websites and three browser benchmarks, against a native Windows Chrome of the
same version (155.0.8059.85) on the same laptop.

* **Machine:** a 13th Gen Intel Core i7-1355U (10 cores, 12 threads), 32 GB, Intel Iris Xe, on AC power.
* **Branch:** `claude/chrome-real-web-a7ede1`.
* **Alchemy runs:** windowed, cloaked, multi-process, V8's JIT on unless stated, a fresh anonymous profile per run
  (`<root>\Users\realweb\profiles\`).
  * The site matrix ran with ANGLE and Graphite on SwiftShader, so WebGL works.
  * The benchmarks ran with Chrome's software compositor (`--disable-gpu`), which is about 9x faster under Alchemy
    (gap 3).
* **Native runs:** kept their hardware GPU.
* **No input:** nothing was typed, submitted or downloaded. The only clicks were a consent wall's "Reject all" on
  Google Maps and YouTube, made by a page script over DevTools.
* **Tools:** `scripts/chrome/web_visit.ps1` (one browser, DevTools only), `web_matrix.ps1` (the sites) and
  `web_bench.ps1` (the benchmarks, native and Alchemy interleaved).
* **Updated the same afternoon** (Follow-up, below):
  * SwiftShader frames went from 10 to 27 per second (gap 3).
  * Child processes no longer preload, and a tree peaks at about 4 GB (gap 4).
  * YouTube's AV1 decoding fails under fx (gap 5).

### Worst gaps first

1. **The fx code arena fills, and a JIT renderer dies.** Speedometer's renderer stops with `bad allocation` about 2
   minutes in (9 minutes with SwiftShader), and JetStream's after 60 s.
   * The renderer's last stats line: arena 1,048,544 KiB of the 1 GiB reserve, **510,590 KiB of it dead** (invalidated
     translations), 65,438 translations, 38,509 SMC faults.
   * The arena is bump allocated and never reclaimed (`src/fx/runtime/arena.cpp`). Each V8 code patch kills
     translations whose space stays used.
   * Any page that runs JIT-heavy JavaScript for long enough hits it. Jitless V8 does not.
2. **The JIT makes DOM-heavy pages slower, not faster, under fx.**
   * Speedometer: 1.30 with the JIT (first iteration, mean of 3), 4.45 jitless (first iteration), so 3.4x slower
     with the JIT.
   * Hot loops (JetStream tests) gain 2–12x from the JIT (Benchmarks).
3. **Frames under SwiftShader: about 6.5 per second** (10.4 on a quieter machine). Since `5332015` it is about 27 per
   second (Follow-up). A page that only moves a 50-pixel box with `requestAnimationFrame`:

   | Configuration | rAF callbacks | Window composites |
   | --- | --- | --- |
   | Native Chrome (hardware GPU) | 2,101 in 35 s (60 per s) | – |
   | Alchemy, software compositor (`--disable-gpu`) | 865 in 15 s (58 per s) | 986 |
   | Alchemy, GPU process with ANGLE and Graphite on SwiftShader | 97 in 15 s (6.5 per s) | 118 |

   * The GPU process rasterizing on SwiftShader under fx is the bottleneck.
   * MotionMark never got past its frame-rate check in that mode, and YouTube video stops in it (gap 5).
   * The software compositor is the faster mode for Chrome under Alchemy today. WebGL and Maps' vector map need
     the SwiftShader one.
4. **Every Alchemy process carries about 1 GB of private memory.** A Chrome tree peaks at 10–12.7 GB private
   (median 10.3 GB over the matrix), against 0.3–1.2 GB for native Chrome on the same pages.
   * Most of it was the arena preloader's copy. With child processes not preloading, the tree peaks at
     about 3.9 GB (Follow-up).
   * Memory maps of three live processes (a browser, a renderer and a utility) show, in each:
     * 460–490 MB of private executable memory: the fx code arena;
     * 550–790 MB private read-write;
     * about 1.1 GB of read-only file mappings.
   * The arena is filled from the persistent translation cache: a renderer's 149,596 cache hits copy about 400 MB
     of translated code into its own arena.
     * Every process gets its own copy of the same code.
     * The copy already uses 40 % of the arena before a page runs any JavaScript, which is why gap 1 arrives so
       soon.
5. **YouTube video with SwiftShader.** A renderer's media worker calls `IOSurfaceSetValue(NULL, ...)` and stops, so
   the video does not play. With the software compositor the same page plays Big Buck Bunny (8.6 s at 854x480, 573
   frames, 100 dropped). See Open, item 3.
   * Later: YouTube's AV1 stream does not decode under fx in any mode (dav1d returns EINVAL). The software compositor
     plays only because the player falls back to VP9 (Follow-up).
   * Since `1f5705a` it plays with SwiftShader too: Alchemy now ignores the NULL surface, as macOS does, and
     supports the planar NV12 IOSurfaces that come next (Follow-up).

### What works

* **All 20 sites load,** with no renderer crash and no Alchemy stop other than the two below (table).
* **Network:**
  * TLS 1.3 with the post-quantum hybrid key exchange (Cloudflare's trace: `tls=TLSv1.3`,
    `kex=X25519MLKEM768`).
  * HTTP/2 everywhere. No HTTP/3 connection happened with fresh profiles, though UDP itself worked (STUN to
    ports 3478 and 19302).
  * DNS through `getaddrinfo`, cookies, and Mojo data pipes between the network service and the renderers.
* **Rendering:**
  * Web fonts (Google Fonts).
  * CJK text and ruby through host-font fallback.
  * Emoji text.
  * Scrolling long pages.
  * WebGL map tiles (Google Maps' vector map).
* **Media:** YouTube video with the software compositor; the audio service's sync sockets after `a18f84b`.
* **Quitting:** every run of the final round quit through `Browser.close` with exit 0.

### The network, and what stayed blocked

* **Policy:** `ALCHEMY_NETWORK=full`, with two layers of refusals for update, crash-upload, variations, metrics and
  GCM hosts.
  * **Chrome's resolver:** `--host-resolver-rules` maps them to `~NOTFOUND`.
  * **Alchemy's deny list:** the new generic `ALCHEMY_NETWORK_DENY` (`7fb5b06`) refuses them in every process of
    the tree. That covers GoogleUpdater, a separate process that ignores Chrome's flags.
  * **The hosts:** `update.googleapis.com`, `*.update.googleapis.com`, `gvt1.com`, `*.gvt1.com`,
    `clients2.google.com`, `clients4.google.com`, `clientservices.googleapis.com`, `dl.google.com`,
    `*.dl.google.com`, `tools.google.com`, the crashpad hosts, `android.clients.google.com` and
    `mtalk.google.com`.
  * **Chrome's own switches:** `--disable-component-update`, `--disable-background-networking`, `--no-pings`,
    `--metrics-recording-only`, `--disable-breakpad` and `--disable-domain-reliability` stayed on.
* **`ALCHEMY_NETWORK_DENY=<host>,...`** (`network_policy.hpp`): exact names, `*.<domain>`, or numeric addresses.
  * Names fail at every name gate: getaddrinfo, gethostbyname, libresolv, DNSServiceGetAddrInfo, WinHTTP (for
    NSURLSession and CFNetwork) and PAC.
  * Numeric entries also fail connect, sendto and sendmsg.
* **`ALCHEMY_NETWORK_LOG=1`:** prints every distinct name and address a gate lets through.
* **The proof, before the first real site:** Chrome on a local page with the network open, idle for 90 s, with
  Chrome's netlog and Alchemy's log.
  * GoogleUpdater's `WinHTTP POST https://update.googleapis.com` was refused by the deny list.
  * Chrome's own update and time requests (`update.googleapis.com`, `clients2.google.com`) resolved to `~notfound`
    in the netlog and opened no connection.
  * Allowed traffic went only to `accounts.google.com`, `www.google.com`, `www.gstatic.com` and
    `android.clients.google.com`. GCM's check-in host went onto the list after this run.
* **Over all 85 runs of the day with the network open** (sites, benchmarks, diagnostics):
  * The only `network denied` line was GoogleUpdater's update POST.
  * No listed host appeared among the allowed names or addresses, apart from GCM in the proof run.
  * Nothing was downloaded or installed into `artifacts/alchemy-root-chrome`.

### Site matrix

The final round, on `e74374a`, with SwiftShader and the JIT.

* **Load event:** from `Page.navigate` to `Page.loadEventFired`, so DNS, TLS, the page and its subresources.
  Example.com, Wikipedia search and the Wikipedia article ran while the test build used 56–100 % of the CPU; the
  other sites started at 3–25 %.
* **First frames:** frames 1–3 of each window capture and a screenshot of each page were kept outside git.
* **No native load times:** native Chrome under the same harness took 9 s to its first page target and up to 25 s
  to example.com's load event, with every request already finished. That is a harness or host artefact this
  session did not resolve, so there is no load-time comparison.

| Site | Loaded | Load event (s) | Protocol | Renderer crashes | Peak private (GB) | Notes |
| --- | --- | ---: | --- | ---: | ---: | --- |
| example.com | yes | 7.4 | h2 | 0 | 10.2 |  |
| Wikipedia article (Alan Turing), scrolled | yes | 22.48 | h2 | 0 | 10.0 |  |
| Wikipedia search results | yes | 35.45 | h2 | 0 | 9.9 |  |
| BBC News, scrolled | yes | 16.35 | h2 | 0 | 11.5 |  |
| GitHub repository (torvalds/linux) | yes | 21.18 | h2 | 0 | 10.4 |  |
| GitHub file view | yes | 23.53 | h2 | 0 | 10.4 |  |
| MDN JavaScript | yes | 12.46 | h2 | 0 | 10.2 |  |
| docs.python.org (asyncio) | yes | 4.16 | h2 | 0 | 10.1 |  |
| YouTube front page | yes | 20.69 | h2 | 0 | 12.5 | consent dialog drawn; `vm_remap` refusal (non-fatal) |
| YouTube video page | yes | 18.3 |  | 0 | 12.7 | (since fixed: `1f5705a`) the video does not play in this mode: a renderer worker stops on `IOSurfaceSetValue(NULL)`; with the software compositor it plays (Open, item 3) |
| Google search results | yes | 11.08 | h2 | 0 | 10.4 | Google's reCAPTCHA page ("unusual traffic"), drawn correctly; not bypassed |
| Google Maps (Paris), consent rejected | yes | 5.24 | h2 | 0 | 10.1 | vector map drawn (WebGL on SwiftShader) |
| Google Fonts specimen (web fonts) | yes | 6.85 | h2 | 0 | 10.5 |  |
| Japanese Wikipedia (CJK) | yes | 9.87 | h2 | 0 | 10.2 | CJK text and ruby drawn through host-font fallback |
| Wikipedia "Emoji" (emoji text) | yes | 8.25 | h2 | 0 | 10.4 |  |
| Hacker News | yes | 2.39 | h2 | 0 | 10.2 |  |
| The Guardian, scrolled | yes | 18.53 | h2 | 0 | 10.2 |  |
| react.dev | yes | 10.34 | h2 | 0 | 10.5 |  |
| Stack Overflow question | yes | 5.73 | h2 | 0 | 10.3 | `vm_remap` refusal (non-fatal): a GPU command buffer ring buffer |
| Cloudflare /cdn-cgi/trace | yes | 2 | h2 | 0 | 10.1 | `tls=TLSv1.3`, `http=http/2`, `kex=X25519MLKEM768` |

* **Earlier rounds:** they found the bugs fixed below.
  * Round 1 (after `caaf624`): 9 of 20 sites hit `+[NSLocale ISOLanguageCodes]`, and 7 of those renderers died
    before the load event. Wikipedia search's renderer died on `fmsub`.
  * Round 2: the video capture service crash loop, Google Fonts (`dateFormatFromTemplate:`, then `-monthSymbols`)
    and the audio service's fileport CHECK.

### Benchmarks

* **Setup:**
  * `web_bench.ps1`, native then Alchemy for each benchmark and run, each started after three load samples under
    30 %.
  * Alchemy ran the frozen binary `e74374a` (all fixes) with the software compositor.
  * The browsers' own load during a run was 23–59 % (median of 10 s samples).
* **Why one Speedometer iteration:** with the JIT, Speedometer 3.0 ran with `?iterationCount=1` on both sides. Its
  official score averages 10 iterations, and an Alchemy JIT renderer does not live that long (gap 1).

| Benchmark | Native Chrome 155 (hardware GPU) | Alchemy, JIT | Alchemy, jitless | Alchemy / native |
| --- | --- | --- | --- | --- |
| MotionMark 1.3 (medium), 3 runs | 1389.2 / 1415.0 / 1377.5 | 140.2 / 139.1 / 142.6 | – | 10.1 % |
| Speedometer 3.0, 1 iteration, 3 runs | 23.56 / 23.34 / 21.51 | 1.274 / 1.242 / 1.390 | 4.45 (the first of the 10 below) | 5.7 % (JIT), 19.5 % (jitless) |
| Speedometer 3.0, 10 iterations | 27.7 ± 1.2 / 28.1 ± 1.4 (earlier the same day) | does not finish (gap 1) | **4.48 ± 0.28** | 16 % (jitless) |
| JetStream 2.2, all tests | 321.5 (earlier the same day) | does not finish (gap 1, 60 s) | – | – |

**JetStream 2.2 tests**, one per fresh browser (`?test=<name>`, one run each, the same binary):

| Test | Native | Alchemy, JIT | Alchemy, jitless | JIT / jitless |
| --- | ---: | ---: | ---: | ---: |
| richards | 1038.2 | 220.2 (21.2 %) | 17.7 (1.7 %) | 12.4x |
| delta-blue | 1847.7 | 170.3 (9.2 %) | 32.8 (1.8 %) | 5.2x |
| crypto | 1392.2 | 204.0 (14.6 %) | 19.9 (1.4 %) | 10.3x |
| Box2D | 496.4 | 39.6 (8.0 %) | 20.5 (4.1 %) | 1.9x |
| navier-stokes | 1095.3 | 202.2 (18.5 %) | 25.4 (2.3 %) | 7.9x |
| splay | 389.1 | 117.8 (30.3 %) | 49.6 (12.7 %) | 2.4x |
| geometric mean of the ratios | | 15.3 % | 2.8 % | |

* **Hot, long-lived code (JetStream's tests):** the JIT under fx is worth 2–12x over jitless and reaches about 15 %
  of native.
* **DOM and framework churn (Speedometer):** the JIT costs 3.4x. V8 recompiles short-lived code, and each rewrite is
  an SMC fault, an invalidation and a retranslation (one renderer: 38,509 faults, 36 s in the fault path).
  * Which mode is faster depends on the page. Only the JIT mode is limited by the code arena.
* **MotionMark at 10 %** is the renderer rasterizing on the CPU and the software compositor, against native's GPU.
* **The JIT ablation** asked for is the jitless column above.

### What was fixed

Each fix comes with a test that fails on the old code, in the provider that owns the API.

| Commit | Area | What Chrome hit | Fix |
| --- | --- | --- | --- |
| `caaf624` | libSystem | **Every HTTPS load failed** with `ERR_NETWORK_CHANGED`. Chrome calls `_sendto` as a two-register function on macOS 13.3–26.0 and treats a nonzero x1 as a known kernel bug (crbug.com/400642488). Alchemy left the caller's buffer address in x1. | Syscall-style services return x1 = 0, as XNU's syscall return leaves it. |
| `6695cbb` | Foundation, CoreFoundation, host | `+[NSLocale ISOLanguageCodes]` was unsupported. Renderers died before the load event on GitHub, MDN, YouTube, the Guardian and Wikipedia's Emoji page. | `host::locale::locales()` (EnumSystemLocalesEx); `CFLocaleCopyAvailableLocaleIdentifiers`, `CFLocaleCopyISOLanguageCodes`, `...ISOCountryCodes`, `...ISOCurrencyCodes` and `...CommonISOCurrencyCodes`; the matching NSLocale class methods. |
| `e15c7da`, `cd3d959` | fx translator | A Wikipedia search page's renderer stopped with `fp: guest vector read after write` on `fmsub s0, s0, s0, s29` (`0x1f00f400`). | Emitter invariant failures now name the instruction word and pc. In the FMADD family, a memory factor that is the destination is loaded before the destination is claimed. |
| `3001b6a`, `0ce7dfa` | Foundation | Google Fonts' renderer died on `+[NSDateFormatter dateFormatFromTemplate:options:locale:]`, then on `-monthSymbols` (Blink's date fields). | TR35 skeletons map to en_US patterns, matching the formatter's en_US styles. Added the month, weekday and era name arrays. |
| `914f5d4` | AVFoundation | YouTube restarted the video capture service 40 times, each start a SIGSEGV reading address 0. | `AVCaptureDeviceTypeExternal`, `...ContinuityCamera` and `...Microphone` exist from macOS 14.0, the version Alchemy reports. They were weak imports bound to NULL. |
| `a18f84b` | Mach | The audio service sends a renderer its sync socket as a fileport. `fileport_makeport` refused sockets, and Mojo's CHECK ended the process. | Socket fileports: each owns a socket of its own and makes a `WSADuplicateSocket` blob per recipient and per `fileport_makefd`. |
| `e74374a` | libSystem VM | **JetStream ended the whole browser** 15 s in: `mach_make_memory_entry_64` returned `KERN_RESOURCE_SHORTAGE`. Each Mojo data pipe is a 2 MiB memory entry, and the 256 MiB per-process cap on Mach VM pages from the initial import was reached. | The cap is now 4 GiB, as a guard against runaways only; Darwin has none. |
| `d4c8b96` | libSystem | Chrome's tracing service stopped on `strftime("%z")`, so `--trace-startup` lost its trace. | `%z` and `%Z` from the Darwin `tm` (`tm_gmtoff`, `tm_zone`). |
| `5332015` | libSystem threads | The SwiftShader GPU process spent about 1.7 cores in `VirtualQuery`: every pthread mutex, condition, rwlock and once operation re-validated its guest address. | A known object is looked up first; only a new address is checked. rAF frames 10.4 → 26.6 per second (Follow-up). |
| `1f5705a` | IOSurface | With SwiftShader, YouTube's video never started: Chrome's `IOSurfaceCanSetColorSpace` sets values on a NULL surface by design, and the provider stopped the media thread; then `IOSurfaceCreate` refused the frame's planar NV12 surface. | A NULL surface is ignored, as on macOS. Planar surfaces (`kIOSurfacePlaneInfo`), their `*OfPlane` accessors and cross-process planes. |
| `19393e9` | fx runtime, alchemy-fx | Each Chrome process installed about 450 MB of other processes' cached code into its arena. | Alchemy child processes don't preload cached regions (`TRANSMUTE_FX_PRELOAD` set explicitly wins); top-level apps are unchanged. Tree peak 12.6 → 3.9 GB. |
| `7fb5b06` | host network policy | No way to open the network while keeping one app's updater off it. | `ALCHEMY_NETWORK_DENY` and `ALCHEMY_NETWORK_LOG` (above). |

`0cf0842`, a `vm_remap` of shared memory over mmap'd pages, was reverted (`57deea7`). It worked in the unit
fixture's engine but not under fx, where a shared alias needs a placeholder reservation, and it had already
unmapped the target. See Open, item 4.

### Follow-up: the open items dug into (2026-10-20, afternoon)

#### SwiftShader frames: a VirtualQuery on every pthread mutex operation

* **How it was found:** a Chrome trace and the fx sampling profiler.
  * The trace: `--trace-startup` with `--trace-startup-format=json`.
  * Its first run lost the trace: Chrome's tracing service stopped on `strftime("%z")`. Fixed in `d4c8b96`:
    `strftime` and `strftime_l` now format `%z` and `%Z` from the Darwin `tm`'s `tm_gmtoff` and `tm_zone`, as
    `wcsftime` already did.
  * The trace showed the GPU main thread busy for 95 % of the time, in raster (`DoEndRasterCHROMIUM`, 81 ms a call)
    and `SwapBuffers`.
  * The profiler showed that same thread on the CPU only about 30 % of the time. About 60 % it waited on a SwiftShader
    condition variable, and 15–20 % was `VirtualQuery`.
  * Over the GPU process, `VirtualQuery` took about 1.7 cores of samples, mostly on SwiftShader's threads.
* **The cause:** every `pthread_mutex_lock` and `unlock`, and every condition, rwlock and once operation, checked
  the guest address with `query_region` before it looked the object up. Under alchemy-fx that check is a
  `VirtualQuery` for mmap'd memory.
* **The fix (`5332015`):** the object table is looked up first; only a new address is checked.

  | rAF test page, SwiftShader GPU-process mode, 25 s | Before | After |
  | --- | --- | --- |
  | rAF callbacks, 3 interleaved runs | 270 / 252 / 253 (10.4 per s) | 714 / 574 / 707 (26.6 per s) |
  | Speedometer 3, 1 iteration, JIT, software compositor | 1.494 / 1.464 / 1.734 (mean 1.56) | 1.714 / 1.613 / 1.682 (mean 1.67) |

* **What remains:** after the fix, the GPU process uses about 1.4 cores. That is roughly half translated
  SwiftShader and Skia code, and half provider calls; the rest of the time it waits.
  * The display link runs at 62.5 Hz (`ExternalBeginFrameSourceMac::OnDisplayLinkCallback`, 938 in 15 s), so frames
    are not held back by pacing.
  * Per frame on the GPU main thread: raster 16 ms, `SwapBuffers` 12 ms (8 ms of it ending the IOSurface
    accesses).
  * That is SwiftShader's own work under translation: fx code quality, not an Alchemy wait.

#### Memory: more per process, not more processes

The final matrix round, per site at the tree's peak:

| | Alchemy (20 sites) | Native Chrome (4 sites) |
| --- | --- | --- |
| Processes at the peak | 12–15 | 10–11 |
| Tree peak private | 9.9–12.7 GB | 0.3–0.5 GB |
| Renderer, each | 1.10–1.20 GB | 25–42 MB |
| GPU process | 1.30–1.38 GB | – |
| Browser | 1.20–1.22 GB | 46–51 MB |
| Network service | 1.08–1.09 GB | – |
| Storage service (idle) | 1.06 GB | – |

* An idle storage service holds 1.06 GB, so most of each process's gigabyte is a floor that every Alchemy process
  pays, not the page's work.
* Per process, from the earlier memory maps:
  * the fx code arena, filled by the preloader from the shared translation cache: 288–450 MB private executable;
  * the runtime's bookkeeping for those installed regions: about 200 MB;
  * the rest of the 550–790 MB of private read-write: the guest heap and the process's own data.

#### `vm_remap` of shared memory over mmap'd pages: why fx differs from the unit engine

* **The case:** the refusal now names its target (`90a150c`). On YouTube it was `vm_remap(target 0x2c6e00000000,
  size 0x10000, ...)`.
  * The target is 4 GiB aligned, inside one of V8's large mmap'd reservations: its pointer-compression cage or
    sandbox, mostly PROT_NONE.
  * On Stack Overflow and YouTube's front page the next line is the renderer's
    `CommandBufferHelper::AllocateRingBuffer() failed` (`kFatalFailure`). That is a GPU context created on the
    renderer's main thread (WebGL or an accelerated canvas), lost while the page itself loads.
* **Why the unit engine and fx differ:**
  * The unit engine (ReferenceEngine) keeps guest memory as its own regions, so a range can be unmapped and an
    alias put in its place.
  * fx gives guest addresses the host's address space directly. A Darwin mmap reservation is one `VirtualAlloc`
    reservation (`FxEngine::Shared::reserve_granules`), and Windows can neither split nor partly release one.
  * fx's shared memory maps views only into placeholders (`MapViewOfFile3 MEM_REPLACE_PLACEHOLDER`,
    `src/fx/runtime/shared_memory.cpp`).
  * A 64 KiB view inside a 4 GiB reservation therefore cannot be placed. The reverted `0cf0842` unmapped the
    target first and then failed, leaving a hole.
* **What a proper fix needs (an fx memory-model change, not done here):**
  * Darwin reservations made as placeholders (`MEM_RESERVE_PLACEHOLDER`).
  * Each 64 KiB granule replaced by a reservation of its own when it is first committed (`provide`, the
    demand-commit hook). `for_each_allocation_piece` already handles ranges that span reservations.
  * `release_empty`, `host_free` and `decommit_ours` taught that a reservation can be made of placeholder pieces and
    granule reservations.
  * An ownership hand-off with `fx::rt` shared memory, so that an alias can take an uncommitted placeholder piece
    and give it back when it is unmapped. Today fx's shared memory releases a reservation once it is all
    placeholder again.
  * A test in the fx engine tests: a large PROT_NONE reservation, a shared view aliased into its middle, stores
    seen both ways, then unmap and reuse.
  * A follow-up session is proposed for this.

#### The arena preloader: off in Alchemy child processes

* `TRANSMUTE_FX_PRELOAD=0` had measured about 0.45 GB less per renderer, and the change waited on a quiet-machine
  comparison.
* That comparison, one binary with preloading on and off, interleaved:
  * Chrome's tree peak: 12.6 → 3.9 GB.
  * Startup and Speedometer: no worse.
  * Small pages load up to 0.2 s later.
  * QLab, the renderer add-on and Blender windowed and headless: within noise.
* **First version (`19393e9`), off for every process:** it cost Blender 67 ms (3 %) of windowed startup against
  main, consistently.
* **What landed:** only Alchemy child processes skip preloading; Blender, QLab and the renderer add-on match main. The rAF page's
  tree then peaks at 3.9 GB: the browser keeps 1.25 GB and the children are 0.2–0.6 GB.
* `TRANSMUTE_FX_PRELOAD` set explicitly wins either way. The full table is in `ALCHEMY_MEMORY.md`, "The fx code
  arena".
* **It does not cure gap 1.** With an arena that starts nearly empty, Speedometer (10 iterations, JIT) still ends its
  renderer with `bad allocation`, about 170 s in instead of about 120 s.
  * So the arena fills from JIT invalidation churn: 510 MB dead in the earlier stats line.
  * The reclaim design (the separate fx-arena-reclaim task) remains the fix.

#### YouTube video: two causes

The `video` action now waits for its `<video>` element, and it records the media players' errors and warnings from
CDP's Media domain (`a08692c`, `3e4d7db`, `e588837`). With the JIT:

| Mode | Result |
| --- | --- |
| Software compositor | **Plays**: 854x480, 1,391 frames, 130 dropped. YouTube first tries AV1, dav1d fails, and the player falls back to VP9. |
| GPU process on SwiftShader, before `1f5705a` | Does not play (readyState 0). The media thread first dies on `IOSurfaceSetValue(NULL)`. |
| SwiftShader, `--disable-gpu-memory-buffer-video-frames` | No `IOSurfaceSetValue(NULL)`. AV1 still fails. |
| GPU process on SwiftShader, `1f5705a` | **Plays**: 854x480, 646 frames in 9 s, 260 dropped, through the VP9 fallback. |

1. **AV1 does not decode under fx (open).**
   * The errors: `dav1d_get_picture() failed with error -22` on the first small non-key frames. With
     `--video-threads=1` it is `dav1d_send_data() failed with error -22` on the second frame, so it is not threading.
   * fx's code-generation switches don't change it: the translation cache off, `TRANSMUTE_FX_NEON=old`,
     `TRANSMUTE_FX_CORE=notier2`, no tier-up.
   * Alchemy reports no DotProd or I8MM, so dav1d runs its baseline NEON code. A mistranslated NEON routine is the
     likely cause.
   * Next step: dav1d's `checkasm`, built for arm64-apple-macos and run under alchemy-fx, compares each NEON routine
     with its C version. It needs the dav1d source downloaded, which has not been approved yet.
2. **`IOSurfaceSetValue(NULL)` in SwiftShader mode (fixed, `1f5705a`).**
   * **How it was found:** a `gpu` and `media` trace. The first AV1 frame decodes, then
     `VideoDecoderStream::PrepareOutput` (the renderer's `MappableSharedImageVideoFramePool`) begins and never ends.
     No shared image is created for the frame.
   * **The failing key:** the provider's error now names its key. It was `IOSurfaceColorPrimaries`.
   * **Chrome 155's source** (`155.0.8059.85`): `CreateHardwareFrame` first asks `gfx::IOSurfaceCanSetColorSpace`,
     and that function is `internal::IOSurfaceSetColorSpace(nullptr, color_space)`
     (`ui/gfx/mac/io_surface.cc`). It sets the colour primaries, transfer function and matrix on a NULL surface on
     purpose, and relies on macOS ignoring the call. Alchemy's `IOSurfaceSetValue` stopped the thread instead.
   * **Software compositor:** the pool exists only with a GPU process, so that mode never reached it.
   * **Fixes:**
     * `IOSurfaceSetValue` ignores a NULL surface.
     * The next stop was `IOSurfaceCreate` of the frame's NV12 ('420v') surface, which Alchemy refused as planar.
       IOSurfaces now support `kIOSurfacePlaneInfo`: per-plane size, element size, row bytes, offset and size in one
       allocation; the `*OfPlane` accessors; planes in the cross-process description.
     * Tests: `quartz2d_unit` (a NULL surface) and `quartzcore_compositing` `iosurface_planar`.

### Follow-up 2 (2026-10-20, evening): fresh numbers, memory, a second site batch

#### Benchmarks on main `4cd8a0c`

JIT, software compositor, Alchemy only (the native numbers above are unchanged).

| Benchmark | This morning | `4cd8a0c` |
| --- | --- | --- |
| Speedometer 3, 1 iteration (3 runs) | 1.274 / 1.242 / 1.390 (mean 1.30) | 1.914 / 1.778 / 1.718 (mean 1.80, +38 %) |
| JetStream tests, geometric mean of the ratios to native | 15.3 % | 17.4 % |
| Speedometer 3, 10 iterations (3 runs) | `bad allocation` after about 120 s | `bad allocation` after 170 / 150 / 150 s |
| JetStream 2.2, full (3 runs) | the renderer ended after about 60 s | `bad allocation` after 125 / 115 s; one `SIGBUS` (below) |

* **The JetStream tests**, against native: richards 245 (1038), delta-blue 224 (1848), crypto 180 (1392), Box2D 43
  (496), navier-stokes 257 (1095), splay 147 (389).
* **Benchmark progress (`3f77efe`):** the driver now writes the progress into the page title every 2 s, so a run whose
  renderer dies says how far it got.
* **New, intermittent:** `SIGBUS` at the same guest pc (`Google Chrome Framework+0x3b97a8`), a protection failure on a
  guest write to `0x2f0...`.
  * Seen in 2 of 11 long JIT runs, and not reproduced in 4 more.
  * MAP_JIT memory is read-write-execute for every thread under Alchemy, so V8's per-thread write toggle is not the
    cause.
  * The fault report now names the host protection, fx's write protection and the guest region (`c1882d8`), so the
    next occurrence says which.
  * Found and fixed in Follow-up 3 (`da0ce47`).

#### The idle storage service: 1.06 GB → 152 MB, and where the rest goes

* **Two fixes:**
  * Preloading off in child processes (`6db5f0d`) took it to 209 MB.
  * `6c6d869`: `MachOImage::bind()` now frees the image's parsed fixups. Chrome Framework's 1.5 million fixups, 40
    bytes each, had stayed in every process's host heap after binding. 209 → 152 MB, and every Chrome process saves
    the same 57 MB.
* **The tools (`c1882d8`):**
  * `ALCHEMY_FX_STATS` with `ALCHEMY_FX_STATS_PERIOD` prints a memory line per process: guest reservations, guest heap,
    fx arena, process private, host heap.
  * `ALCHEMY_PERF_MEMORY=1` prints the private bytes and host heap at each startup phase.
  * `host::process::memory_usage()` backs both.
* **The storage service now** (live, 20 s):

  | Part | MB |
  | --- | ---: |
  | Host heap | 55 |
  | … of which provider registration (every Alchemy process) | 26 |
  | … the fx cache module index | 7 |
  | Guest memory (Chrome's own: reservations and heap) | 26 |
  | fx code arena | 14 |
  | fx runtime tables (the 8 MB lookup table among them) and other host allocations | about 57 |
  | Private, in all | 152 |

* **Any Alchemy process:** a minimal one is about 50 MB private, mostly installing the providers (26 MB of host heap).

#### The SwiftShader GPU process after the lock fix

* About 27 fps on the rAF page, and frames are not held back by pacing: the display link runs at 62.5 Hz.
* Per frame on the GPU main thread: raster 16 ms and `SwapBuffers` 12 ms.
  * `EndAccessImages` (8 ms) has no traced children apart from `IOSurfaceImageBacking::WaitForCommandsToBeScheduled`,
    7.8 ms over 1,047 calls.
  * The rest is Dawn and SwiftShader finishing the frame in translated code.
* **The remaining provider cost is small:**
  * `query_region` for newly created mutex and condition addresses, about 1.5 % of samples;
  * the `sched_yield` spin of SwiftShader's scheduler;
  * nothing like the per-operation `VirtualQuery` that `5332015` removed.
* fx translation time in the GPU process is negligible (`translate_and_install` about 40 samples). What remains is fx
  code speed on SwiftShader's own work.

#### A second batch of sites (`web_matrix.ps1 -Batch 2`)

SwiftShader mode, JIT, one fresh browser per site. Nothing was typed, submitted, bought or signed into.

| Site | Loaded | Load event (s) | Renderer crashes | Peak private (GB) | Notes |
| --- | --- | ---: | ---: | ---: | --- |
| Reddit | yes | 6.5 | 0 | 4.2 | Reddit's "Prove your humanity" check, drawn; not bypassed |
| X, a public profile (@NASA), logged out | yes | 6.4 | 0 | 4.2 | profile and posts drawn (12 images) |
| Amazon front page | yes | 1.7 | 0 | 3.5 | AWS WAF challenge first (HTTP 202); see below |
| Wikipedia edit view (read only) | yes | 7.2 | 0 | 3.8 | "View source" (protected article), editor text shown |
| Discord landing page | yes | 16.7 | 0 | 4.4 | one request refused by the page's own CSP |
| Figma landing page | yes | 9.9 | 0 | 4.3 | |
| Google Docs landing page | yes | 21.0 | 0 | 3.8 | |
| WebGL Aquarium (webglsamples.org) | yes | 32.7 | 0 | 3.7 | 3.8 fps (`fps` action: 19 frames in 5 s) |

* **Amazon's AWS WAF challenge** (a 202 page whose script gets a token and reloads):
  * Under Alchemy with the GPU process on SwiftShader it takes 26.5 s; native Chrome passes it at once.
  * The software compositor passes it in 1.5 s, because there is no GPU process and the challenge's GPU work is
    skipped.
  * In SwiftShader mode the renderers sit idle while one GPU-process thread runs translated SwiftShader code. So it is
    SwiftShader under fx again, most likely shader compilation. The WebGL Aquarium's 33 s load event points the same
    way.
  * Once the token is in, the store front loads ("Amazon.com. Spend less. Smile more.").
* **Driver additions (`3f77efe`, `d9b4872`):** the `fps` action, and the `html` action that saves a page's
  markup. It showed the challenge page.

### Follow-up 3 (2026-10-20, night): SwiftShader's first use, the SIGBUS, provider registration

#### WebGL first use under SwiftShader: compiling under translation, not JIT churn

Amazon's AWS WAF challenge (open item 6) was measured in the GPU process with `ALCHEMY_FX_STATS` and the sampling
profiler.

| | Native Chrome, SwiftShader | Alchemy, SwiftShader |
| --- | ---: | ---: |
| The challenge, from navigation to the store front's load | 1.1 s | 26.5 s |

* **fx's own work in the GPU process during the challenge is small:**
  * Translations of code SwiftShader generated at run time (registered JIT regions): 139 → 315.
  * fx JIT time 0.24 s and install time 0.16 s, in all.
  * 0 self-modifying-code faults and 0 re-translations.
  * The code arena grew 71 → 78 MB.
* **Where the time goes:** of the translated samples on the busy GPU thread, 83 % are in `libvk_swiftshader`'s own code
  and 17 % in the code it generated. The profile is flat (90 % of the samples spread over 697 regions): it is
  most likely SwiftShader's shader compiler (Reactor) running as translated code, about 24 times slower
  than native.
* **For the arena task:** this is **not** JIT churn. A WebGL page's first frames add about 7 MB of arena and nothing is
  invalidated. The arena churn is V8's (open item 1).
* **What would help:** faster fx code for compiler-like workloads (branchy, pointer-chasing). It is not an Alchemy
  provider wait, and not translation time.

#### The intermittent `SIGBUS`: a race in fx's SMC write-fault handling

Eight long JIT runs with the fault report of `c1882d8` (alternating Speedometer ×10 and JetStream) hit it twice, at
the known pc (`Google Chrome Framework+0x3b97a8`, a worker thread):

```
EXC_BAD_ACCESS: guest write of 0x2f0160b30 (protection failure) at pc 0x2003d97a8
  [host page 0x2f0160000 protect 0x2 allocation 0x2f0000000, fx write-protected (guest 0x4),
   region 0x2f0000000+0x7dc0000 --- lazy]
```

* **What the report says:** the page is read-only because fx write-protected it for self-modifying-code detection
  (fx had translated code from it); the guest's own protection is read-write. Most likely it is V8's code space,
  which V8's background threads write while the main thread runs code from the same page.
* **The cause,** in fx's write-fault handler (`src/fx/runtime/veh.cpp`, `smc_handle_write_fault`). Several threads
  write a code page while another runs code from it:
  1. Writer B's store faults; B's handler invalidates the page's translations and gives the page back its read-write
     protection.
  2. Writer D's store faulted at the same time; D's handler finds the page no longer fx's but writable, and retries.
  3. The running thread translates from the page again, and fx write-protects it again. D's retried store (or D's
     next store to the same word) faults again.
  4. The handler gave up after 4 such retries per thread and address, counted across separate stores, and passed
     the fault on as a guest protection failure: `SIGBUS`.
  A second, narrower window let the handler decide "not fx's" and "not writable" across a re-protection, which also
  passed the fault on.
* **The fix:**
  * The retries count only while nothing changes: a retry after fx protected or gave back a page in between is
    progress, not a loop. fx counts its page protection changes for this.
  * Both checks ("fx's page?" and "writable now?") are made under one map lock. fx protects a page again only under
    the exclusive lock, so the page cannot change hands between them. `smc_handle_write_fault` now answers
    smc / writable / other.
* **The test** (`fx_runtime_tests`, `concurrent_writers_on_a_code_page_never_fault`): four threads store to their
  own function on one code page while four others call those functions. With the old handler a store faulted for
  good in 5 of 12 runs (and in 6 of 20 with only the lock change). With the fix: 0 of 25.
* **Chrome:** the same eight long JIT runs (alternating Speedometer ×10 and JetStream, SwiftShader mode):

  | Binary | `SIGBUS` | `bad allocation` (the arena, open item 1) | Speedometer progress at the end |
  | --- | ---: | ---: | --- |
  | Before (`ebcedb1`) | 2 | 6 | 395, 467, 441 of 580 tests (one run hit the `SIGBUS` at 15) |
  | `da0ce47` and `81096de` | 0 | 8 | 488, 422, 519, 407 of 580 tests |

  JetStream still ends in the arena at test 18 or 19 of 64 (`pdfjs`, `prepack`).

#### Provider registration: per-registration cost, and lazy native entries

Measured on `Google Chrome --version` (a minimal Alchemy process) with `ALCHEMY_PERF=1 ALCHEMY_PERF_MEMORY=1`; the
installation now has a perf phase per provider and per GUI installer.

* **What is registered:** 10,681 exports, 9,671 native Objective-C methods and 725 classes
  (`ALCHEMY_REGISTRATION_AUDIT=1`). They make 13,898 synthetic calls (engine call slots).
* **Where the cost is:** the GUI installers take most of it (largest: `install_completion_kit`, which completes AppKit and
  Foundation classes, 57 ms and 8.8 MB of host heap; AppKit 41 ms and 5.3 MB; `install_chrome_strict` 33 ms).
* **It is a fixed cost per registration, not a few big tables.** A temporary experiment that registered 5,000 empty
  functions and 5,000 empty methods measured:

  | One registration | Host heap | Private | Time |
  | --- | ---: | ---: | ---: |
  | function (`install_function`) | 670 B | 1.1 KB | 3.4 µs |
  | Objective-C method (`add_native_method`) | 1.1 KB | 1.4 KB | 5.2 µs |

  A function's cost is its handler closures (the fork-child guard wraps every handler and copies its library and
  symbol names), the export map entry, the engine's slot, and, in fx, a native host-callback entry: about 160 bytes of
  arena code plus an adapter, two hash-map entries and an instruction-cache flush.
* **Sharing between processes is not possible as things are built:** the handlers are host closures over
  per-process objects, and Windows has no fork to inherit them.
* **Lazy native entries (done):** a synthetic call now gets its fx host-callback entry at its first call. The first
  call takes the SVC slot, which installs the entry; later calls are native as before. `Chrome --version` calls 118
  of its 13,898. `ALCHEMY_FX_STATS` prints the two counts at exit, and `alchemy_fx_engine` checks them.

  | Per process, `Chrome --version` | Before | Lazy entries |
  | --- | ---: | ---: |
  | Provider installation (5 runs, interleaved) | 83–91 ms | 55–64 ms |
  | Process start to the guest's `main` | 315–362 ms | 288–328 ms |
  | Private memory after the providers | 72.0 MB | 65.0 MB |
  | Host heap after the providers | 29.5 MB | 27.4 MB |

* **Still eager, and the next step if more is wanted:** the handler closures and the export map. Making them lazy
  means registering by name and building the handler on the first `resolve()`, which changes every provider's
  installer.

### Follow-up 4 (2026-10-25): AV1 under fx, with dav1d's checkasm

The user approved downloading dav1d's source for this.
* **The downloads** (diagnosis only; neither is in the repo or under artifacts/):
  * dav1d 1.5.4: https://downloads.videolan.org/pub/videolan/dav1d/1.5.4/dav1d-1.5.4.tar.xz, 1,038,852 bytes,
    SHA-256 `686616b7...a5c5` (as published), BSD-2-Clause.
  * Its checkasm framework v1.2.0: https://code.videolan.org/videolan/checkasm/-/archive/v1.2.0/checkasm-v1.2.0.tar.gz,
    202,810 bytes, SHA-256 `bfcbcd76...7616`, BSD-2-Clause.
* **The build:** checkasm for arm64-apple-macos11, 8 bpc and baseline NEON (Alchemy reports no DotProd or I8MM),
  with Visual Studio's clang 19 and ld64.lld. It uses `runtime/include` plus a few stand-in Darwin headers. The
  recipe is in the notes folder (`dav1d-checkasm`).
* **Results under alchemy-fx** (1039 routines, each compared with its C version):

  | Binary | Wrong routines |
  | --- | --- |
  | `3ff181c` (before the fx perf wave) | `cfl_ac` (9 variants), `save_tmvs` |
  | `fc30d43b` (main, perf wave 1) | the same, and `splat_mv` crashes (SIGSEGV) |
  | `fc30d43b` + the RBIT fix | `save_tmvs`; `splat_mv` crashes |
  | `fc30d43b` + the RBIT fix, `TRANSMUTE_FX_CORE=exactflags` | none |
  | main `79654111` (the RBIT fix `2e589e64`, the BR revert `c1a198ac`) | `save_tmvs` |

* **Three causes:**
  * **`cfl_ac` (fixed, `2e589e64`):** fx turns RBIT feeding CLZ into one TZCNT, with one pending fusion slot. dav1d
    computes ctz(width) + ctz(height) as `rbit w9; rbit w10; clz w9; clz w10`. The second RBIT took over the slot,
    so the first CLZ counted the zeros of a register its RBIT never wrote. log2sz came out wrong and the DC became 0.
    Test: `fx_translator_tests` `core_rbit_clz_interleaved`.
  * **`splat_mv`:** the address of dav1d's jump table is computed once, then each loop iteration does
    `subs ...; br x3`, and the target blocks end in `b.gt`. The perf wave treats flags as dead at a BR without an
    ADR/ADRP in its block (`f01bb567`), so the loop count is lost. Fixed on main: `c1a198ac` reverts that default.
  * **`save_tmvs`:** `cmp x9, x10; blr x11; b.ge 4f`, where `x11` is one of dav1d's local store stubs that keep
    NZCV. fx's older ABI assumption treats flags as dead at BLR. `exactflags` fixes it. Open; it is the fx flag
    model, which the fx perf wave owns. Its lead is measuring exactflags as the default, and p2 is building a
    cheap exact path. dav1d's checkasm now runs in every fx integration gate.
* **Chrome** (YouTube's AV1 stream, software compositor, `fc30d43b` + the RBIT fix):
  * With `exactflags`, `Dav1dVideoDecoder` decodes 854x480 with no dav1d error. Before, it failed with error -22
    and fell back to VP9.
  * With the default flags, the renderer crashes in dav1d (a write to 0).
  * YouTube's player still ended with "Something went wrong" in the `exactflags` run; not explained yet.

* **YouTube on main `d7806f6e`** (software compositor, the same AV1 stream):

  | Flags | Result |
  | --- | --- |
  | default | the renderer dies with `SIGBUS`, an out-of-bounds write in dav1d (`save_tmvs`'s loop runs on when its flags are lost at `blr`) |
  | `TRANSMUTE_FX_CORE=exactflags` | **plays AV1**: 854x480, 1,103 frames in 18 s, 321 dropped; the frames are clean |

  The earlier "Something went wrong" (an `exactflags` run) had no decoder error. YouTube's script dropped the
  player after repeated `DEMUXER_UNDERFLOW`, following a consent rejection and reload on a machine at 100 % load.
  It did not recur. Exact flags as the default (fx perf wave, integ `9caadb55`) is what main needs for AV1.
* **Main `c4b587ad` (exact NZCV by default), no `TRANSMUTE_FX_CORE`:**
  * dav1d's checkasm: all 1039 pass.
  * `av1_webcodecs.html`: PASS, at the same 29.4 dB as native Chrome.
  * YouTube AV1 with the consent overlay left alone: plays, 854x480, 1,801 frames in 20 s, 501 dropped.
  * After "Reject all", YouTube showed "Something went wrong" once more, again with no media error: its script
    destroyed the player after the consent reload. That is YouTube's flow on a slow, loaded machine, not decoding.
    Not chased further; native Chrome could not be compared under the same load.

### Open, worst first

1. **fx code arena exhaustion** (gap 1). It needs fx runtime design work, for which a follow-up is proposed:
   * Reclaim invalidated translations once no thread can be in them: an epoch at the dispatcher, then a free list,
     or a flush of the translation part of the arena at a safepoint. The stubs and the ABI thunks would then need an
     area of their own.
   * Map persistent-cache translations from the cache file, shared and executable, instead of copying them into
     every process's arena. That saves about 0.4 GB of private memory per process and 40 % of each arena (gap 4).
2. **The SwiftShader GPU process**, now about 27 fps after `5332015` (gap 3).
   * The rest is SwiftShader's and Skia's work under translation: raster 16 ms and `SwapBuffers` 12 ms per frame on
     the GPU main thread.
   * That is fx code quality, not an Alchemy wait (Follow-up).
3. **YouTube video** (gap 5):
   * **AV1 under fx is fixed on main** (`c4b587ad`): `cfl_ac` (`2e589e64`), `splat_mv` (`c1a198ac`) and
     `save_tmvs` (exact NZCV by default, fx perf round 13). dav1d's checkasm passes, and YouTube plays AV1
     (Follow-up 4).
   * `IOSurfaceSetValue(NULL)` and planar IOSurfaces are fixed (`1f5705a`, Follow-up). With SwiftShader the video
     now plays through the VP9 fallback.
   * CoreVideo pixel buffers still cannot be created under Alchemy (`CVPixelBufferCreate` answers
     `kCVReturnUnsupported`).
4. **`vm_remap(VM_FLAGS_OVERWRITE, copy = FALSE)` of shared memory over mmap'd pages** is still
   `KERN_NOT_SUPPORTED`.
   * About 1 in 1,400 of Chrome's remaps. On Stack Overflow it failed a GPU command buffer's ring buffer
     (`AllocateRingBuffer() failed`), and the page still loaded.
   * A real alias needs the target in a placeholder reservation, so mmap'd ranges in fx would have to be
     placeholder-backed. The target is inside V8's large reservations (Follow-up), so the design is in place.
     A follow-up session is proposed for it.
5. **The intermittent `SIGBUS` in long JIT runs is fixed** (`da0ce47`, Follow-up 3): fx's write-fault handler gave up
   on a store to a code page that several threads wrote while another ran code from it.
6. **WebGL first use under SwiftShader:** Amazon's challenge takes 26.5 s (native 1.1 s) and the WebGL Aquarium loads
   in 33 s. It is SwiftShader's shader compiler running as translated code, not fx translation or JIT churn
   (Follow-up 3), so it is part of fx code speed (item 2).
7. **Fixed: a process could not place its main image** (`Mach-O: requested segment address is unavailable`), seen in
   3 runs (two of Chrome's GoogleUpdater children, the renderer add-on's render server), on old and new binaries.
   * Windows places a process's first allocations (loader thread stacks, the first heap segments) bottom-up from a
     random base, before `main`. Now and then that base falls in [4 GiB, 8 GiB), the executable's range.
     `hold_load_range` (`324c5a4`) can only hold the parts that are still free, so the image could not map.
   * A position-independent executable (every arm64 one) now slides, as on Darwin, to the lowest held part of the
     range that fits it (`FxEngine::slide_main_image`). The notice names the host memory in the way.
   * Test: `alchemy_process_tree_fx_rebased` takes 64 KiB at 0x1'0000'0000 before main. Main's binary fails with
     the message; the fix loads the fixture at 0x1'0001'0000.
8. **Smaller:**
   * Chrome logs `DNS config watch failed to start` (`dns_config_service_posix.cc`), so it does not notice DNS
     configuration changes.
   * Google's search page asks for a reCAPTCHA.
   * The paint timing `first-contentful-paint` is missing on some pages.
   * The native baseline's slow page loads under the harness (Site matrix) were not explained.

### Regression check before landing

The branch's binary at `b833b8e` (code as of `e74374a`) against main `ad7dbf7`.

* **The main binary:** stage 4's integration build of `c2ce643`. Its code equals `ad7dbf7`.
* **Same build mode:** Ninja and sccache for both.
* **Runs:** interleaved, cloaked, `ALCHEMY_NETWORK=loopback`, no input.
* **`ctest -R "^alchemy_"`, cloaked:** 196 of 196 passed. `fx_emit_fp_tests`: 52 of 52.

| Check | main `ad7dbf7` | `b833b8e` |
| --- | --- | --- |
| Chrome strict headless `--dump-dom probe/test.html`, jitless, 3 runs | exit 0 x3, `data-js="ran"` x3 | the same |
| Chrome windowed (`run_windowed.ps1 -Frames 2 -Settle 20`), 3 runs | exit 0 x3; 31 / 28 / 31 composites | exit 0 x3; 29 / 25 / 33 composites |
| QLab to its welcome window, 2 runs | drawn; capture SHA-256 `3B0D89F748905800...` x2 | **byte-identical** x2 |
| Renderer add-on headless cube, 2 runs | exit 0 x2; renderer created; render returned 6.4 / 6.4 s | exit 0 x2; the same, 6.4 / 6.4 s |
| Blender headless CPU, 14 rounds: median process CPU | 2.35 s | 2.34 s |
| Blender windowed, 3 runs: first frame / first 3D viewport draw, median | 1484 / 2557 ms | 1480 / 2557 ms |

* **Network denials:** only the known ones, Chrome's `[2001:4860:4860::8888]:443`, QLab's
  `accounts.figure53.com` and the renderer's license server.
* **Logs:** no new `[alchemy metal]` or `[alchemy msl]` line in Blender's log.

### Regression check before the follow-up's landing

The branch's binary at `6db5f0d` (SHA-256 `9D09B150...`) against main `ba00818` (its binary is the benchmark binary,
`3D81CB47...`). The runs were interleaved, cloaked, with `ALCHEMY_NETWORK=loopback` and no input.

* **`ctest -R "^alchemy_"`, cloaked:** 196 of 196 passed.
* **fx unit tests:** `fx_runtime_tests` and `fx_emit_fp_tests` passed.

| Check | main `ba00818` | `6db5f0d` |
| --- | --- | --- |
| Chrome strict headless `--dump-dom`, jitless, 3 runs | exit 0 x3, `data-js="ran"` x3; wall 6.8 / 7.3 / 7.6 s; root CPU 15.9 / 17.6 / 18.2 s | the same; wall 5.8 / 6.0 / 5.7 s; root CPU 15.4 / 15.1 / 14.8 s |
| Chrome windowed, 3 runs | exit 0 x3; 50 / 53 / 50 composites | exit 0 x3; 76 / 82 / 81 composites |
| QLab to its welcome window, 2 runs | capture SHA-256 `3B0D89F748905800...` x2; first backing 1.1 / 0.9 s | **byte-identical** x2; 0.9 / 0.9 s |
| Renderer add-on headless cube, 2 runs | exit 0 x2; enabled in 23.0 / 22.6 s; render 6.4 / 6.4 s | exit 0 x2; 22.8 / 22.9 s; 6.4 / 6.4 s |
| Blender windowed, 3 runs: first frame / first 3D viewport draw, median | 1262 / 2244 ms | 1250 / 2222 ms |
| Blender headless, 7 rounds: process CPU / wall, median | 2.12 / 1.9 s | 2.11 / 1.9 s |

* **Network denials:** only the known ones, Chrome's `[2001:4860:4860::8888]:443`, QLab's `accounts.figure53.com`
  and the renderer's license server.

### Regression check for the IOSurface fix (`1f5705a`)

The branch's binary at `1f5705a` (`DC1394FC...`) against main `dfb8b4b` (`9D09B150...`), the same method as above.

* **`ctest -R "^alchemy_"`:** 196 of 196 passed.
* **QLab:** the capture is byte-identical (`3B0D89F748905800...`); first backing 1.1 / 0.9 s on main, 1.0 / 0.9 s new.
* **Renderer add-on:** render 6.5 / 6.4 s on main, 6.4 / 6.5 s new.
* **Blender windowed**, median first frame / viewport: 1279 / 2317 ms on main, 1281 / 2261 ms new.
* **Blender headless**, median CPU / wall: 2.12 / 1.9 s on main, 2.03 / 1.9 s new.
* **Chrome windowed:** exit 0 in every run, with similar composite counts.
* **Chrome strict headless** (no IOSurface call at all in that mode):
  * Interleaved pairs, main first in each pair, were 0.8 s slower on the new binary in 5 of 6 pairs.
  * Runs in blocks of two (new, new, main, main, ...) gave new 4.5 / 4.5 / 5.8 / 5.8 s against main 6.7 / 6.6 /
    6.5 / 5.7 s.
  * So that difference was drift between runs, not the binary.

### Regression check for Follow-up 2

The branch at `d9b4872` (binary `3AE405A1...`, the docs commit adds no code) against main `4cd8a0c` (`DC1394FC...`), the
same method as above.

* **`ctest -R "^alchemy_"`:** 196 of 196 passed.
* **fx unit tests:** `fx_runtime_tests` and `fx_emit_fp_tests` passed.

| Check | main `4cd8a0c` | `d9b4872` |
| --- | --- | --- |
| Chrome strict headless, jitless, 3 runs | exit 0 x3, `data-js="ran"` x3; wall 4.6 / 6.4 / 6.6 s | the same; 5.4 / 6.5 / 5.9 s |
| Chrome windowed, 3 runs | exit 0 x3; 68 / 79 / 79 composites | exit 0 x3; 67 / 67 / 77 |
| QLab to its welcome window, 2 runs | capture `3B0D89F748905800...` x2 | **byte-identical** x2 |
| Renderer add-on headless cube, 2 runs | exit 0 x2; render 6.5 / 6.5 s | exit 0 x2; 6.4 / 6.5 s |
| Blender windowed, median first frame / first viewport draw | 1274 / 2346 ms | 1289 / 2289 ms |
| Blender headless, 7 rounds, median CPU / wall | 2.05 / 1.8 s | 2.02 / 1.9 s |

### Regression check for Follow-up 3

The branch at `e4415d8` (binary `2D1D57CB...`) against main `ebcedb1` (`3AE405A1...`). Interleaved, cloaked,
`ALCHEMY_NETWORK=loopback`, no input. The machine was shared with other sessions' builds and benchmarks (load up to
100 %), so the timings are noisy; quiet-machine numbers are left to the fx performance suite.

* **`ctest -R "^alchemy_"`, cloaked, -j12:** 195 of 196. `alchemy_gui_windowserver_win32` failed its focus checks
  under the load and passed 3 of 3 alone.
* **fx unit tests:** `fx_runtime_tests` 93 of 93, `fx_emit_fp_tests` 52 of 52.

| Check | main `ebcedb1` | `e4415d8` |
| --- | --- | --- |
| Chrome strict headless, jitless | exit 0 and `data-js="ran"` in every run | the same |
| … in blocks under load (new, new, main, main, twice): median root CPU / wall | 24.0 / 27.6 s | 23.0 / 18.2 s |
| Chrome windowed, 3 runs | exit 0 x3; 62 / 62 / 36 frames | exit 0 x3; 76 / 74 / 63 |
| QLab to its welcome window, 2 runs | capture `3B0D89F748905800...` x2 | **byte-identical** x2 |
| Renderer add-on headless cube, 6 runs: render time | rendered x6; 6.4 to 8.4 s | rendered 5 of 6 (below); 6.5 to 9.6 s |
| Blender headless, 7 rounds, median process CPU | 2.28 s | 2.19 s |
| Blender windowed, median first frame (3 runs; again at 100 % load) | 1793 ms (9381 ms) | 2056 ms (9431 ms) |

* **Renderer add-on, the one failure:** the renderer add-on's render server, a child process, ended at load with `Mach-O: requested segment
  address is unavailable`, and the add-on gave up. Older binaries showed the same message twice in Chrome runs
  earlier today (a child process). It is an intermittent failure to place a child's main image, not this change;
  open item 7 (fixed since).
* **Network denials:** only the known ones.

### Regression check for the main-image slide (`3ff181c`)

The branch at `3ff181c` (binary `823E9577...`) against main `9d6b42c` (`2D1D57CB...`), the same method. The machine
ran at 100 % load from other sessions throughout, so timings only show that nothing broke.

* **`ctest -R "^alchemy_"`, cloaked, -j12:** 196 of 196. **fx unit tests:** 93 of 93 and 52 of 52.
* **Renderer add-on headless cube, 6 runs on the new binary:** rendered 6 of 6; then 2 of 2 on each binary, interleaved.
* **Chrome strict headless and windowed, 3 runs each:** exit 0 every time on both.
* **QLab:** the capture is byte-identical (`3B0D89F748905800...`) on both.
* **Blender:** windowed and headless runs exit 0 on both. Headless median CPU was 5.41 s main and 5.81 s new, inflated
  about 2.5 times by the load, with overlapping ranges (5.08 to 5.88 and 5.28 to 6.50 s).

## Stage 4 result (2026-10-19): the GPU process, in software

Stage 4 ([brief](ALCHEMY_BRIEF_CHROME_STAGE4.md)) is done on `integ/chrome-s4`, but not as the brief planned. It
lands with the GPU process running ANGLE and Skia Graphite on Chrome's bundled SwiftShader, so on the CPU. Chrome's
GPU stack doesn't reach Alchemy's Metal. Within this wave's rules, nothing in Chrome 155 on macOS reaches the
hardware GPU (below).

* **The run.** `scripts/chrome/run_windowed.ps1 -SwiftShader` adds `--use-gl=angle --use-angle=swiftshader
  --enable-unsafe-swiftshader` to stage 2's windowed run. Graphite stays on, the default. The GPU process runs as a
  child process and survives. WebGL and compositing work, and Chrome quits with exit 0.
  * Windowed `probe/test.html`: exit 0 in 3 of 3 runs.
  * Windowed `scripts/chrome/pages/webgl.html`: `Alchemy WebGL: PASS` and exit 0 in 3 of 3 runs, with 236-258
    composites each.
  * Headless `gpu_report.html`: `webgl=software webgl2=software webgpu=unavailable` in 3 of 3 runs.
  * Headless `webgl.html`: PASS. `webgpu.html` reports `FAIL adapter`, as designed.
  * No Alchemy stop, trap or `unsupported` line in any of these runs.
* **What runs where:**

  | Feature | Runs on | Result |
  | --- | --- | --- |
  | GL (ANGLE) | ANGLE's Vulkan back end on SwiftShader (`Libraries/libvk_swiftshader.dylib`), on the CPU under fx | works |
  | WebGL 1 and 2 | ANGLE on SwiftShader | `scripts/chrome/pages/webgl.html` (15 checks) PASS, headless and windowed; the in-page report says `webgl=software webgl2=software` |
  | Skia Graphite (raster and compositing) | Dawn's SwiftShader (Vulkan) fallback adapter | works; frames reach the window through IOSurfaces |
  | WebGPU | none | `requestAdapter()` answers null: `webgpu=unavailable` |
  | Alchemy's Metal on the host GPU | not reached by Chrome's GPU stack | see below |

* **Why the hardware GPU is out of reach:**
  * **ANGLE's Metal back end** loads its built-in shaders from a precompiled metallib, Apple's AIR (LLVM bitcode).
    The load is `DisplayMtl.mm:1350`, through `-[MTLDevice newLibraryWithData:error:]`, which fails explicitly
    unless the AIR translator DLL is present (below).
    Translating AIR means working out Apple's undocumented builtins, which is reverse engineering, and the user
    decided against it.
  * **metal2vulkan,** a public LGPL AIR-to-SPIR-V translator, was evaluated as a library. ANGLE's library is AIR 2.1.
    * **First evaluation** (`720ef1c` and `a2c440b`, October 19–20): it translated 8 of the 37 pipelines in ANGLE's
      library, and they passed spirv-val. The others failed in its AIR front end: function constants
      (`air.function_constants`), `air.center` on a vertex entry, and texture intrinsics. That was
      [steelbrain/metal2vulkan#9](https://github.com/steelbrain/metal2vulkan/issues/9).
    * **Re-test** on its `development` branch at `4d695b7` (October 25, with AIR 2.1 support added for that issue):
      **all 37 pipelines translate and pass spirv-val (vulkan1.2 and 1.4)**, given the function-constant values a
      Metal runtime passes. A sweep over each constant's value range passed too. Nothing ran on a GPU yet.

    The library was taken
    from a run-time dump of what Chrome passes to Metal (`ALCHEMY_MSL_DUMP`, `245dedd`). Nothing was cut out of
    Chrome's files.
    * **Adopted** (the user's decision, October 25) as a separately built LGPL library: `m2v_capi.dll`, a C ABI
      over it in `external/m2v-capi/` (LGPL-3.0-or-later), built by `scripts/m2v/build_capi.ps1` and loaded at run
      time by `src/alchemy/darwin/metal/air_translator.cpp`. Without the DLL, `newLibraryWithData:` fails as
      before. Nothing of metal2vulkan's code is in the MIT tree. Its license, the pinned revision, the crates and
      what distributing the DLL requires are in
      [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md#loaded-at-run-time-when-present-m2v_capidll-metal2vulkan).
      `alchemy_metal_air` creates real VkPipelines for all 37 of ANGLE's pipelines on this machine's GPU. It also
      draws ANGLE's clear through the encoders (the right color at every pixel) and runs its 8-to-16-bit index
      conversion (`commands_foreign.cpp`; the translator's binding plan per draw and dispatch).
    * **In Chrome** (headless, `--single-process --in-process-gpu`, the DLL present): ANGLE-Metal initializes past
      `DisplayMtl.mm:1350`. WebGL 1 and 2 contexts report "ANGLE Metal Renderer: Intel(R) Iris(R) Xe Graphics", and
      a clear reads back exactly. ANGLE's generated MSL needed `as_type<T>()` unqualified inside templates and
      `metal::get_num_samples()`, which the MSL compiler now has.
    * **WebGL draws on the GPU** (headless, single-process). A WebGL2 probe gives exactly the expected pixels for
      an 8-bit-indexed draw, a sampled 2x2 RGB texture with mipmaps, and a framebuffer blit, with glError 0 and no
      context loss. WebGPU still gets no adapter.
    * **Windowed, multi-process** (`run_windowed.ps1 -Gpu`, `webgl.html`): passes. The GPU process used to stop
      with "invalid or destroyed compatibility object" (isa 0). Chrome's Objective-C zombies replace
      `-[NSObject dealloc]`: they destruct the object, keep its memory, and free it later with `object_dispose`.
      The object store freed the memory at once, and a new object (an autoreleased `MTLRenderPassDescriptor`)
      took its address. Kept memory now stays allocated until `object_dispose`.
    * **A three.js-style scene** (`scripts/chrome/pages/webgl_scene.html`, 13 self-checking WebGL 2 tests): a
      perspective lit cube from a uniform block with depth and culling, 64 draws with uniform changes, mipmaps,
      cube maps, 2D-array and 3D textures, multiple render targets, a 4x MSAA renderbuffer resolved by a blit,
      stencil, a post-process pass, an integer texture with 32-bit indices and `gl_VertexID`, transform feedback,
      `copyTexSubImage2D`, then 30 animated frames. It passes headless and windowed (cloaked, 0 visible windows),
      exit 0. A desktop Chrome on D3D11 gives the same pixels. Headless, the 30 frames take about 26 s (a
      functional run on a busy machine, not a measurement).
    * **WebGPU on Dawn's Metal back end** (`--enable-unsafe-webgpu`; the AIR translator DLL is needed here too,
      or ANGLE-Metal fails and the single-process run traps). WebGPU used to stop before Dawn: the renderer maps
      its WebGPU command buffer's ring buffer into V8's sandbox with `vm_remap(VM_FLAGS_OVERWRITE, copy = FALSE)`
      over mmap'd pages, which Alchemy refused. alchemy-fx now has placeholder-backed guest reservations
      (`b83d56e4`), and `requestAdapter()` gives the hardware adapter (intel, gen-12lp, not the fallback).
      `webgpu.html` passes, and so does `scripts/chrome/pages/webgpu_scene.html`: 15 self-checking tests in the
      classic samples' style. They cover a lit perspective cube with depth and culling, dynamic uniform offsets,
      mipmaps rendered level by level, 4x MSAA with a resolve target, ping-pong compute particles, atomics and
      workgroup reductions, a GPU-written `drawIndirect`, render bundles, storage textures, cube maps, a shadow
      comparison sampler, stencil with alpha blending, occlusion queries, `copyExternalImageToTexture`, and 30
      animated canvas frames. It passes headless and windowed (cloaked, 0 visible windows), exit 0, and a desktop
      Chrome on D3D12 passes the same checks. Dawn keeps occlusion results in a private-storage visibility
      buffer, which used to be an explicit stop. Results are now copied into the buffer on the GPU at the end of
      the pass, so a resolve blit later in the same command buffer reads them; that is also a fix for shared
      buffers, which completion used to fill.
    * **Breadth** (`scripts/chrome/pages/gpu_breadth.html`): 34 tests of the less common parts real pages use.
      * WebGPU exposes all 12 optional features this machine supports: depth-clip-control, timestamp-query,
        rg11b10ufloat-renderable, float32-filterable, shader-f16, dual-source-blending, clip-distances,
        depth32float-stencil8, bgra8unorm-storage, indirect-first-instance, texture-compression-bc and subgroups.
        The page uses each once, plus depth copies, sRGB views, 3D slices, read-only depth, primitive restart,
        lines and points.
      * WebGL 2 gets occlusion queries and the depth clamp, polygon mode, clip distance, indexed draw buffers,
        provoking vertex, float color buffer, S3TC and anisotropy extensions. It also gets sRGB and integer
        targets, PBO reads, scaled blits, shadow samplers and array layers.
      * 33 pass and 1 is skipped: ANGLE doesn't expose `WEBGL_draw_instanced_base_vertex_base_instance` on Metal,
        and desktop Chrome skips it too. The page passes headless and windowed.
      * Depth clamp (`setDepthClipMode:`, used by WebGPU's `unclippedDepth` and `EXT_depth_clamp`) and line fill
        (`setTriangleFillMode:`, used by `WEBGL_polygon_mode`) used to be explicit stops. They are now pipeline
        variants, for MSL pipelines and translated ones alike.
  * **ANGLE's Vulkan back end on the host's Vulkan** isn't allowed. Chrome 155 on macOS accepts only ANGLE on Metal,
    or on SwiftShader with `--enable-unsafe-swiftshader`. Its message is `gl_factory.cc:110 ... not found in allowed
    implementations: [(gl=egl-angle,angle=metal)]`.
  * **With ANGLE on SwiftShader, Chrome keeps the rest off the GPU** *(design)*:
    * `DawnContextProvider::DefaultForceFallbackAdapter()` is true, so Graphite asks Dawn for the fallback adapter.
      Selecting Dawn's Metal back end then finds no adapter (`FATAL:gpu_init.cc:1237 InitializeDawn() failed!`).
    * The WebGPU feature status is "software", so WebGPU is off.
* **What Alchemy gained** (by group; details in each commit):
  * **Metal core (g1, merged early):**
    * GPU IORegistry entries with PCI vendor and device IDs from the Vulkan physical devices. Dawn's
      `GetDeviceIORegistryPCIInfo` warning is gone.
    * macOS 15's queue and log-state descriptors.
    * The timestamp counter set.
  * **Shaders (g2):**
    * The MSL that Tint and ANGLE's translator generate. Derived function constants used to read 0, and other
      dynamic initializers are now an explicit error.
    * Clip-distance arrays, buffer-pointer argument buffers, and macOS 13's `MTLBinding` reflection.
    * `alchemy_msl_chrome_corpus`, over the shaders Chrome hands Metal, dumped to `artifacts/` and never committed.
      Its first Chrome corpus is 4 of 4 Tint kernels from the On-Device Model Service's Dawn. Graphite on SwiftShader
      sends no MSL.
  * **Surfaces (g3):**
    * IOSurface-backed `MTLTexture`s, with macOS-style implicit sync across processes, and cross-process
      `MTLSharedEvent`s.
    * `CAMetalDisplayLink`, the `CADynamicRange` keys, `IOSurfaceSetPurgeable`, and `CALayer.contentsRect`.
    * A two-process test in which a child renders with Metal into an IOSurface that a parent's CALayer shows.
  * **ANGLE, WebGL and the Metal API (g4):**
    * The smoke pages (`gpu_report`, `webgl` and `webgpu.html`).
    * Sampler LOD clamps, listener queues, and `newBufferWithBytesNoCopy:`.
    * Four fixes outside its area that Chrome's GPU process needed:
      * `strnlen` and `strlcat` take their size as a bound. `strnlen(p, SIZE_MAX)` was EINVAL; Dawn's labels use it.
      * `class_getProperty` from guest property metadata. It was an explicit stop in Chrome's
        `RemoteLayerAPISupported` probe.
      * ICC profiles for named CoreGraphics color spaces, written by the project from the standards.
      * `confstr(_CS_DARWIN_USER_*)`.
  * **Build:**
    * `alchemy_msl_*` tests could read another worktree's fixtures, because the shared compiler cache kept an
      absolute path from a compile definition. That path now comes from the test's environment (`9d1d998`).
    * The harness gained `-Gpu`, `-SwiftShader`, `-Jit`, title patterns with `-Pass`, and `-Settle`. A static page
      can draw in a single composite after GPU-process relaunches.
* **Verification:**
  * `ctest -R "^alchemy_"`, cloaked: 196 of 196 on c2ce643. 16 merge rounds; the only failure among them was
    `alchemy_gui_windowserver_win32` once, the wave rules' known foreground case.
  * The landing regression, c2ce643 against main a8e89f2: interleaved, cloaked, loopback, Blender on a quiet
    machine.

    | Check | main a8e89f2 | c2ce643 |
    | --- | --- | --- |
    | Chrome strict headless `--dump-dom`, jitless, 3 runs | exit 0, `data-js="ran"` ×3 | same |
    | Chrome windowed CPU (`-Frames 2 -Settle 20`), 3 runs | exit 0 ×3 | exit 0 ×3 |
    | QLab to its welcome window, 2 runs | welcome drawn | same; the capture is byte-identical in all 4 runs |
    | Renderer add-on headless cube, 2 runs | exit 0; render returned 6.6 / 6.5 s | exit 0; 6.6 / 6.5 s |
    | Blender headless CPU, 7 rounds: median | 2.39 s | 2.34 s (-2.1 %) |
    | Blender windowed, 3 runs: first 3D viewport draw, median | 2599 ms | 2587 ms |
    | Blender's Darwin VM calls | mmap 19, munmap 8, mprotect 1, 0 failures | identical |

    * No new error lines: the normalized error sets are identical.
    * Blender's log has no new `[alchemy metal]` or `[alchemy msl]` line.
  * `network denied`: only `[2001:4860:4860::8888]:443` and `https://update.googleapis.com` (Chrome), QLab's
    accounts.figure53.com and the renderer's license server.
* **Open, and follow-ups:**
  * Hardware WebGL: with the AIR translator DLL, WebGL programs draw on the GPU, headless and windowed (above).
    WebGPU runs on Dawn's Metal back end, headless and windowed (above). Whether Graphite runs on Dawn's Metal
    back end hasn't been checked yet.
  * The On-Device Model Service's Dawn on Metal reaches our MSL compiler. Its remaining shader failures stay in that
    utility child.
  * Metal selectors no run reached: the indirect-command-buffer pair. Also not exercised: Chrome's own use of
    cross-process `MTLSharedEvent`s, and CAMetalLayer drawables in a windowless GPU process (both stop explicitly).
  * Noise: `vm_map`/`mach_vm_map` return `KERN_INVALID_RIGHT` 40-60 times per run (stage 3's follow-up), and
    `CVDisplayLinkCreateWithCGDisplay failed (-6670)` appears in headless runs, where Chrome passes its synthetic
    screen ID.

## Stage 3 result (2026-10-19): V8 with its JIT on

Stage 3 of the [staged plan](#staged-plan) ([brief](ALCHEMY_BRIEF_CHROME_STAGE3.md)) is done on
`integ/chrome-s3`.

* **Goal met.** Strict headless `--dump-dom` **without** `--js-flags=--jitless` exits 0 with
  correct output on three local pages: the wave 1 probe (`data-js="ran"`), a JavaScript benchmark
  (`scripts/chrome/pages/js_bench.html`: loops, float math, inline-cache churn, sort, strings,
  closures, typed arrays; results in the DOM, checksum identical to jitless and to Node) and a
  WebAssembly page (`scripts/chrome/pages/wasm.html`: an inline module, sync and async
  instantiation, a table call; `data-wasm="ok"`). 18 of 18 runs (3 pages x jit/jitless x 3)
  exited 0 on round 4 at 25-100 % load, and 18 of 18 again on round 5 (the landing candidate) at
  25-90 % load and on a quiet machine (the table below).
  Jitless V8 has no WebAssembly; that page then reports `data-wasm="unavailable"` and exits 0.
* **What stopped the JIT runs, and the fixes:**
  * **kevent timers** (libSystem, `ef7b0f3`): a negative relative `EVFILT_TIMER` interval
    (MessagePumpKqueue arms an overdue wake-up as `wakeup - now`) was EINVAL, so renderers died on
    `message_pump_kqueue.cc:553 Check failed: kevent64, set timer`. Now it fires at once, once,
    as XNU's `filt_timervalidate` does *(design)*; a negative `NOTE_ABSOLUTE` deadline stays
    EINVAL. It hit jitless runs under load too.
  * **Commit charge** (alchemy-fx, `1e82099`, `8fff8fb`): V8 maps its 256 MiB code range
    `MAP_JIT` and makes it RWX with one `mprotect`, which committed all of it per renderer; under
    machine-wide commit pressure that ended in `bad allocation` (two renderers' dlopen, a browser
    thread). Darwin reservations made accessible in 8 MiB or more now commit on first touch, from
    a demand-commit branch in fx's fault handler (also covering fx's own code reads, providers,
    threads, `MADV_FREE` and region queries). A live renderer's code range commits ~2 MiB.
  * **Shutdown** (stage 2's `35540a8`, `3d8ff5b`, cherry-picked): a SIGKILLed child is waitable as
    soon as its host process is terminated, so Chrome's process launcher no longer waits seconds
    per renderer and the 15 s "Teardown watchdog" stops firing (it did once in 18 runs at 100 %
    load on round 3, never on rounds 4 and 5).
  * **AppKit** (stage 2's `ef1bcfe`, cherry-picked): `+[NSEvent enterExitEventWithType:...]`,
    reached when the real cursor crosses a cloaked window's tracking area.
* **What is real now in fx** (`b9ad118`..`ba3fec3`):
  * `sys_icache_invalidate` reaches `FxEngine::invalidate_code` -> `fx::flush_guest_code`,
    byte-granular: translations overlapping the flushed bytes die, their page neighbours survive
    and stay write-protected. In Chrome it never had to drop code (V8 writes through the executing
    mapping, so the SMC fault always came first); it covers writes fx cannot see.
  * Guest-code registrations merge and publish only on change (V8 re-registers its code range on
    every `mprotect`); an `mprotect` to RX/R no longer lets an SMC fault restore RW over it.
  * SMC-killed JIT translations whose guest words are unchanged are installed again instead of
    re-translated (checked under write protection, so coherence stays exact): about 500 of 2856
    JIT translations in the benchmark renderer.
  * Counters: `ALCHEMY_FX_STATS=1` (or `ALCHEMY_PERF=1`) prints an `alchemy-fx invalidation:` line;
    `ALCHEMY_FX_STATS_PERIOD=<s>` prints it periodically, because Chrome SIGKILLs most renderers
    and their exit statistics are lost.
  * No sub-page SMC: about 1,000-1,200 SMC faults per page run (~140/s on a quiet machine) and
    0.6-0.8 s in the fault path per renderer, under 0.5 % of profile samples. The real cost was collateral re-translation after a page kill, which the
    reuse above addresses.
* **VM under V8** (j2): V8's code range and pointer cage behave as on Darwin (reservations,
  `MAP_FIXED` inside them, partial unmap/protect, `MAP_JIT`, W^X toggles through
  `pthread_jit_write_protect_np`, a documented no-op over RWX memory;
  `pthread_jit_write_protect_supported_np` answers 1). V8's `vm_remap` of its embedded builtins
  into the code range now works (an exact copy of immutable image text). V8 makes no writable
  alias of code *(from the traces)*. Alchemy still reports no entitlements; V8 passes `MAP_JIT`
  without them. Explicit failures: `vm_remap` with copy=TRUE or an alignment mask
  (`KERN_NOT_SUPPORTED`), a writable remap of private memory (the shared-alias stop; never hit).
* **Tests:** `runtime_invalidation_tests.cpp` (fx_runtime: flush granularity, kept neighbours,
  lowered protections, sparse huge ranges, merged ranges, concurrent rewrite under execution),
  engine fixtures `fx_jit_flush` (fails on main at step 28) and `fx_vm_lazy` (fails on main at
  step 49), the `kevent_timers` cases (fail on the old code at arming).

### Performance: jitless vs JIT

Quiet machine (run r5q, 2026-10-19 00:28; started after 3 minutes under 30 % CPU, load per run
15-42 %, 16 of 18 runs under 30 %). Binary: round 5, `integ/chrome-s3` 7e83a09, `alchemy-fx.exe`
sha256 `1235cfc5559c7225d0bd203fd2a89a96fc81e002091495edeb027a019bd65aac`. Strict headless
`--dump-dom`, `--single-process`, cloaked, `ALCHEMY_NETWORK=loopback`, 3 interleaved runs per cell,
medians. `scripts/chrome/jit_matrix.ps1 -StatsPeriod 5`. 18 of 18 runs exited 0 with correct dumps.

| Page | Mode | Exit codes | Wall (s) | SMC faults | SMC faults/s | icache flushes | Re-translations | Translations |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| probe | jitless | 0, 0, 0 | 6.7 | 0 | 0 | 0 | 0 | 55 |
| probe | JIT | 0, 0, 0 | 6.9 | 978 * | ~142 | 1190 | 1923 | 2305 |
| js_bench | jitless | 0, 0, 0 | 9.9 | 0 | 0 | 0 | 0 | 71 |
| js_bench | JIT | 0, 0, 0 | 8.3 | 1158 | 139.5 | 1402 | 2239 | 2681 |
| wasm | jitless | 0, 0, 0 | 7.0 | 0 | 0 | 0 | 0 | 33 |
| wasm | JIT | 0, 0, 0 | 7.4 | 1125 | 150.8 | 1376 | 2234 | 2604 |

* Wall time is the whole tree (browser start, renderer start-up, page, shutdown), so the JIT barely
  moves it on these short pages. The page's own work does: js_bench kernels, jitless -> JIT (ms):
  integer loops 1129 -> 92, float math 383 -> 97, objects/inline caches 475 -> 235, sort 256 -> 345,
  strings 73 -> 184, closures 67 -> 53, typed arrays 519 -> 209; **total 2767 -> 1251 (2.2x)**.
  `sort` and string building run embedded builtins either way *(design)*; with the JIT they run
  from V8's copy of the builtins, translated fresh in each renderer (see the follow-ups).
  WebAssembly hot loop with the JIT: median 2.9-3.8 ms (jitless V8 has no WebAssembly).
* **Counters** are per-process sums: for each column the larger of the last periodic line
  (`ALCHEMY_FX_STATS_PERIOD=5`) and the exit line per pid, since Chrome SIGKILLs most renderers
  (no exit line) and short-lived ones end before the first period (no periodic line); the last
  <= 5 s of a killed process are missing. "Re-translations" are translations of registered (JIT)
  code at a pc translated before; translation invalidations are in the same `alchemy-fx
  invalidation:` line, but the per-region "invalidated" count is printed only at exit and is
  omitted here. * probe JIT: in 2 of 3 runs the page renderer was terminated before its first
  period; the value is from a 1 s-period rerun on the same binary (r5qp1, 3/3 exit 0, load
  27-79 %, median 978; run 1 of r5q had 817).
* Under load (round 4, 25-100 %) the same cells took 33-52 s, with ~20-60 SMC faults/s: the
  fault count per page is stable, the rate follows wall time.

### Regression check before landing

Main 7f3be67 against round 5 7e83a09, both built the same way (Ninja + shared compiler cache),
interleaved, cloaked, `ALCHEMY_NETWORK=loopback`, on a loaded machine: Chrome strict headless
jitless `--dump-dom` 3/3 exit 0 with `data-js="ran"` on both; QLab's welcome window 2/2 on both;
the renderer add-on headless cube 2/2 on both (add-on enabled, renderer created, render returned, exit 0; no
license under loopback); Blender's Darwin VM calls identical (19 mmap, 8 munmap, 1 mprotect, 0
failures, 0 demand commits). No new error lines; network denials only the expected targets. A
jitless Chrome tree peaks at ~4.5 GB private commit on both. Blender on a quiet machine (measured
right after landing, same two binaries): headless `-b --factory-startup` CPU, 7 interleaved rounds
at 0-15 % load, median 2.34 s (main) vs 2.28 s (stage 3); windowed startup, 3 interleaved runs at
7-15 % load, first 3D viewport draw 2479 vs 2480 ms, first frame 1432 vs 1451 ms; all 20 runs exit
0. No slowdown.

### Not done, and follow-ups

* Renderer start-up dominates a page's wall time: 35 % of the benchmark renderer's samples are the
  framework's dlopen in every renderer (`ModuleLoader`; main's file-backed image segments since
  15aa602 change this, not re-measured here).
* V8's copied embedded builtins are 27 % of translated-code samples and are re-translated in every
  renderer: the copy isn't a registered module, so it never reaches the persistent cache.
  Registering it as an alias of the framework's text would.
* `mach_vm_map` of Mach memory entries returns `KERN_INVALID_RIGHT` about 80 times per run
  (sizes 8, 0x28, 0x1fff0); not a stop.
* `observer_list.h:330 Check failed: observers_.empty()` at teardown in every run (since wave 1,
  non-fatal).
* Executable `MAP_SHARED` file views are refused (macOS allows them); not reached by Chrome.

## Stage 2 result (2026-10-19)

Stage 2 ([ALCHEMY_BRIEF_CHROME_STAGE2.md](ALCHEMY_BRIEF_CHROME_STAGE2.md))
is done on `integ/chrome-s2`. Windowed Chrome 155 runs cloaked with
`ALCHEMY_NETWORK=loopback` and no injected input. Its software compositor's
frames reach the window and the capture, and it quits with exit 0.

* **The run.** `scripts/chrome/run_windowed.ps1 -Frames 3` starts Chrome
  with `--disable-gpu --use-mock-keychain --js-flags=--jitless --no-sandbox`
  and the network-blocking flags. It opens `probe/test.html`, waits until
  the page target has the script-set title and the window capture has
  frames, then sends `Browser.close` over `--remote-debugging-pipe`.
  * The pipe is fds 3 and 4, handed to the root guest process by the
    launcher's new `--fd N=MODE:TARGET` option.
  * Windowed runs are multi-process: no `--single-process` (see "Known
    limitation" below).
* **The capture** (test artifact description; the image itself stays out of
  git): `ALCHEMY_CONTENTS_CAPTURE` writes `window-1.bmp`, 1184x1069, composed
  from the window's CALayer tree, never read from the screen. It shows:
  * the tab strip, with one tab titled "Alchemy probe JS";
  * the toolbar (back, forward, reload) and the omnibox with
    `File /Volumes/C/dev/transmute/artifacts/alchemy-root-chrome/probe/test.html`,
    the star and the profile and menu buttons;
  * Chrome's infobar about the unsupported `--host-resolver-rules` flag;
  * the page: the heading "Hello from Alchemy" and "static page".
* **What is real now** (by group; details in each commit):
  * **Windows (w1):**
    * ordering through `-orderWindow:relativeTo:`, which Chromium overrides
      to track visibility;
    * `occlusionState` and its notification once the window is visible (a
      cloaked window counts as visible);
    * real `NSThemeFrame`/`NSNextStepFrame` frame views with title-bar and
      traffic-light layout;
    * window delegates through the notification center;
    * a view that drops its layer clears the layer's delegate;
    * owned child windows (`WindowServer::set_parent`).
  * **Events (w2):**
    * application delegates hear their notifications through the
      notification center, so Chrome's own
      `NSApplicationWillTerminateNotification` reaches
      `applicationWillTerminate:`;
    * the NSEvent/CGEvent bridge and enter/exit events, and NSTrackingArea
      activity options;
    * the NSCursor stack, and the text input context following the first
      responder;
    * `CFEqual`/`CFHash` of CTFonts and font descriptors.
  * **Compositing (w3):**
    * CALayer trees, with IOSurface or CGImage contents on any sublayer,
      composited into their window on `CATransaction` commit;
    * IOSurface Mach ports, within one process and across processes: the
      `'K'` wire form for foreign kernel objects carries the surface's
      shared pages, so the GPU process's viz frames reach the browser's
      `DisplayCALayerTree`;
    * display-link timing.
  * **Quit and misc (w4):**
    * the `--fd` hand-off and the CDP harness;
    * a child killed with SIGKILL is waitable at once, which fixes the
      headless "Teardown watchdog expired" exit 2 under load;
    * NSWorkspace and accessibility defaults.
  * **Shared with stage 3:** a negative relative `EVFILT_TIMER` interval
    fires at once.
* **Known limitation: windowed `--single-process`.**
  * Windowed Chrome under `--single-process` faults on the browser's
    raster worker (`Google Chrome Framework+0x1e5cd74`, a load through a
    NULL first argument).
  * Cause, by Chromium's public design: in `--single-process` the browser
    expects the in-process renderer to install the
    `DiscardableMemoryAllocator`. Official Chrome 155 still starts
    renderers as child processes, so nobody installs it, and Skia's glyph
    cache allocates through a NULL allocator.
  * Multi-process runs don't fault. Headless runs never rasterize browser
    UI glyphs, so `--single-process` stays fine there.
* **Open:**
  * `--kiosk`: a main-thread trap at `Framework+0x9a3157c` in the
    immersive-mode setup, right after `setAlwaysShowTrafficLights:`. Main
    traps there too.
    * The lead is that title-bar accessory views are stored but never laid
      out.
  * The IOSurface seed and `IOSurfaceSetValue` values are per process.
    IOSurface IDs and use counts are shared across the process tree since
    2b50b4d.
  * `mach_vm_map` of Mach memory entries returns `KERN_INVALID_RIGHT` about
    170 times a minute in Chrome's IPC threads (mojo). This happens in
    headless runs too, and not on the frame path (stage 3 follow-up).
  * The traffic lights aren't drawn in the capture. The frame view lays
    them out, but they are AppKit views under Chrome's full-size content,
    and the composition shows only the layer tree.
  * Tool tips are stored but not shown, and IME composition and context
    menus are not exercised, because no input is injected.
  * Under load, dawn's Metal adapter enumeration in the browser can take
    minutes. It predates this stage (stage 4).
* **Verification.**
  * On `integ/chrome-s2` fa6fb91 (before main 82c0836, stage 3, was merged):
    * `ctest -R "^alchemy_"`: 185/185, cloaked.
    * Headless strict `--dump-dom`: exit 0 in 3 of 3 runs, with
      `data-js="ran"`.
    * Windowed quit: exit 0 in 3 of 3 runs both without frames (1.1–1.7 s
      after `Browser.close`) and with `-Frames 3` (19–22 composited frames,
      3.3–5.9 s after `Browser.close`).
  * On the landing head, after merging main 82c0836:
    * `ctest`: 184/185. `alchemy_gui_windowserver_win32` failed two
      foreground checks under `-j12` and passes 3 of 3 alone; it is the
      wave rules' known environmental case.
    * Headless: 3 of 3 exit 0, `data-js="ran"`.
    * Windowed `-Frames 3`: 3 of 3 exit 0, with 18–30 frames.
  * The regression against main 7f3be67, on fa6fb91 (interleaved, cloaked,
    loopback, machine load 18–100 %):

    | Check | main 7f3be67 | fa6fb91 |
    | --- | --- | --- |
    | Chrome strict headless `--dump-dom`, 3 runs | exit 0, `data-js="ran"` ×3 | exit 0, `data-js="ran"` ×3 |
    | QLab, 2 runs | "Welcome to QLab" drawn, no stops | same; capture byte-identical in all 4 runs |
    | Renderer add-on headless cube, 2 runs | exit 0, renderer created, render 6.5 / 6.4 s | exit 0, renderer created, render 6.3 / 6.4 s |
    | Blender windowed, 3 runs: first 3D viewport draw | exit 0 ×3; 7.6 / 7.2 / 6.4 s | exit 0 ×3; 7.2 / 7.0 / 6.3 s |
    | Blender headless CPU (`-b --factory-startup` + monkey), quiet (load 7–16 %) | 2.27 / 2.42 s | 2.25 / 2.22 / 2.77 s |
    | ... the same at load 80–100 %, 5 rounds | exit 0 ×5; 5.31–6.0 s | exit 0 ×4, 4.3–5.94 s; one exit 1 (see below) |
    | Darwin VM calls in that run (`ALCHEMY_TRACE_CALLS`) | mmap 19, munmap 7, mprotect 1, 0 failures | identical |

    * No new error lines.
    * The Blender headless CPU A/B ran after landing.
      * At 100 % load, one round on fa6fb91 exited 1 after 1.2 s of CPU.
        Its log was overwritten by the next round: the CPU script keeps one
        stderr file per binary. So the cause is unknown.
      * A retry of 6 rounds per binary, with a log each, gave 12 of 12
        exit 0. That makes 1 failure in 11 runs for fa6fb91 and 0 in 11 for
        main, not reproduced.
      * Follow-up: per-round logs in the CPU script, so a recurrence can be
        diagnosed.
  * The follow-up landing 2b50b4d (IOSurface IDs and use counts shared
    across processes):
    * `ctest` 186/186; headless 3 of 3 exit 0; windowed `-Frames 2` 3 of 3
      exit 0, with 27–39 frames.
    * The A/B against fa6fb91, load 15–53 %:
      * QLab's capture is byte-identical (2 runs each).
      * Blender windowed exits 0 in 6 of 6 runs. The first 3D viewport draw
        took 2.72 / 3.27 / 2.87 s on fa6fb91 and 2.55 / 3.01 / 2.61 s on
        2b50b4d.
  * `network denied`: only `[2001:4860:4860::8888]:443` and
    `https://update.googleapis.com` (Chrome), plus QLab's
    accounts.figure53.com and the renderer's license server.
  * The `--remote-debugging-pipe` harness is `scripts/chrome/run_windowed.ps1`.
    Captures stay in scratch directories, never in git.

## Wave 1 result (2026-10-17)

Stage 1 of the [staged plan](#staged-plan) is done, and stage 5's Mach IPC
part with it, on `integ/chrome-w1`.

* **Goal met.** Strict mode (no `ALCHEMY_PROBE_MISSING`) runs

  ```text
  --headless --single-process --no-sandbox --disable-gpu --use-mock-keychain
  --js-flags=--jitless --dump-dom file:///probe/test.html
  ```

  It prints the page's DOM with the effects of its inline script
  (`data-js="ran"`, the script-set title), and exits 0.
* **`--single-process` doesn't keep renderers in-process.** Official
  Chrome 155 still starts every renderer, and the Alerts and utility
  helpers, as child processes. The GPU, network service and audio stay
  in-process. So the dump needed stage 5's cross-process Mach IPC
  (described below), not just the in-process namespace stage 1 planned.
* **Binding.** Strict mode binds the whole tree, including each helper
  inspected on its own.
  * 15 missing identities went to 0.
  * Final `doctor --deps --scan-bundle` on 28 images:
    * 0 of the 518 strong gaps are left.
    * 0 imports are classed as unsupported; `objc_begin_catch`/`_end_catch`
      are provided.
    * 27 unique weak imports bind to NULL, which Chrome checks at run
      time: ScreenCaptureKit, CoreFoundation, VideoToolbox and others.
  * The scan still lists 3 unresolved names and the framework as
    "missing" for the helpers. These are bundle-scan artifacts: the scan
    resolves `@executable_path` against the main executable. Each helper
    binds when inspected on its own.
* **What is real now** (by area; details in each branch's commits):
  * **Memory:**
    * `DarwinVmSpace` on fx: mmap hints, `MAP_FIXED`, partial
      munmap/mprotect of anonymous, file and shared mappings, reservations
      over 1 TiB, `MAP_JIT` and madvise;
    * `mach_vm_region(_recurse)` returns per-page protection runs, and
      `mach_vm_purgable_control` works;
    * cross-process host sections (`cross_process_memory.hpp`).
  * **libSystem:**
    * a real malloc zone registry with a complete system zone;
    * AF_UNIX stream sockets over Windows AF_UNIX, and symlink records;
    * NTFS-stream xattrs, POSIX record locks and devfs nodes;
    * the struct layout fixes (`statvfs`), and a process tree with stable
      pids.
  * **Mach:**
    * an in-process port namespace and `mach_msg` with descriptors;
    * kevent `EVFILT_MACHPORT`/`TIMER`/`VNODE` with `EV_CLEAR`;
    * dispatch MACH_RECV/SEND/MEMORYPRESSURE/PROC/READ/WRITE/VNODE sources,
      and libnotify;
    * across processes: a process-tree bootstrap broker over named pipes,
      right and message transfer, dead names, special ports from
      `posix_spawnattr_setspecialport_np`, task ports, `task_policy` and
      `task_info`.
  * **Frameworks:**
    * every missing identity;
    * framework bundle resources, CFURL for file URLs and `CFStringCompare`
      options;
    * IOKit power notifications, SCDynamicStore run-loop sources and
      reachability;
    * the macOS 13–15 menu API, NSMenu copying and NSSpellChecker;
    * blocks as CF objects and NSURL resource values, with many AppKit
      selectors.
* **Network policy.** `ALCHEMY_NETWORK=loopback` (src/alchemy/host/common/network_policy.cpp)
  keeps every Alchemy process in the tree on this machine.
  * It gates sockets, DNS, dns_sd, WinHTTP and PAC, and logs each refused
    attempt.
  * Chrome runs need it: GoogleUpdater posts to `update.googleapis.com`
    through NSURLSession, and the browser probes `[2001:4860:4860::8888]:443`
    by numeric address. Chrome's own flags don't cover either.
* **Known, not blocking:**
  * `Check failed: observers_.empty()` at profile shutdown, after
    `~AccountReconcilor`. It is non-fatal; the exit code stays 0. Its
    cause is not identified.
  * The crashpad handshake and an occasional shutdown `bootstrap_look_up`
    to the broker have no answer.
  * "DNS config watch failed", because `dns_configuration_*` is absent.
* **Next stages:** windowed Chrome (stage 2), the JIT (stage 3) and the GPU
  process (stage 4). Their briefs replace the rows of the staged plan below.

The sections below are the initial measurement, taken before wave 1.

## Initial measurement (before wave 1)

* **Load.** Every Mach-O of `Google Chrome.app` parses.
* **Probe mode.** In probe mode the whole 520 MB framework loads and binds:
  run through an Aperitif helper, 441 stubs and 82 placeholder classes.
* **Where execution stops.** Execution ends within the first hundred
  provider calls, in Chrome's allocator start-up, with:

  ```text
  Failed to install the delegating zone as default.
  alchemy: Darwin abort requested SIGABRT; ... [guest pc 0x710000030e20, thread 1]
    lr 0x100000c0c Google Chrome+0xc0c
  ```

  This happens before `main` parses the command line, so no flag avoids
  it. Headless `--dump-dom` was not reached, and so the windowed run was
  not attempted.
* **Strict mode.** Strict mode stops one step earlier, at bind time:
  `alchemy: Mach-O: unresolved strong import _malloc_zone_register from
  library ordinal 1`.
* **Inventory.**
  * 2,804 unique imports in 26 arm64 images.
  * 2,144 bind to providers; 81 more bind to stubs that, by a source
    heuristic, only fail.
  * 518 strong imports are unresolved or come from one of 15 missing
    identities; 56 weak imports bind to NULL.
  * Objective-C: 82 referenced classes and 643 selectors have no
    implementation.

## Setup and reproduction

```powershell
git worktree add .claude/worktrees/chrome -b mac/chrome-probe mac/qlab
cmake -S . -B build-c -G "Visual Studio 17 2022" -A x64 -DTRANSMUTE_WITH_FX=ON `
  -DALCHEMY_LLVM_RUNTIME_DIR=C:/dev/transmute/artifacts/alchemy-llvm-arm64 ...
cmake --build build-c --config Release --target alchemy-fx -- /m:4
scripts/alchemy_install_dmg.ps1 -Dmg C:\dev\transmute\artifacts\downloads\chrome-20261016\googlechrome.dmg `
  -Root C:\dev\transmute\artifacts\alchemy-root-chrome -Alchemy build-c\Release\alchemy-fx.exe
scripts/alchemy_chrome_inventory.ps1 ...      # docs/ALCHEMY_CHROME_GAPS.md, "Input and method"
```

* **Input.** The input is the official universal DMG, 275,971,452 bytes,
  sha256 `32036e8aab250e5d92bd63a31bd46e6dede7ac40c13dbbd6179b327a835d93c3`.
  It is Chrome 155.0.8059.85, and its bundle ID is `com.google.Chrome`. It
  is not redistributed.
* **The root.** The bundle is installed into
  `C:\dev\transmute\artifacts\alchemy-root-chrome` with 6 links. The runs use
  a scratch `HOME` (`<root>\home`), a per-run `--user-data-dir` under
  `<root>\profiles`, and a local `file://` page (`<root>\probe\test.html`).
* **Run settings.** Runs are headless and cloaked: `ALCHEMY_GUI_CLOAK=1`,
  `ALCHEMY_GUI_NO_ACTIVATE=1` and `ALCHEMY_GUI_NO_CRASH_DIALOG=1`. No input is
  injected. The Chrome flags are `--headless --no-sandbox --disable-gpu
  --use-mock-keychain --js-flags=--jitless --dump-dom <file url>` plus
  `--no-first-run --disable-background-networking --disable-component-update
  --disable-sync --disable-default-apps --metrics-recording-only
  --disable-breakpad`. Chrome never reached its network code, so no traffic
  left the machine.
* **Long paths: required, and not committed.** `alchemy install` failed
  first:

  ```text
  alchemy: directory_iterator::directory_iterator: The system cannot find the path specified.:
  "...\Helpers\Google Chrome Helper (Aperitif Alerts).app\Contents\Resources\base.lproj"
  ```

  * **Cause.** The deepest bundle path is 255 characters below
    `Google Chrome.app`
    (`Helpers/GoogleUpdater.app/.../GoogleSoftwareUpdateAgent.app/Contents/MacOS/GoogleSoftwareUpdateAgent`),
    so every installed path passes `MAX_PATH`. The host has
    `LongPathsEnabled=1`, but `alchemy(-fx).exe` has no `longPathAware`
    manifest.
  * **Local workaround.** The build used for this audit adds one: a
    `src/alchemy/alchemy.manifest` source on both executables. It is kept
    out of the commit, because the scope of this branch is inventory only.
  * **Work item.** This is a one-line build change, a prerequisite for
    installing Chrome. The probe runs themselves stay below `MAX_PATH`.

## The bundle

| Image | Arm64 role |
| --- | --- |
| `Contents/MacOS/Google Chrome` | Tiny launcher. Its only load dependency is libSystem. It `dlopen`s the framework and calls `ChromeMain` *(design: `chrome_exe_main_mac.cc`)*. Before that it runs PartitionAlloc's early malloc-zone registration. |
| `Google Chrome Framework` (520 MB universal) | All of Chrome: 2,697 import records, 174 Objective-C classes, 3,594 selector references, statically linked libc++/libc++abi, no Swift. |
| Helpers `Google Chrome Helper`, `(GPU)`, `(Renderer)`, `(Alerts)` | Child-process launchers. They link libSystem and libsandbox only, and `dlopen` the framework like the main executable. |
| Helpers `(Aperitif)`, `(Aperitif GPU/Renderer/Alerts)` | New in this generation: they link `../../../../Google Chrome Framework` (`ChromeMain`) and `Libraries/libaperitif.dylib` (`AperitifCheckInitialized`) directly. libaperitif's initializer runs the same zone registration. |
| `chrome_crashpad_handler` | Crashpad: Mach exception server, `task_threads`/`thread_get_state`, `posix_spawn`. |
| `app_mode_loader`, `web_app_shortcut_copier` | PWA app shims. Not used by a browser run. |
| `libvk_swiftshader`, `libvulkan` | SwiftShader Vulkan (software GPU fallback; it has its own JIT) and the Vulkan loader. |
| `liboptimization_guide_internal`, `libchromecompaneros`, Widevine (arm64; the x64 slice is rejected) | Loaded on demand. |
| GoogleUpdater, Keystone (`ksadmin`, `ksinstall`, agent), privileged helper | Updater processes. They are not part of the browser process tree. |

## Doctor summary

`doctor --deps --scan-bundle --json` covers 27 images. Each executable is
also re-inspected on its own, because a bundle scan resolves
`@executable_path` against the main executable. Full tables:
[ALCHEMY_CHROME_GAPS.md](ALCHEMY_CHROME_GAPS.md).

* **Missing identities (15).**
  * 13 strong: AVFAudio, Accessibility, AuthenticationServices,
    CoreHaptics, CoreML, CoreMediaIO, CryptoTokenKit,
    LocalAuthenticationEmbeddedUI, MediaPlayer, Network, SafariServices,
    UserNotifications and Vision.
  * 2 weak: ScreenCaptureKit and SkyLight.

  All are linked by the framework. CryptoTokenKit and Network are also
  linked by the updater images; CryptoTokenKit also by the app shim and Widevine. The 13
  strong identities carry 102 imports.
* **Unresolved strong imports, by library (unique names, all images).** The
  first number is unresolved or missing, the second is weak-unresolved.

  | Library | Strong / weak | What is in it |
  | --- | --- | --- |
  | AppKit | 130 / 7 | 103 `NSAccessibility*` constants. Touch Bar, sharing, `NSVisualEffectView`, `NSStatusBarButton`, `NSTextAttachment`, `NSPerformService`, workspace power notifications. Weak: `NSThemeFrame`, `NSNextStepFrame`, `_NSThemeWidget`, `NSGlassEffectView` |
  | libSystem | 65 / 2 | `malloc_zone_register/unregister`. 10 `_Unwind_*` and `__register_frame`/`__deregister_frame`. `mach_vm_region_recurse`, `mach_vm_purgable_control`, `task_threads`, `task_set_special_port`, `posix_spawnattr_setspecialport_np`, `task_create_identity_token`, `bootstrap_parent`, `mach_port_type`/`get_refs`. `csops`, `csr_get_active_config`, `___mac_syscall`. 10 `swap_*` Mach-O byte-swap helpers, 6 `acl_*`, `getopt_long`, `err*`, `getprogname`, `nan`/`nanf`, QoS override calls |
  | ApplicationServices | 31 / 0 | `AXUIElement*`/`AXObserver*`/`AXTextMarker*` (accessibility client), `PM*` print-settings data, `UAZoomChangeFocus` |
  | Security | 23 / 0 | Code signing (`SecCodeSigner*`, `kSecCodeInfo*`, `kSecGuestAttribute*`), legacy keychain ACLs (`SecAccess*`, `SecACL*`, `SecKeychainItemCreateFromContent`) |
  | Network (missing) | 22 / 0 | `nw_path_monitor_*` (network change), `nw_browser_*` (Bonjour discovery) |
  | VideoToolbox | 21 / 5 | Encoder property keys (HEVC, HDR, low latency) |
  | Vision, AuthenticationServices, MediaPlayer, UserNotifications, CoreHaptics, CoreML, AVFAudio (missing) | 20, 14 / 6, 11, 11, 9, 5 / 4, 5 | Barcode/face detection, passkeys, Now Playing, notifications, haptics, on-device ML, speech synthesis |
  | CoreFoundation | 15 / 5 | `CFDictionaryApplyFunction`, `CFStringAppend`, `CFNotificationCenterGetDarwinNotifyCenter`, `CFPreferencesAppValueIsForced`, `NSInputStream` (subclassed), private `_CFURL…PropertyListRepresentation`. Weak: `_CFCopySystemVersionDictionary` |
  | libcups | 15 / 0 | IPP request building (printing) |
  | CoreVideo / CoreMedia / AVFoundation | 14 / 7 / 6 (+3 weak) | Pixel-format descriptions, clean-aperture keys, HEVC format descriptions, capture photo output, `AVSampleBufferAudioRenderer` |
  | CoreServices / Carbon | 11 / 6 | Apple Events send/handlers, `LSRegisterURL`, Carbon event loop calls |
  | GameController | 11 / 0 | DualSense/DualShock/Xbox pads, haptics localities |
  | CFNetwork / Foundation | 6 / 6 | `NSURLSession`, `NSURLRequest`, `NSHTTPURLResponse` (GoogleUpdater only); `NSURLProtocol` (subclassed), `NSURLQueryItem`, `NSUserActivity` |
  | libsandbox | 6 / 0 | `sandbox_compile_string`, `sandbox_apply`, `sandbox_create_params`, ... |
  | Others (≤ 5 each) | 40 | CoreGraphics (4), CoreText (4), CoreMIDI (4), CoreAudio (3 + 3 weak, process taps), OpenDirectory (3), SystemConfiguration (3, `SCNetworkInterface*`), UniformTypeIdentifiers (3), libbz2 (3 decompress calls), libobjc (5: `objc_begin_catch`/`objc_end_catch` unsupported, `imp_implementationWithBlock`, `objc_copyCppObjectAtomic`, `objc_setExceptionPreprocessor`), Metal (2 + 2 weak), DiskArbitration (2), IOKit (2), AudioToolbox (2), IOSurface (`IOSurfaceSetPurgeable`), ServiceManagement (`SMJobBless`), libbsm (`audit_token_to_euid`) |

* **Weak-only:** 56 weak imports bind to NULL.
  * ScreenCaptureKit (13), AuthenticationServices (6), VideoToolbox (5),
    CoreFoundation (5), CoreML (4) and AVFoundation (3).
  * AppKit's private frame classes (7).
  * QuartzCore's `CAMetalDisplayLink` and the `CADynamicRange*` keys (3).
  * Metal's `MTLCommandQueueDescriptor`/`MTLLogStateDescriptor`, CoreAudio
    process taps, SkyLight's `SLSGetDisplayLink`, and
    `posix_spawn_file_actions_addchdir`.

  Chrome checks these at run time.
* **Bound but failing.** By the source heuristic, 81 imports bind to stubs
  that only fail. The list is in
  [ALCHEMY_CHROME_GAPS_SYMBOLS.md](ALCHEMY_CHROME_GAPS_SYMBOLS.md).
  * Process: `mach_port_allocate`/`construct`, `bootstrap_look_up`/`check_in`,
    `mach_msg_send`, `mach_make_memory_entry_64`, `launch_msg`, `notify_register_*`.
  * Crashpad: `thread_get_state`, `thread_set_exception_ports`,
    `host_set_exception_ports`, `task_suspend`/`resume`.
  * Sandbox: `sandbox_extension_*`.
  * Media: all of `AudioConverter*`, ForceFeedback.
  * Window server: the private `CGSSetWindow*` calls.
  * The heuristic misses `mach_msg` itself, which fails every send by design.

## Gap classification by area

Counts are unique imports of the browser-process tree (no updater), from the
area table in the gaps file. "Missing" means strong gaps; "fails" means bound
error or stop stubs.

### Process model: Mach ports, bootstrap, XPC (3 missing, 15 fail; plus `mach_msg`)

* **What machsys provides.** machsys deliberately has no Mach port
  namespace:
  * port allocation and construction fail with `KERN_NOT_SUPPORTED`;
  * every `mach_msg` send fails with `MACH_SEND_INVALID_DEST`;
  * bootstrap answers `BOOTSTRAP_UNKNOWN_SERVICE`;
  * `kevent` supports only `EVFILT_READ`/`EVFILT_WRITE`, and
    `EVFILT_MACHPORT` fails with `ENOTSUP`.
* **Even a single process needs real in-process Mach ports.** *(design)*
  * base's IO thread on macOS is `MessagePumpKqueue`. It allocates a
    port set and a wake-up receive right and waits in `kevent64` with
    `EVFILT_MACHPORT`. It `MACH_CHECK`s the allocation.
  * Mojo's macOS channel transport (`ChannelMac`) sends messages and port
    rights with `mach_msg`.
  * `base::PlatformSharedMemoryRegion` on macOS is a Mach memory entry:
    `mach_make_memory_entry_64` plus `mach_vm_map`. It fails today.

  So `--single-process` still needs a Mach port namespace inside the
  process: receive, send and send-once rights, port sets, `mach_msg` with
  timeouts and inline/OOL/port descriptors, dead-name and no-senders
  notifications, memory entries, and `EVFILT_MACHPORT`.
* **Multi-process needs it across processes.** On top of the in-process
  namespace, every child needs:
  * a bootstrap server, for `MachPortRendezvousServer`, which the browser
    checks in and the children look up;
  * `posix_spawnattr_setspecialport_np` (unresolved);
  * transfer of rights between Alchemy processes, which are separate
    Windows processes;
  * task ports for `base::ProcessMetrics`.
* **XPC.** No `xpc_*` import appears in the browser images. Only the
  updater uses `NSXPCConnection`. `SMJobBless` is the updater's.
* **Effort.**
  * In-process Mach IPC, memory entries and `EVFILT_MACHPORT`: **L, about
    1.5 waves.**
  * Cross-process brokering: **L, about 1.5 waves more**.

### Sandbox: `sandbox_init*`, Seatbelt (8 missing, 2 fail)

* **Imports.**
  * libsandbox's `sandbox_compile_string`, `sandbox_create_params`,
    `sandbox_set_param` and `sandbox_apply` (the helpers' `sandbox_apply`
    too).
  * `csops`, `csr_get_active_config`, `___mac_syscall`.
* **Today.** machsys refuses `sandbox_init`, and `sandbox_check` answers
  "allowed". `--no-sandbox` skips the Seatbelt path *(design)*.
* **Effort.**
  * Identities plus truthful refusals or a documented non-enforcing
    apply: **S**.
  * Real confinement (an AppContainer/job mapping of the profile):
    **XL**, and optional.

### GPU and compositing: Metal, IOSurface, CoreAnimation (3 missing, 1 fails, 6 weak)

* **Link coverage is nearly complete.** IOSurface (37 of 38),
  QuartzCore (18 of 21, the rest weak) and Metal (26 of 30) are linked and
  provided.
  * Missing: `IOSurfaceSetPurgeable` and `MTLCommonCounter*` timestamps.
  * Weak: `CAMetalDisplayLink` and `MTLCommandQueueDescriptor`.
* **Remote layers.** The remote-layer classes `CAContext` and
  `CALayerHost` appear only as C strings, not as imports.
  * Chrome looks them up by name and falls back to IOSurface transport
    when they are absent *(design: `ui::RemoteLayerAPISupported`)*.
  * The runtime registers neither, so the fallback path would be taken.
* **What a GPU process needs** *(design)*:
  * ANGLE's Metal backend, with MSL generated at run time
    (`newLibraryWithSource:`);
  * Skia Graphite / Dawn on Metal;
  * `MTLSharedEvent`;
  * IOSurface-backed `MTLTexture`s shared with CoreAnimation, and
    `CAMetalLayer`.

  The fallback is `libvk_swiftshader` on the Vulkan loader, which needs the
  JIT.
* **Effort.**
  * Software compositing to an IOSurface-backed `CALayer.contents`: **M**.
  * ANGLE-Metal plus Dawn/Graphite coverage on Alchemy's Metal-to-Vulkan
    layer: **XL, 4 waves or more**.

### JIT (memory API: 4 missing; see below)

* **Linked:** `pthread_jit_write_protect_np`, `sys_icache_invalidate`,
  `mmap`, `mprotect`, `madvise` and `vm_*`.
* **Missing:** `malloc_zone_register`/`unregister`, `mach_vm_region_recurse`
  and `mach_vm_purgable_control`.
* **Run-time semantics** are the real gap. See the
  [JIT analysis](#jit-analysis).

### Keychain and Security (39 missing, 1 fails, 6 weak)

* **Security provider.** Alchemy's Security provider binds 107 of 131
  names.
  * Missing: code-signing introspection, legacy keychain ACLs, and the
    passkey and smart-card frameworks (AuthenticationServices,
    CryptoTokenKit, LocalAuthenticationEmbeddedUI).
* **Keychain at start-up.** `--use-mock-keychain` replaces the OS
  keychain for `OSCrypt` ("Chrome Safe Storage") *(design)*.
* **Effort.**
  * Identities and explicit failures: **S**.
  * Passkeys: out of scope.

### Crashpad: Mach exception ports (1 missing, 7 fail)

* **Handler imports.** The handler image imports `task_threads`
  (unresolved), `thread_get_state`, `thread_set_exception_ports`,
  `host_set_exception_ports`, `task_suspend`/`resume` (all failing) and the
  Mach IPC above.
* **Start-up** *(design)*. The browser starts the handler with
  `posix_spawn` and hands it a receive right. Without Mach ports,
  `StartHandler` fails. Chrome keeps running without crash reports.
  `--disable-breakpad` disables uploads only. Not verified.
* **Effort.** Fault delivery from fx to an exception server in another
  process: **L, about 1 wave**, and optional.

### AppKit UI (198 missing, 14 fail, 9 weak)

* **Missing.**
  * 103 accessibility constants; ApplicationServices `AX*` (31);
  * Touch Bar and sharing;
  * `NSVisualEffectView`, `NSStatusBarButton`, `NSTextAttachment`;
  * the subclassed private frame classes `NSThemeFrame` and
    `NSNextStepFrame` (weak imports);
  * Carbon event calls; `CGSSetWindow*` (failing).
* **Subclassed classes.** Chrome subclasses NSApplication, NSWindow,
  NSWindowController, NSView, NSViewController, NSCursor, NSTrackingArea,
  NSAnimation, NSAccessibilityElement and
  NSTitlebarAccessoryViewController.
* **What Chrome uses.** Chrome's UI is Views/Skia, drawn into its own
  surfaces. It uses AppKit for:
  * windows, events and IME (`NSTextInputClient`);
  * menus, cursors, drag and drop and tool tips;
  * the custom frame view (`BrowserNativeWidgetWindow` on
    `NSThemeFrame`) *(design)*.

  It does not use the AppKit control set QLab needs.
* **Headless.** New headless (the only `--headless` in Chrome 132 and
  later) still runs `BrowserCrApplication` and an NSApplication run loop
  *(design, not verified)*.
* **Effort.**
  * Identities and constants: **S**.
  * Windowing, events, IME, cursor and frame-view subclassing for
    windowed Chrome: **L, about 1.5 waves**.

### Media: audio and video (119 missing, 29 fail, 28 weak)

* **Imports.** VideoToolbox encoder keys, CoreVideo/CoreMedia format
  descriptions, AVFoundation capture and renderers, `AudioConverter*`
  (failing), CoreAudio aggregate devices and taps, CoreMIDI 2.0 event
  lists, ScreenCaptureKit, MediaPlayer, CoreHaptics, GameController, Vision
  and CoreML.
* **When they run.** Nothing here runs for a static page *(design: media
  is lazily initialized; audio output starts with playback)*.
* **Effort.**
  * Identities and explicit failures: **M, about 0.5 wave**.
  * Working playback (AudioConverter and HAL output; VideoToolbox decode or
    software decode, which Chrome has for most codecs): **L**.

### Networking: SystemConfiguration, Network.framework (28 missing)

* **Missing.** Network.framework is a missing identity: 22 names,
  `nw_path_monitor_*` and `nw_browser_*`. Also `SCNetworkInterface*`,
  and OpenDirectory keys. The 6 `CFNetwork` `NSURLSession` classes are
  GoogleUpdater's only. GSS (Kerberos) is `dlopen`ed by name.
* **What Chrome's own stack uses.** Chrome's network stack is its own,
  over BSD sockets, which Alchemy provides. It uses SystemConfiguration and
  `nw_path_monitor` for change notification and proxy settings *(design)*.
* **Effort.** A Network.framework path monitor over host notifications,
  plus explicit Bonjour failure: **M**.

### Fonts and text (4 missing)

* **Coverage.** CoreText is 71 of 75. Missing:
  `CTFontCollectionCreateMatchingFontDescriptorsForFamily`,
  `CTFontCreateForStringWithLanguage` (fallback fonts),
  `CTFontCopyVariationAxesInternal` and `kCTFontVariationAxesAttribute`.
* **Use.** Skia's macOS font manager is CoreText-based *(design)*, so
  `--dump-dom` layout depends on these.
* **Effort:** **S to M.**

### Everything else (88 missing, 12 fail)

* **Groups.** The libSystem leftovers listed above (unwind, `swap_*`,
  ACLs, getopt, err), CoreFoundation (15), libcups (15), Apple Events
  (CoreServices/Carbon), DiskArbitration, IOKit HID access, libbz2
  decompress and libobjc.
* **The C++ unwinder.** `_Unwind_*` from libSystem matter early. The loader
  binds them from a resident LLVM `libunwind`, but Chrome links its own
  libc++ statically, so nothing loads `libunwind.dylib`. Every Chrome image
  therefore has them unresolved. A strict run needs libunwind made resident
  for such images, or a provider: **S**.
* **Objective-C exceptions.** `objc_begin_catch`/`objc_end_catch` are
  unsupported (doctor: "unsupported Objective-C exceptions"). Chrome builds
  with `@try` in a few places *(design)*. They bind and fail on use: **M**
  for real support.

## Probe frontier

The test HTML is a local file with a heading, a paragraph and one inline
script. Logs are in the scratchpad, `runs/<name>/`.

| Run | Program and mode | Result | Stop |
| --- | --- | --- | --- |
| p1-zero | `/Applications/Google Chrome.app`, `ALCHEMY_PROBE_MISSING=zero` | exit 1, 0.4 s | `[alchemy probe] zero-return ... _malloc_zone_register`, `... _malloc_zone_unregister`, then the guest's `Failed to install the delegating zone as default.` and `alchemy: Darwin abort requested SIGABRT; signal delivery is unsupported during synthetic guest call ... lr 0x100000c0c Google Chrome+0xc0c` |
| p3-single-zero | as p1 plus `--single-process --enable-features=NetworkServiceInProcess --in-process-gpu` | exit 1, 0.4 s | identical: the stop precedes argument parsing |
| p4-trap | as p1, `ALCHEMY_PROBE_MISSING=trap` | exit 1, 0.2 s | `alchemy: Alchemy probe: called missing /usr/lib/libSystem.B.dylib _malloc_zone_register ... lr 0x100000af4 Google Chrome+0xaf4` |
| p2-aperitif-zero | `.../Helpers/Google Chrome Helper (Aperitif).app` as the program (it links the framework directly), same flags | exit 1, 1.2 s | Loads and binds the whole framework (344 function and 97 data stubs, 82 placeholder classes), then `libaperitif.dylib`'s initializer stops the same way: `guest module initialization failed for ...libaperitif.dylib: Darwin abort requested SIGABRT ...` |
| s1-strict | `/Applications/Google Chrome.app`, probe off | exit 1, 0.3 s | `alchemy: Mach-O: unresolved strong import _malloc_zone_register from library ordinal 1` |
| s2-aperitif-strict | the Aperitif helper, probe off | exit 1, 0.8 s | `alchemy: missing guest dylib: /System/Library/Frameworks/CryptoTokenKit.framework/Versions/A/CryptoTokenKit` |

* **The path to the abort.** `ALCHEMY_TRACE_CALLS=_` (runs t1, t2) shows
  the same path for both entry points, after Alchemy's own start-up:
  1. `malloc_default_zone`, `malloc_default_purgeable_zone` and
     `malloc_get_all_zones`, which returns one zone;
  2. `malloc_zone_register` and `malloc_zone_unregister`, three times in
     all (probe stubs);
  3. `malloc_default_zone` and `malloc_get_all_zones` again;
  4. `abort_report_np` and `abort`.

  This is PartitionAlloc's `EarlyMallocZoneRegistration` *(design)*. It
  registers a delegating zone, then unregisters and re-registers the
  system default zone so that its own zone becomes zone 0, and aborts when
  `malloc_default_zone()` is not its zone.
* **No flag avoids it.** The stop is in `main` before argument parsing (and
  in libaperitif's static initializer), so it is the frontier for every
  flag combination. The Aperitif variant only shows that the framework
  itself loads.
* **Windowed run.** Not attempted, because the DOM dump was not reached.

### Predicted next stops (static, not verified)

Read from Alchemy's sources and Chromium's design. Each one is a stage-1
work item.

1. **Strict binding.** In order:
   * the 9 or 10 launcher imports (`malloc_zone_*`, `_Unwind_*`);
   * the framework's 13 missing identities;
   * its 371 unresolved names.
2. **PartitionAlloc reservations.** PartitionAlloc reserves its pools with
   `mmap(hint, …)` using random advisory hints, trims alignment slack with
   partial `munmap`, and decommits with `mmap(MAP_FIXED)` *(design)*.
   Alchemy's `mmap` rejects all three:
   * any non-null address → `EINVAL` (`darwin_services.cpp`, `_mmap`);
   * `MAP_FIXED` → `EINVAL`;
   * a partial `munmap` → "range unmapping for partial Darwin munmap".

   The result would be an out-of-memory crash at the first partition.
3. **The V8 sandbox reservation** (on even with `--jitless`). It is about
   1 TB plus guard regions. Alchemy caps one reservation at `1 << 40`, and
   V8 then falls back to a partially reserved sandbox *(design)*.
4. **Mach ports.** `MessagePumpKqueue` (IO thread) and Mojo channel
   creation hit `mach_port_allocate` → `KERN_NOT_SUPPORTED`, `kevent64`
   `EVFILT_MACHPORT` → `ENOTSUP`, and
   `mach_make_memory_entry_64` → failure (shared memory regions).
5. **Then** the Objective-C start-up of `BrowserCrApplication`, CoreText
   font enumeration, the crashpad handler spawn, and the Network.framework
   path monitor.

## JIT analysis

*Written before stage 3; [Stage 3 result](#stage-3-result-2026-10-19-v8-with-its-jit-on) has
what runs now. `MAP_JIT` was accepted in wave 1, and `sys_icache_invalidate` is no longer a no-op.*

How fx handles code written at run time today:
* **Memory.** Guest memory is host memory at the same address, and guest
  pages are never host-executable. `mmap`/`mprotect` with `PROT_EXEC` call
  `FxEngine::set_memory_access`/`protect_range`. These register the range
  as guest code (`fx::register_guest_code_memory`), and any writable or
  non-executable change invalidates its translations
  (`fx::invalidate_guest_code`).
* **RWX.** RWX is allowed: there is no W^X enforcement.
* **Self-modifying code.** fx write-protects every writable guest page it
  has translated (`protect_pages`). A later store faults in the VEH:
  `smc_handle_write_fault` invalidates that page, restores its protection
  and retries the store.
  * Code written and then executed is translated on first execution from
    the current bytes.
  * Code rewritten after translation is caught by the fault.

  This is the same coherence model fx uses for Windows JITs.
* **Cache flush.** `sys_icache_invalidate` calls `invalidate_engine_code`.
  FxEngine has no `invalidate_code`, so the call is a no-op that relies on
  the SMC faults above (ALCHEMY_FX.md lists exposing it as an open item).
  That is correct for same-mapping writes. It does not cover a write
  through a second mapping that fx does not know as an alias. fx's shared
  memory aliases are protected on both views.
* **`pthread_jit_write_protect_np`.** A no-op (machsys). This is correct
  because nothing is ever execute-only.
* **`MAP_JIT` is rejected.** `mmap` accepts only `MAP_PRIVATE` and
  `MAP_PRIVATE|MAP_ANON` (after the shared cases), so `MAP_JIT` (0x800)
  fails with `EINVAL`.
  * PartitionAlloc adds `MAP_JIT` only when the process has the
    `com.apple.security.cs.allow-jit` entitlement *(design)*.
  * Alchemy's `SecTaskCopyValueForEntitlement` returns NULL (no
    entitlements), so Chrome would not ask for it, and would map code space
    RWX through `mprotect`, which Alchemy allows.
  * Accepting `MAP_JIT` as a no-op flag is still the safer fix.
* **Translation cache.** JIT code is not in a registered module, so it never
  enters fx's persistent cache. That is correct, with nothing stale to load.

**Would V8's JIT work today?** No, but not because of code generation:
* **The blocker is memory.** V8's code range and cage are reservations
  made through PartitionAlloc's page allocator, so they need advisory
  hints, `MAP_FIXED` inside a reservation, partial `munmap` and large
  aligned reservations. `--jitless` needs the same reservations for the
  heap cage and sandbox, which makes this a stage-1 item anyway.
* **Once that works, the JIT is expected to work.** Coherence is handled by
  the SMC faults; `pthread_jit_write_protect_np` and `sys_icache_invalidate`
  are already harmless. Two concerns remain:
  * **Cost.** Each patch to a translated code page (inline-cache updates,
    relocation during code-space compaction, lazy deoptimization) costs a
    VEH fault and a page invalidation. Toggling a code range writable
    invalidates it all. Expect noticeable overhead on JavaScript-heavy
    pages until fx gets cheaper invalidation (an explicit
    `invalidate_code` and sub-page granularity).
  * **SwiftShader and WebAssembly.** SwiftShader (`libvk_swiftshader`) and
    WebAssembly use the same path.
* **Not verified.** Chrome never reached V8. `--js-flags=--jitless` stays
  the default until stage 3.

## Staged plan

These estimates are in agent-waves. A wave is one batch of parallel agents
with disjoint file ownership, as in the QLab G1-G6 plan, each owning about
3k lines. The ranges are rough and come from the inventory counts above.

| Stage | Goal | Work | Estimate |
| --- | --- | --- | --- |
| 0 | Install | `longPathAware` manifest on `alchemy(-fx).exe` (the local diff of this audit) | trivial |
| 1 | Headless, jitless, `--no-sandbox`, `--single-process`: `--dump-dom` of a local page | (a) malloc zone API: registry, default-zone order, `malloc_get_all_zones`, `malloc_zone_from_ptr` (S). (b) resident libunwind for images that import `_Unwind_*` from libSystem (S). (c) identities for the 13 missing frameworks and explicit failures for the framework's 475 strong gaps, so that strict mode binds (M; QLab-style, mostly constants and classes). (d) memory API for PartitionAlloc/V8: advisory `mmap` hints, `MAP_FIXED` inside own reservations, partial `munmap`, more than 1 TiB reservations or V8's fallback, `MAP_JIT` accepted, `mach_vm_region_recurse`, `mach_vm_purgable_control` (M). (e) in-process Mach ports: rights, port sets, `mach_msg` with descriptors, notifications, memory entries + `mach_vm_map`, `kevent64` `EVFILT_MACHPORT` (L). (f) the dynamic gaps then found in `BrowserCrApplication` start-up, CoreText fallback fonts, Network path monitor and the crashpad spawn (M, unknown until reached) | **3-4 waves** |
| 2 | Windowed, CPU rendering, cloaked capture | NSApplication/NSWindow subclass behaviour, `NSThemeFrame`/`NSNextStepFrame` frame-view subclassing, events and IME (`NSTextInputClient`), menus, cursors, tracking areas, tool tips; software-compositor output to IOSurface-backed `CALayer.contents` presented by QuartzCore; accessibility constants | **2-3 waves** |
| 3 | JIT on | Re-verify the stage-1 memory work under V8's code range; explicit `invalidate_code` on FxEngine for `sys_icache_invalidate`; measure the SMC fault rate and, if needed, cheaper invalidation in fx; WebAssembly | **1-2 waves** |
| 4 | GPU process with Metal and IOSurface | ANGLE Metal backend and Dawn/Graphite Metal on Alchemy's Metal to Vulkan, with run-time MSL; IOSurface-backed `MTLTexture`; `MTLSharedEvent`; `CAMetalLayer` presentation; `IOSurfaceSetPurgeable`; or SwiftShader Vulkan as an intermediate step (needs stage 3) | **4-6 waves** |
| 5 | Sandbox and multi-process | Cross-process Mach port broker (bootstrap server, rendezvous, right and memory-entry transfer between Alchemy processes), `posix_spawnattr_setspecialport_np`, task ports; renderer, GPU and network/utility helpers as Alchemy children; Seatbelt calls bound and non-enforcing (documented), or an AppContainer mapping; optional crashpad exception delivery | **4-6 waves** (+1 for crashpad) |

**Total: about 14 to 21 waves** to multi-process, GPU-accelerated Chrome.
Stage 1 has the most uncertainty: only its first stop is measured, and every
later item is predicted.

## Not verified

* Every statement marked *(design)*. They come from Chromium's public
  sources, not from this binary's behavior. That includes:
  * the PartitionAlloc and V8 reservation patterns;
  * `MessagePumpKqueue`/`ChannelMac` use of Mach ports in single-process
    mode;
  * crashpad's behavior without Mach ports;
  * `RemoteLayerAPISupported`;
  * whether headless runs an NSApplication loop.
* The error/stop classification is a source heuristic.
* The selector gap check is class-agnostic.
* The class-name and library-name candidates are string matches.
* No page was loaded, no DOM was dumped and no window was opened, so
  networking, fonts, media and GPU were never exercised.
