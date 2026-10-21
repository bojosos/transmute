#pragma once

// The headless shells of the AppKit, CoreGraphics, CoreText, Metal, IOKit and
// media frameworks (darwin/shell_kit.hpp): in alchemy.exe each registers that
// framework's headless answers for names its GUI provider owns in
// alchemy-fx; in alchemy-fx each is empty (<framework>_fx.cpp).
// install_darwin_frameworks calls them after the framework identities.

namespace alchemy {
class CoreFoundation;
class ObjcRuntime;
class ProcessContext;
} // namespace alchemy

namespace alchemy::darwin_shells {

// NSDevice*ColorSpace names, the CoreGraphics cursor functions, Carbon input
// state, UCKeyTranslate, HIShape, the Carbon
// pasteboard, HITheme drawing, NSRectFill, and AppKit's toolbar, pasteboard
// and view-animation strings. alchemy-fx: darwin/appkit/{carbon,hitoolbox,
// pasteboard,panels,graphics}.cpp and darwin/coregraphics/appkit_drawing.cpp.
void install_appkit_shell(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf);
// CGColorSpace and CGColor values, geometry, the display queries (no
// display), and the drawing and ImageIO entry points that fail explicitly.
// alchemy-fx: darwin/coregraphics/ (displays.cpp for the displays).
void install_coregraphics_shell(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf);
// CFAttributedStringCreate, the CoreText attribute keys and the text layout
// entry points that fail explicitly, and the AppKit text attribute keys.
// alchemy-fx: darwin/coretext/.
void install_coretext_shell(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf);
// No Metal device. alchemy-fx: darwin/metal/metal_objects.cpp.
void install_metal_shell(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf);
// IOKit lookups in an empty registry. alchemy-fx: darwin/iokit/iokit_registry.cpp.
void install_iokit_shell(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf);
// No CAClock. alchemy-fx: darwin/media/caclock.cpp.
void install_media_shell(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf);

} // namespace alchemy::darwin_shells
