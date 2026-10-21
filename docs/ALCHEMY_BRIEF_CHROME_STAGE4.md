# Brief: Chrome stage 4, GPU process on Metal

The rules for running this wave are in
[ALCHEMY_WAVE_RULES.md](ALCHEMY_WAVE_RULES.md). For background, see
[ALCHEMY_CHROME_AUDIT.md](ALCHEMY_CHROME_AUDIT.md) ("GPU and compositing").
The existing Metal-to-Vulkan layer is in `metal/`, `msl/` and
`quartzcore/` (post-refactor: `darwin/metal/`, `darwin/msl/`,
`darwin/quartzcore/`).

* **When:** after stage 2, which builds the windowed presentation path.
  Stage 3 (the JIT) helps, and is required for the SwiftShader fallback.
* **The GPU work is portable:** Vulkan is the host API for it on both
  Windows and Linux. Only surface creation goes through `host::gpu` and
  `host::ui`.

## Goal

Windowed Chrome without `--disable-gpu` renders a local page with GPU
rasterization and compositing:

* through Skia Graphite on Dawn's Metal backend, and ANGLE's Metal
  backend for WebGL;
* presented through `CAMetalLayer` / IOSurface-backed textures.

`chrome://gpu`-equivalent information, dumped from a local page through
`navigator.gpu` and WebGL queries, shows hardware acceleration. Chrome
exits with 0.

## What is known (from wave 1)

* **Dawn's Metal start-up already runs in-process.** Wave 1's headless
  runs got as far as `GetDeviceIORegistryPCIInfo failed: Failed to get the
  IO registry entry for the accelerator`
  (`dawn/native/metal/PhysicalDeviceMTL.mm:144`).
  * It is logged, not fatal.
  * Next: an IORegistry entry for the GPU with PCI vendor and device IDs
    taken from the Vulkan physical device.
* **Link coverage is close:** Metal is 26 of 30, IOSurface 37 of 38,
  QuartzCore 18 of 21.
  * Missing: `MTLCommonCounter*` timestamps.
  * Weak: `MTLCommandQueueDescriptor`, `MTLLogStateDescriptor`,
    `CAMetalDisplayLink` and the `CADynamicRange*` keys.
* **What it needs** *(design)*:
  * MSL generated at run time through `newLibraryWithSource:`. Alchemy has
    an MSL frontend, `msl/`, tested against Blender's corpus. Measure it
    against ANGLE and Dawn's generated MSL.
  * `MTLSharedEvent`, both across processes (GPU process to browser,
    when not `--in-process-gpu`) and inside one process.
  * `MTLTexture`s backed by IOSurfaces and shared with CoreAnimation, and
    `CAMetalLayer` drawables.
  * Argument buffers, indirect command buffers and heaps, as far as
    Graphite uses them.
* **A stepping stone:** SwiftShader Vulkan (`libvk_swiftshader`, shipped
  in the bundle) as the GPU process's backend. It needs the stage 3 JIT,
  and shows the Vulkan path without Metal translation.

## Groups (suggested)

* **g1 Metal core:** device and IORegistry identity, command queues and
  buffers, `MTLSharedEvent`, heaps, counters, and the remaining Metal
  names. Owns `darwin/metal/`.
* **g2 shaders:** the MSL ANGLE and Dawn generate, run through Alchemy's
  MSL frontend to SPIR-V.
  * Build a corpus from captured `newLibraryWithSource:` inputs, dumped
    at run time to `artifacts/`, never committed.
  * Fix the gaps it finds.
  * Owns `darwin/msl/`.
* **g3 surfaces:** IOSurface-backed textures, `CAMetalLayer` and its
  drawables, presentation timing, and cross-process IOSurface for a GPU
  process running as a child, using wave 1's xmem sections. Owns
  `darwin/quartzcore/` and IOSurface.
* **g4 ANGLE and WebGL:**
  * Run WebGL from a local page through ANGLE-Metal.
  * Do the WebGL conformance-style smoke from local pages, with no
    downloads.
  * Optionally, the SwiftShader fallback once stage 3 has landed.

## Done when

* The goal's page renders with GPU acceleration, confirmed in the cloaked
  capture and by the in-page GPU report.
* WebGL and WebGPU smoke pages pass.
* Chrome exits with 0 in 3 of 3 runs.
* The MSL corpus test covers Chrome's shaders, without the shaders being
  committed.
* The regression check before landing passes, including Blender's Metal
  viewport.

## Not in scope

* Video decode (VideoToolbox) and media.
