# Metal

**Covers:** Metal on Vulkan: devices, resources, pipelines, command buffers
and the CAMetalLayer bridge (`metal.cpp`, `device.cpp`, `resources.cpp`,
`pipeline.cpp`, `commands.cpp`, `quartzcore_link.cpp`), IOSurface-backed
textures (`surface_textures.cpp`: the texture's image and the surface's
pages are copied at command-buffer boundaries and event signals, straight
into the pages with `VK_EXT_external_memory_host`, else staged at commit
and completion; GPU writes synchronize
with IOSurfaceLock and the compositor in every process), cross-process
MTLSharedEvent handles (`shared_events.cpp`: a named-section value per
event, mirrored to and from each process's timeline semaphore within about
1 ms; MTLSharedEventHandle is NSSecureCoding), MetalKit
(`metalkit.cpp`) and Metal device discovery (`metal_display.cpp`).
`gpu_identity.cpp` lists the host's GPUs (Vulkan physical devices) with the
name and registry ID their MTLDevice reports, for IOKit's GPU registry
entries. Shaders
compile through `darwin/msl/`. Without Vulkan headers at build time only
`metal_unavailable.cpp` is built.

**Headless:** `metal_headless.cpp` (alchemy.exe only, no device), empty
`metal_fx.cpp` in alchemy-fx (`darwin/shells.hpp`).

**Host dependencies:** `host::gpu` (the Vulkan loader and window-surface
extension; vulkan-1.dll on win32), `host::ui` (monitor identities).

**Unsupported (explicit failures):** for example MTKTextureLoader without a
device; precompiled libraries (`newLibraryWithData:`, metallib files: Apple
GPU code); IOSurface-backed textures other than plane 0 of a chunky surface
as a 2D single-level, single-sample texture of the surface's texel size
(nil); a host signal of a cross-process shared event past a GPU signal of
it still pending in the receiving process (Vulkan forbids it; one process
signalling and the other waiting never does it). GPU counters: only the
timestamp counter set, sampled at stage boundaries (encoder starts and ends;
a render pass's vertex and fragment boundaries are its start and end):
sampleCountersInBuffer: at draw, dispatch or blit boundaries stops
explicitly. MTLLogState never receives shader logs (shader logging is
not translated).

## Moved from darwin/identities/

* `stage_input_output_shell.cpp`: the MTLStageInputOutputDescriptor shell (the support kit).
* `base_classes.cpp`: headless, every build: the descriptor and acceleration-structure classes.
