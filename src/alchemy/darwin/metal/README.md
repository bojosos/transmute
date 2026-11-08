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
`residency.cpp`: MTLResidencySet is real bookkeeping (Vulkan keeps every
allocation resident, so what a set asks for always holds); the MTLIO
factories answer nil with an MTLIOErrorDomain error. Metal's NSString
constants (counter names, error domains, MTLCommandBufferEncoderInfoErrorKey)
are in `metal_objects.cpp` (install_counter_names).
`gpu_identity.cpp` lists the host's GPUs (Vulkan physical devices) with the
name and registry ID their MTLDevice reports, for IOKit's GPU registry
entries. Shaders
compile through `darwin/msl/`. Without Vulkan headers at build time only
`metal_unavailable.cpp` is built.

**Headless:** `metal_headless.cpp` (alchemy.exe only, no device), empty
`metal_fx.cpp` in alchemy-fx (`darwin/shells.hpp`).

**Host dependencies:** `host::gpu` (the Vulkan loader and window-surface
extension; vulkan-1.dll on win32), `host::ui` (monitor identities).

**Argument buffers, tier 2** (`bindless.cpp`): one device-wide descriptor
set (arrays of sampled images, storage images and samplers; update after
bind) holds every texture and sampler a client asked an MTLResourceID for;
gpuResourceID and MTLArgumentEncoder setTexture(s):/setSamplerState(s):
write that slot, which shaders index (msl README). argumentBuffersSupport is
Tier2 only on devices with descriptor indexing and update-after-bind.
Pipelines whose shaders read the set get it at set 2 of their layout.
Pipelines, ICBs, acceleration structures and function tables in argument
buffers stop explicitly; useResource:/useHeap: need nothing (textures stay
in GENERAL).

**Pipeline creation:** on Intel's Windows driver, shader module and pipeline
creation (and the pipeline cache's data reads) run one at a time
(Device::pipeline_creation_lock): its compute pipeline compiles fault when
several run at once and miss the driver's shader cache, as Blender's
parallel shader compiles do on a first run. ALCHEMY_METAL_SERIAL_PIPELINES=1
or 0 forces it on or off; ALCHEMY_METAL_SPIRV_SALT=<text> (diagnostics) makes
every module new to the driver's cache, as on a first run.

**Command buffer times:** kernelStartTime/kernelEndTime are the host's submit
and observed completion; GPUStartTime/GPUEndTime come from two timestamp
queries per command buffer, recorded once the process first asked for GPU
times, mapped to the host clock through the calibrated device clock
(`commands.hpp`, CommandBuffer::times).

**Binary archives:** what an archive can hold for VkPipelines is the device's
VkPipelineCache. Adding a compute or render pipeline creates it;
serializeToURL: writes the cache behind an Alchemy header; an archive made
from such a file merges it back, one from anywhere else (macOS, another GPU)
starts empty; a missing file fails with MTLBinaryArchiveErrorInvalidFile.
Tile and mesh pipelines and functions fail with CompilationFailure.

**Multisample depth/stencil resolves:** depthResolveFilter Min/Max use the
device's MIN/MAX resolve modes and stop explicitly without them;
stencilResolveFilter DepthResolvedSample is sample 0 with a Sample0 depth
resolve and stops explicitly with Min/Max. setPurgeableState: answers the
previous state; contents are never discarded (Volatile and Empty allow that).

**Unsupported (explicit failures):** for example MTKTextureLoader without a
device; precompiled libraries (`newLibraryWithData:`, `newLibraryWithURL:`/
`newLibraryWithFile:`, `newDefaultLibrary`/`newDefaultLibraryWithBundle:`'s
default.metallib) without the AIR translator DLL (with it they are translated;
a missing file is MTLLibraryErrorFileNotFound, no default.metallib nil);
IOSurface-backed textures other than plane 0 of a chunky surface
as a 2D single-level, single-sample texture of the surface's texel size
(nil); a host signal of a cross-process shared event past a GPU signal of
it still pending in the receiving process (Vulkan forbids it; one process
signalling and the other waiting never does it). GPU counters: only the
timestamp counter set, sampled at stage boundaries (encoder starts and ends;
a render pass's vertex and fragment boundaries are its start and end) and at
draw, dispatch and blit boundaries (sampleCountersInBuffer:atSampleIndex:
withBarrier:; inside a render pass the pass is suspended around the sample);
tile dispatch boundaries are not supported. MTLLogState never receives
shader logs (shader logging is not translated).

**Late store actions, step rates, asynchronous pipelines:** a multisample
resolve asked for while encoding (setColor/Depth/StencilStoreAction: on an
encoder made with MTLStoreActionUnknown) runs when the pass ends, as a second
rendering instance that loads the attachments and resolves them; store action
options other than None stop explicitly (no programmable sample positions).
MTLVertexStepFunctionPerInstance with a stepRate above 1 is a vertex
attribute divisor (VK_KHR/EXT_vertex_attribute_divisor) and stops explicitly
without one; stepRate 0 always stops. The ...options:completionHandler:
pipeline forms run their synchronous forms and call the handler before
returning; tile and mesh render pipelines answer nil with an error.

**Heaps, buffer-backed textures, dynamic strides:** MTLHeapTypePlacement
heaps own one allocation (PlacementMemory) and bind buffers and textures at
the client's offsets, aliasing as the client arranges; heapBuffer/
TextureSizeAndAlign answer the Vulkan requirements (bufferImageGranularity
included). Sparse heaps stop explicitly. newTextureWithDescriptor:offset:
bytesPerRow: with MTLTextureType2D is a linear image bound to the buffer's
memory: bytesPerRow must be the driver's row pitch (the row bytes rounded up
to minimumLinearTextureAlignmentForPixelFormat:) and the offset must meet the
driver's linear offset alignment (buffers of 64 KiB and more start on it);
otherwise it stops explicitly. Texture buffers have views of another format
of the same texel size (no swizzle) and texture-to-texture copies.
MTLBufferLayoutStrideDynamic layouts take the ...attributeStride: binds'
strides (VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE). Vector function
constants: msl/README.md.

**Translated (AIR) pipelines, indirect:** indirect draws, indexed indirect
draws and indirect dispatches of metallib pipelines run from the arguments
buffer (the translator's DrawIndirect/DrawIndexedIndirect and indirect
regions). The translator's indirect-clamp prepass is not recorded: group
counts above maxComputeWorkGroupCount are not clamped, as for native
pipelines. An indexed indirect draw that needs its arguments on the CPU, and
replayed dispatches, stop explicitly.

## Moved from darwin/identities/

* `stage_input_output_shell.cpp`: the MTLStageInputOutputDescriptor shell (the support kit).
* `base_classes.cpp`: headless, every build: the descriptor and acceleration-structure classes.
