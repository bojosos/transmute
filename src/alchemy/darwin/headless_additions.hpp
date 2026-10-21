#pragma once

// The frameworks' headless additions: answers every build registers when the
// process starts, in this order, by install_darwin_frameworks
// (darwin/frameworks.cpp), after the framework set. A GUI
// provider of alchemy-fx may register over any of them. Each piece is in its
// framework's directory (helpers: darwin/headless_provider.hpp). CoreGraphics
// drawing, CoreText layout and ImageIO codecs fail in their frameworks'
// headless shells (darwin/shells.hpp), as do HIShape, the Carbon pasteboard,
// HITheme drawing and NSRectFill.

namespace alchemy {
class CoreFoundation;
class ObjcRuntime;
class ProcessContext;
} // namespace alchemy

namespace alchemy::darwin_headless {

// The headless framework set (the former darwin/identities/frameworks.cpp),
// installed first, in this order:
// AppKit and Cocoa: the base classes (NSResponder, NSApplication, NSView,
void install_appkit_base_classes(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf);   // appkit/base_classes.cpp
// Metal: the descriptor and acceleration-structure classes, so programs that
void install_metal_base_classes(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf);   // metal/base_classes.cpp
// QuartzCore: CALayer and CAMetalLayer, the classes the headless Metal set
void install_quartzcore_base_layers(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf);   // quartzcore/base_layers.cpp
// OpenAL 1.1 with no output or capture device. No device or context can be
void install_openal(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf);   // openal/install.cpp
// CoreGraphics (headless-correct): zero online displays, so per-display
void install_coregraphics_no_displays(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf);   // coregraphics/no_displays.cpp
// ColorSync: no display device is registered; the device-info keys are
void install_colorsync(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf);   // colorsync/install.cpp
// CoreVideo: pixel buffers here are IOSurface-backed GPU interop surfaces,
void install_media_pixel_buffers(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf);   // media/pixel_buffers.cpp
// OpenGL (CGL): no CGL context can be created.
void install_opengl_cgl(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf);   // opengl/cgl.cpp
// IOKit with an empty registry: matching dictionaries are real, and lookups
void install_iokit_empty_registry(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf);   // iokit/empty_registry.cpp
// Carbon (HIToolbox) Text Input Sources: the property keys (no keyboard
void install_appkit_input_sources(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf);   // appkit/input_sources.cpp
// CoreServices LSSharedFileList: a headless process has no Finder sidebar,
void install_coreservices_shared_file_lists(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf);   // coreservices/shared_file_lists.cpp
// CoreServices CarbonCore fp.h exports: the libSystem implementations, or
void install_coreservices_carbon_core_math(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf);   // coreservices/carbon_core_math.cpp
// AudioUnit / AudioToolbox / CoreAudio: no audio component is registered, so
void install_media_audio_components(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf);   // media/audio_components.cpp
// libiconv (the GNU interface as shipped in /usr/lib/libiconv.2.dylib):
void install_libiconv(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf);   // libiconv/install.cpp

// Then the additions, in this order:
// CoreFoundation (headless-correct): the system encoding and the encodings
void install_corefoundation_strings_and_bundles(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf);   // corefoundation/strings_and_bundles.cpp
// AppKit (headless-correct): +[NSColor setIgnoresAlpha:] and +ignoresAlpha, a
void install_appkit_color_alpha(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf);   // appkit/color_alpha.cpp
// Foundation: the NSUserDefaults class (its methods are alchemy.exe's headless
void install_foundation_class_shells_and_events(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf);   // foundation/class_shells_and_events.cpp
// libSystem (headless-correct): _NSGetArgc/_NSGetArgv over the startup vector,
void install_libsystem_crt_and_host_info(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf);   // libsystem/misc/crt_and_host_info.cpp
// ApplicationServices (HIServices) Process Manager (headless-correct):
void install_applicationservices_process_manager(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf);   // applicationservices/process_manager.cpp
// Foundation NSGeometry (headless-correct): NSIsEmptyRect, NSEqualSizes,
void install_foundation_geometry(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf);   // foundation/geometry.cpp
// CoreGraphics (headless-correct): CGEventSourceKeyState (no keyboard).
void install_coregraphics_event_source(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf);   // coregraphics/event_source.cpp
// Carbon (HIToolbox): no Carbon event loop and no global hot keys. Handlers
void install_appkit_carbon_events(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf);   // appkit/carbon_events.cpp
// PrintCore (through ApplicationServices): no printers. The server's printer
void install_printing_no_printers(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf);   // printing/no_printers.cpp
// CoreServices (headless-correct): the UTType constants and table
void install_coreservices_types_and_gestalt(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf);   // coreservices/types_and_gestalt.cpp
// QuartzCore: kCAContentsFormatRGBA8Uint.
void install_quartzcore_contents_formats(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf);   // quartzcore/contents_formats.cpp
// AppKit class shells (real superclasses from the AppKit headers, parents
void install_appkit_class_shells(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf);   // appkit/class_shells.cpp

// All of them, in order, with the r2 headless shells they used to install in
// between (authorization, CFDateFormatter, CFSocket).
void install_additions(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf);

} // namespace alchemy::darwin_headless
