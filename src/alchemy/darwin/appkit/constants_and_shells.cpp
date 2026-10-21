#include "alchemy/darwin/kit/support/support_kit.hpp"

// AppKit constants with their real values, NSBitsPerPixelFromDepth, and the
// NSStatusBar and NSStackView class shells (real superclass, no methods of
// their own: a message they do not inherit fails as an unsupported message).
// Qt 6.5's bind-time surface first needed them (moved from
// darwin/identities/qt_imports.cpp); installed by the support kit
// (kit/support/install.cpp) at the old position, only when missing.
namespace alchemy::gui::support_kit {

namespace {

constexpr std::string_view appkit = "/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit";

} // namespace

void install_appkit_constants_and_shells(Support& support) {
    // ---- AppKit constants (real values) ----
    for (const auto& [symbol, value] : std::initializer_list<std::pair<const char*, const char*>>{
             {"_NSColorPanelColorDidChangeNotification", "NSColorPanelColorDidChangeNotification"},
             {"_NSDocumentTypeDocumentAttribute", "DocumentType"},
             {"_NSFileContentsPboardType", "NXFileContentsPboardType"},
             {"_NSFontSetChangedNotification", "NSFontSetChangedNotification"},
             {"_NSHTMLTextDocumentType", "NSHTML"},
             {"_NSRTFTextDocumentType", "NSRTF"},
             {"_NSMenuDidBeginTrackingNotification", "NSMenuDidBeginTrackingNotification"},
             {"_NSPasteboardTypeColor", "com.apple.cocoa.pasteboard.color"},
             {"_NSPasteboardTypeFont", "com.apple.cocoa.pasteboard.character-formatting"},
             {"_NSPasteboardTypeHTML", "public.html"},
             {"_NSPasteboardTypeMultipleTextSelection", "com.apple.cocoa.pasteboard.multiple-text-selection"},
             {"_NSPasteboardTypePDF", "com.adobe.pdf"},
             {"_NSPasteboardTypeRTF", "public.rtf"},
             {"_NSPasteboardTypeRTFD", "com.apple.flat-rtfd"},
             {"_NSPasteboardTypeRuler", "com.apple.cocoa.pasteboard.paragraph-formatting"},
             {"_NSPasteboardTypeTabularText", "public.utf8-tab-separated-values-text"},
             {"_NSSystemColorsDidChangeNotification", "NSSystemColorsDidChangeNotification"},
             {"_NSWorkspaceDidActivateApplicationNotification", "NSWorkspaceDidActivateApplicationNotification"}})
        if (!support.process.resolve(appkit, symbol)) support.string_constant(appkit, symbol, value);
    // NSBitsPerPixelFromDepth (real): NSWindowDepth = bits per sample | (color space << 8).
    support.fill_function(appkit, "_NSBitsPerPixelFromDepth", [](CpuState& c, GuestMemory&) {
        const auto depth = static_cast<std::uint32_t>(c.x[0]);
        switch (depth) {
        case 0x208: c.x[0] = 24; break;    // NSWindowDepthTwentyfourBitRGB
        case 0x210: c.x[0] = 64; break;    // NSWindowDepthSixtyfourBitRGB
        case 0x220: c.x[0] = 128; break;   // NSWindowDepthOnehundredtwentyeightBitRGB
        default: c.x[0] = (depth & 0xff) * ((depth >> 8) == 2 ? 3 : 1);
        }
    });

    const auto shell = [&](std::string_view library, const char* name, const char* superclass) {
        if (support.process.resolve(library, "_OBJC_CLASS_$_" + std::string(name))) return;
        if (!support.objc.find_class(superclass)) return;
        static_cast<void>(support.declare_class(name, superclass, library));
    };
    shell(appkit, "NSStatusBar", "NSObject");
    shell(appkit, "NSStackView", "NSView");   // linked by a renderer plug-in's scatter library
}

} // namespace alchemy::gui::support_kit
