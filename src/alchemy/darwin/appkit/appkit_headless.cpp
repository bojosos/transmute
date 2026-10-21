#include "alchemy/darwin/shell_kit.hpp"
#include "alchemy/darwin/shells.hpp"

// alchemy.exe only (darwin/shells.hpp): AppKit's headless answers, from the
// framework identities (darwin/frameworks.cpp, darwin/headless_additions.cpp).
// A headless process has no window server, keyboard, pasteboard server or
// graphics context.
namespace alchemy::darwin_shells {
namespace {
constexpr std::string_view appkit = "/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit";
constexpr std::string_view cocoa = "/System/Library/Frameworks/Cocoa.framework/Versions/A/Cocoa";
constexpr std::string_view core_graphics = "/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics";
constexpr std::string_view carbon = "/System/Library/Frameworks/Carbon.framework/Versions/A/Carbon";
constexpr std::string_view core_services = "/System/Library/Frameworks/CoreServices.framework/Versions/A/CoreServices";
constexpr std::string_view application_services =
    "/System/Library/Frameworks/ApplicationServices.framework/Versions/A/ApplicationServices";

// MacErrors.h / CGError.h
constexpr std::int32_t param_err = -50;
constexpr std::int32_t cg_illegal_argument = 1001, cg_none_available = 1011;

std::uint64_t argument(const CpuState& state, const GuestMemory& memory, unsigned index) {
    return index < 8 ? state.x[index] : memory.read64(state.sp + (index - 8) * 8ULL);
}
} // namespace

void install_appkit_shell(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf) {
    ShellKit kit{context, objc, cf};
    for (const auto library : {appkit, cocoa}) {
        kit.string_symbol(library, "_NSDeviceRGBColorSpace", "NSDeviceRGBColorSpace");
        kit.string_symbol(library, "_NSDeviceWhiteColorSpace", "NSDeviceWhiteColorSpace");
    }

    // No display (the display queries are CoreGraphics' shell): the cursor
    // cannot be moved or decoupled.
    kit.function(core_graphics, "_CGDisplayMoveCursorToPoint", [](CpuState& s, GuestMemory&) { s.x[0] = status(cg_illegal_argument); });
    kit.function(core_graphics, "_CGAssociateMouseAndMouseCursorPosition", [](CpuState& s, GuestMemory&) { s.x[0] = status(cg_none_available); });

    // Carbon (HIToolbox): no keyboard input source or event state.
    kit.function(carbon, "_GetCurrentEventButtonState", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    kit.function(carbon, "_LMGetKbdType", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    for (const auto* name : {"_TISCopyCurrentKeyboardInputSource", "_TISCopyCurrentKeyboardLayoutInputSource",
             "_TISGetInputSourceProperty"})
        kit.function(carbon, name, [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    // Layout data only comes from an input source, which never exists, so
    // no 'uchr' table is interpreted: report paramErr with no output.
    kit.function(core_services, "_UCKeyTranslate", [](CpuState& s, GuestMemory& m) {
        if (const auto actual = argument(s, m, 8)) m.write64(actual, 0);
        s.x[0] = status(param_err);
    });
    kit.function(carbon, "_GetCurrentKeyModifiers", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });

    // Explicit failures.
    kit.unsupported(application_services, {"_HIShapeContainsPoint", "_HIShapeCreateIntersection", "_HIShapeCreateMutable",
        "_HIShapeCreateMutableCopy", "_HIShapeCreateUnion", "_HIShapeCreateWithRect", "_HIShapeDifference",
        "_HIShapeGetBounds", "_HIShapeIntersect", "_HIShapeIsEmpty", "_HIShapeIsRectangular", "_HIShapeOffset",
        "_HIShapeReplacePathInCGContext", "_HIShapeUnion", "_HIShapeUnionWithRect"}, "HIShape regions are not implemented");
    kit.unsupported(application_services, {"_PasteboardCreate", "_PasteboardSetPasteLocation", "_PasteboardSynchronize"},
        "the Carbon pasteboard is not implemented");
    kit.unsupported(carbon, {"_GetThemeMetric", "_HIThemeDrawButton", "_HIThemeDrawFocusRect", "_HIThemeDrawFrame",
        "_HIThemeDrawPaneSplitter"}, "HITheme drawing is not implemented");
    kit.unsupported(appkit, {"_NSRectFill", "_NSFrameRectWithWidthUsingOperation"}, "AppKit drawing needs a graphics context");

    // AppKit constants (the class shells stay with the identities).
    for (const auto& [symbol, value] : std::initializer_list<std::pair<std::string_view, std::string_view>>{
             {"_NSCalibratedWhiteColorSpace", "NSCalibratedWhiteColorSpace"}, {"_NSPatternColorSpace", "NSPatternColorSpace"},
             {"_NSGeneralPboard", "Apple CFPasteboard general"},
             {"_NSToolbarFlexibleSpaceItemIdentifier", "NSToolbarFlexibleSpaceItem"},
             {"_NSToolbarSpaceItemIdentifier", "NSToolbarSpaceItem"},
             {"_NSViewAnimationEffectKey", "NSViewAnimationEffectKey"}, {"_NSViewAnimationEndFrameKey", "NSViewAnimationEndFrameKey"},
             {"_NSViewAnimationFadeInEffect", "NSViewAnimationFadeInEffect"},
             {"_NSViewAnimationFadeOutEffect", "NSViewAnimationFadeOutEffect"},
             {"_NSViewAnimationStartFrameKey", "NSViewAnimationStartFrameKey"},
             {"_NSViewAnimationTargetKey", "NSViewAnimationTargetKey"}}) {
        for (const auto library : {appkit, cocoa}) kit.string_symbol(library, std::string(symbol), value);
    }
    // An element without an accessibility parent chain is its own
    // unignored descendant.
    for (const auto library : {appkit, cocoa})
        kit.function(library, "_NSAccessibilityUnignoredDescendant", [](CpuState&, GuestMemory&) {});
}

} // namespace alchemy::darwin_shells
