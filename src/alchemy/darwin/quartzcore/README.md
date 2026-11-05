# QuartzCore

**Covers:** CALayer, CAMetalLayer and presentation over Vulkan
(`quartzcore.cpp`, `presenter.cpp`), CALayer tree composition into windows
on CATransaction commit (`layer_tree.cpp`, `layer_contents.cpp`; the AppKit
contract is `include/alchemy/gui/layer_contents.hpp`), CoreVideo's
CVDisplayLink (`display_link.cpp`), CAMetalDisplayLink and the
CADynamicRange keys (`metal_display_link.cpp`: a CFRunLoopTimer at the
display's rate on the run loop the link was added to, handing the delegate
the layer's next drawable) and CADisplayLink (the same timer, sending the
target its selector; made by AppKit's -displayLinkWithTarget:selector:),
IOSurface (`iosurface.cpp`), CoreImage
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
uses the 2D (affine) part of each transform: rotated, sheared and masked
layers are drawn as groups (offscreen at the same resolution, the `mask`
layer's alpha applied, then resampled bilinearly into place), and scaled
contents follow `minificationFilter` / `magnificationFilter` (linear by
default, or nearest; trilinear minifies as linear). The 3D parts (perspective,
z) are not drawn yet; a transform that maps a layer to nothing, or a group
over 8192 pixels on a side, is reported once and not drawn. Changes inside a
mask layer show at the masked layer's next change. Filters (`filters`,
`compositingFilter`) and conic gradients are not drawn (conic reported once).

**Layer appearance and kinds (drawn):** background colours with corner
radii (per corner, `maskedCorners`), borders, shadows (opacity, offset,
radius as a box blur, `shadowPath`), rounded `masksToBounds` clipping and
`contentsRect`/gravity frames. Layers that draw get a backing store:
`-display` asks the delegate's `-displayLayer:`, else `-drawInContext:`
(the delegate's `-drawLayer:inContext:`) into a bitmap context at the
layer's scale. CAShapeLayer fills and strokes its path (with
`strokeStart`/`strokeEnd`), CATextLayer draws strings and attributed
strings through CoreText, CAGradientLayer draws axial and radial
gradients, CAReplicatorLayer draws its copies with the instance transform
and alpha offset (colour offsets and `instanceColor` are kept, not drawn). Hit testing, `containsPoint:`, the `convert*` methods,
CAScrollLayer scrolling, CAConstraint / CAConstraintLayoutManager layout,
`renderInContext:` and the CAMediaTiming time conversions are real.
CATiledLayer draws as one tile. CATransaction keeps +setValue:forKey: values
per transaction (nested transactions inherit them; the kCATransaction...
keys are its properties, the animation timing function among them).
`wantsExtendedDynamicRangeContent` is kept on every layer, but composition
is standard dynamic range: EDR contents show clamped. CAMetalLayer's
`preferredDevice` is the system default device (there is one GPU).

**Animations (presentation values):** while a layer has animations, the
composition shows its presentation values, redrawn each frame (60 Hz) until
the last animation ends. CABasicAnimations are interpolated at the current
media time: CAMediaTiming (begin time, time offset, speed, duration,
repeats, autoreverses, fill mode), the timing function's curve (linear when
nil), from / to / by values (a missing one is the current value), additive
and cumulative. Key paths: opacity, zPosition, cornerRadius, borderWidth,
the shadow's opacity, radius and offset, position, anchorPoint, bounds and
their components, contentsRect, transform and sublayerTransform (the 2D part
decomposed into translation, rotation the shorter way, scale and shear; the
rest linear) with transform.rotation(.z) / .scale(.x .y) / .translation(.x
.y), the background, border and shadow colours, and hidden; other key paths
are left at their values. `-presentationLayer` is a copy made by
`-initWithLayer:` (which copies the CALayer state) with those values;
`-modelLayer` answers the model. NSValue's `+valueWithCATransform3D:` /
`-CATransform3DValue` are provided. Keyframe, group, spring and transition
animations are timed (delegate messages, removal) but do not change what is
shown yet. Layer trees AppKit draws itself (no contents) do not show
presentation values.

**Springs, value functions, emitters:** CASpringAnimation keeps its mass,
stiffness, damping and initial velocity; `settlingDuration` is computed from
that spring's motion (until it stays within 0.1% of the end value), and the
perceptual duration and bounce (macOS 14) follow the usual conversions
(unverified). CAValueFunction is made by name. CAEmitterLayer and
CAEmitterCell keep their whole configuration, but composition emits no
particles (the emitter layer draws as a plain layer).

## Moved from darwin/identities/

* `shape_layer_shell.cpp`: the CAShapeLayer shell (the support kit).
* `base_layers.cpp`, `contents_formats.cpp`: headless, every build: CALayer and CAMetalLayer, kCAContentsFormatRGBA8Uint.
