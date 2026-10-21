#include "alchemy/darwin/headless_additions.hpp"
#include "alchemy/darwin/headless_provider.hpp"

// AppKit class shells (real superclasses from the AppKit headers, parents
// first), and Cocoa's NSUnderlineColorAttributeName.
// Moved from darwin/identities/installer_frameworks.cpp (an installer
// binds them first); registered in both builds by install_darwin_frameworks.
namespace alchemy::darwin_headless {
namespace {

class ClassShells final : public Provider {
public:
    using Provider::Provider;
    void install() {
        const std::initializer_list<std::string_view> owners{appkit, cocoa};
        // Real superclasses (AppKit headers); parents first.
        constexpr std::pair<std::string_view, std::string_view> classes[] = {
            {"NSCell", "NSObject"}, {"NSActionCell", "NSCell"}, {"NSButtonCell", "NSActionCell"},
            {"NSControl", "NSView"}, {"NSButton", "NSControl"}, {"NSPopUpButton", "NSButton"},
            {"NSTextField", "NSControl"}, {"NSSecureTextField", "NSTextField"}, {"NSComboBox", "NSTextField"},
            {"NSImageView", "NSControl"}, {"NSSlider", "NSControl"}, {"NSStepper", "NSControl"},
            {"NSScroller", "NSControl"}, {"NSTableView", "NSControl"}, {"NSBox", "NSView"},
            {"NSProgressIndicator", "NSView"}, {"NSScrollView", "NSView"}, {"NSTabView", "NSView"},
            {"NSText", "NSView"}, {"NSTextView", "NSText"}, {"NSPanel", "NSWindow"}, {"NSSavePanel", "NSPanel"},
            {"NSOpenPanel", "NSSavePanel"}, {"NSColorPanel", "NSPanel"}, {"NSFontPanel", "NSPanel"},
            {"NSTabViewItem", "NSObject"}, {"NSTableColumn", "NSObject"}, {"NSToolbar", "NSObject"},
            {"NSToolbarItem", "NSObject"}, {"NSFont", "NSObject"}, {"NSFontManager", "NSObject"},
            {"NSGraphicsContext", "NSObject"}, {"NSLayoutManager", "NSObject"}, {"NSParagraphStyle", "NSObject"},
            {"NSMutableParagraphStyle", "NSParagraphStyle"}, {"NSPasteboardItem", "NSObject"},
            {"NSDraggingItem", "NSObject"}, {"NSPageLayout", "NSObject"}, {"NSPrintInfo", "NSObject"},
            {"NSPrintPanel", "NSObject"}, {"NSAnimation", "NSObject"}, {"NSViewAnimation", "NSAnimation"},
            {"NSGestureRecognizer", "NSObject"}, {"NSMagnificationGestureRecognizer", "NSGestureRecognizer"},
            {"NSPanGestureRecognizer", "NSGestureRecognizer"}, {"NSPressGestureRecognizer", "NSGestureRecognizer"},
            {"NSRotationGestureRecognizer", "NSGestureRecognizer"}};
        for (const auto& [name, parent] : classes) static_cast<void>(class_shell(owners, name, parent));

        // AppKit's constants and NSAccessibilityUnignoredDescendant are AppKit's
        // and CoreText's headless shells (darwin/shells.hpp); no GUI provider
        // exports Cocoa's NSUnderlineColorAttributeName.
        string_symbol(cocoa, "_NSUnderlineColorAttributeName", "NSUnderlineColor");
    }
};

} // namespace

void install_appkit_class_shells(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf) {
    std::make_shared<ClassShells>(context, objc, cf)->install();
}

} // namespace alchemy::darwin_headless
