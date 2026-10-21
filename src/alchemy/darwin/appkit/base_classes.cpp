#include "alchemy/darwin/headless_additions.hpp"
#include "alchemy/darwin/headless_provider.hpp"
#include "alchemy/darwin_frameworks.hpp"

// AppKit and Cocoa: the base classes (NSResponder, NSApplication, NSView,
// NSWindow, ...), NSApp (nil until +sharedApplication, which a headless
// process never completes) and the pasteboard and appearance names.
// Moved from darwin/identities/frameworks.cpp (the headless framework set);
// registered in both builds by install_darwin_frameworks.
namespace alchemy::darwin_headless {
namespace {

class BaseClasses final : public Provider {
public:
    using Provider::Provider;
    void install() {
        constexpr std::pair<std::string_view, std::string_view> classes[] = {
            {"NSResponder", "NSObject"}, {"NSApplication", "NSResponder"}, {"NSView", "NSResponder"},
            {"NSOpenGLView", "NSView"}, {"NSWindow", "NSResponder"}, {"NSImageRep", "NSObject"},
            {"NSBitmapImageRep", "NSImageRep"}, {"NSAlert", "NSObject"}, {"NSAppearance", "NSObject"},
            {"NSBezierPath", "NSObject"}, {"NSColor", "NSObject"}, {"NSColorSampler", "NSObject"},
            {"NSColorSpace", "NSObject"}, {"NSCursor", "NSObject"}, {"NSDocumentController", "NSObject"},
            {"NSEvent", "NSObject"}, {"NSImage", "NSObject"}, {"NSMenu", "NSObject"}, {"NSMenuItem", "NSObject"},
            {"NSOpenGLContext", "NSObject"}, {"NSOpenGLPixelFormat", "NSObject"}, {"NSPasteboard", "NSObject"},
            {"NSScreen", "NSObject"}, {"NSTextInputContext", "NSObject"}, {"NSWorkspace", "NSObject"}};
        for (const auto& [name, parent] : classes) {
            static_cast<void>(declare_class(appkit, name, parent));
            static_cast<void>(declare_class(cocoa, name, parent));
        }
        // NSApp is nil until +[NSApplication sharedApplication], which a
        // headless process never completes (no window server connection).
        // One cell: Cocoa re-exports AppKit's NSApp, so both names bind to the
        // same global (a GUI provider resolves it and stores the application).
        const auto application = context.allocate_guest(8);
        memory().write64(application, 0);
        for (const auto library : {appkit, cocoa}) {
            context.install_export(std::string(library), "_NSApp", {application, ExportKind::data});
            constexpr std::pair<std::string_view, std::string_view> strings[] = {
                {"_NSAppearanceNameVibrantDark", "NSAppearanceNameVibrantDark"},
                {"_NSAppearanceNameVibrantLight", "NSAppearanceNameVibrantLight"},
                {"_NSFilenamesPboardType", "NSFilenamesPboardType"},
                {"_NSPasteboardTypeFileURL", "public.file-url"}, {"_NSPasteboardTypePNG", "public.png"},
                {"_NSPasteboardTypeString", "public.utf8-plain-text"}, {"_NSPasteboardTypeTIFF", "public.tiff"},
                {"_NSPasteboardURLReadingContentsConformToTypesKey", "NSPasteboardURLReadingContentsConformToTypesKey"},
                {"_NSPasteboardURLReadingFileURLsOnlyKey", "NSPasteboardURLReadingFileURLsOnlyKey"},
                {"_NSTextInputContextKeyboardSelectionDidChangeNotification", "NSTextInputContextKeyboardSelectionDidChangeNotification"},
                {"_NSWindowWillCloseNotification", "NSWindowWillCloseNotification"}};
            for (const auto& [symbol, value] : strings) string_symbol(library, std::string(symbol), value);
        }
    }
};

} // namespace

void install_appkit_base_classes(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf) {
    std::make_shared<BaseClasses>(context, objc, cf)->install();
}

} // namespace alchemy::darwin_headless
