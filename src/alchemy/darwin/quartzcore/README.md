# QuartzCore

**Covers:** CALayer, CAMetalLayer and presentation over Vulkan
(`quartzcore.cpp`, `presenter.cpp`), CALayer tree composition into windows
on CATransaction commit (`layer_tree.cpp`, `layer_contents.cpp`; the AppKit
contract is `include/alchemy/gui/layer_contents.hpp`), CoreVideo's
CVDisplayLink (`display_link.cpp`), CAMetalDisplayLink and the
CADynamicRange keys (`metal_display_link.cpp`: a CFRunLoopTimer at the
display's rate on the run loop the link was added to, handing the delegate
the layer's next drawable), IOSurface (`iosurface.cpp`), CoreImage
(`core_image.cpp`, `ci_engine.cpp`), GLKit's math (`glkit.cpp`), and QLab's
CAAnimation, CATransform3D and CAMediaTimingFunction providers with their
value math (`graphics_math.cpp`) and property bags (`graphics_support.cpp`).

**IOSurface across processes:** a surface's Mach port
(IOSurfaceCreateMachPort) travels to other processes of the Alchemy process
tree as a foreign kernel object (`include/alchemy/mach_kernel_objects.hpp`,
the Mach provider's 'K' form): its pixel pages (vm_allocate'd, fx shared
memory) are shared, not copied, and IOSurfaceLookupFromMachPort there answers
a local surface over the same pages. IOSurfaceIDs are unique across the tree
(a counter in a named host section) and the same in every process. The use
count (IOSurfaceIncrementUseCount / DecrementUseCount / GetUseCount /
IsInUse) is shared by the tree too (a small named section per surface), and
the compositor holds a use while a surface is a layer's contents, like the
window server: a producer that checks IOSurfaceIsInUse (Chrome's GPU process)
does not draw into the surface the browser shows. The seed is shared the
same way; attached values (IOSurfaceSetValue) are per process.

**IOSurface and the GPU:** IOSurface-backed MTLTextures (Metal's
`surface_textures.cpp`, through `surface_pages` in `quartzcore_vulkan.hpp`)
render into and sample from the surface's pages. As on macOS, IOSurfaceLock
(without kIOSurfaceLockAvoidSync) and the compositor wait for the GPU writes
submitted so far, in every process of the tree (counters in the surface's
shared section, bounded at 2 s), and a completed GPU write advances the seed.
The pages outlive the surface object while GPU work may use them.
IOSurfaceSetPurgeable keeps the state per surface; pages are never purged.

**Host dependencies:** Vulkan through `host::gpu` (the loader and window
surfaces; vulkan-1.dll on win32), `host::ui` (window client sizes, the frame
timer), `host::sections` (the IOSurfaceID counter), the window server for
presentation.

**Unsupported (explicit failures):** for example planar IOSurfaces,
CoreImage kernels, and some CIImage source pixel formats. Layer composition
draws axis-aligned transforms only (rotating or shearing layers are skipped
and reported once), with nearest-neighbour sampling; masks, corner radii,
borders, shadows and filters are not drawn.

## Moved from darwin/identities/

* `shape_layer_shell.cpp`: the CAShapeLayer shell (the support kit).
* `base_layers.cpp`, `contents_formats.cpp`: headless, every build: CALayer and CAMetalLayer, kCAContentsFormatRGBA8Uint.
