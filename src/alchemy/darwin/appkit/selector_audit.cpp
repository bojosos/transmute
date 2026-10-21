#include "alchemy/darwin/kit/support/support_kit.hpp"

#include <fstream>
#include <sstream>

// ALCHEMY_SELECTOR_AUDIT=<file>: a diagnostic for bring-up. The file holds
// whitespace-separated selector names (for example the objc_msgSend$<sel>
// stubs of an image). At install time every selector that no instance or
// class of the host AppKit / Foundation / QuartzCore classes below answers is
// printed as "[alchemy support] audit: no host class answers <sel>". Selectors a
// guest class implements itself appear too; the list is for review only.
namespace alchemy::gui::support_kit {

void run_selector_audit(Support& support) {
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, "ALCHEMY_SELECTOR_AUDIT") != 0 || value == nullptr) return;
    const std::string path(value);
    std::free(value);
    std::ifstream in(path);
    if (!in) { diagnostic("audit: cannot read " + path); return; }
    std::vector<Address> probes, classes;
    for (const auto* name : {"NSObject", "NSResponder", "NSView", "NSWindow", "NSPanel", "NSApplication", "NSEvent", "NSScreen",
                             "NSMenu", "NSMenuItem", "NSCursor", "NSPasteboard", "NSPasteboardItem", "NSColor", "NSColorSpace",
                             "NSFont", "NSFontDescriptor", "NSFontManager", "NSImage", "NSImageRep", "NSBitmapImageRep",
                             "NSGraphicsContext", "NSString", "NSArray", "NSMutableArray", "NSDictionary", "NSMutableDictionary",
                             "NSSet", "NSNumber", "NSData", "NSURL", "NSBundle", "NSNotificationCenter", "NSNotification",
                             "NSTrackingArea", "NSTextInputContext", "NSWorkspace", "NSRunningApplication", "NSProcessInfo",
                             "NSUserDefaults", "NSAppearance", "NSDate", "NSTimer", "NSRunLoop", "NSThread", "NSValue",
                             "NSAttributedString", "NSOpenPanel", "NSSavePanel", "NSAlert", "CALayer", "CAMetalLayer",
                             "NSButton", "NSTextField", "NSPopUpButton", "NSControl", "NSCell", "NSColorPanel", "NSFontPanel",
                             "NSOpenGLContext", "NSOpenGLPixelFormat", "NSDraggingItem", "NSTextView", "NSAppleEventManager",
                             "NSOperationQueue", "NSAutoreleasePool", "NSFileManager", "NSLocale", "NSTimeZone", "NSUUID",
                             // wxWidgets (an installer wizard)
                             "NSScrollView", "NSClipView", "NSScroller", "NSProgressIndicator", "NSLayoutManager",
                             "NSTextContainer", "NSTextStorage", "NSText", "NSBox", "NSTabView", "NSTabViewItem", "NSTableView",
                             "NSTableColumn", "NSImageView", "NSSlider", "NSStepper", "NSComboBox", "NSSecureTextField",
                             "NSToolbar", "NSToolbarItem", "NSAnimation", "NSDockTile", "NSParagraphStyle",
                             "NSMutableParagraphStyle", "NSCalendar", "NSPrintInfo", "NSMutableAttributedString",
                             "NSTextFieldCell", "NSButtonCell", "NSFormatter", "NSNumberFormatter", "NSIndexSet",
                             "NSAffineTransform", "NSCharacterSet", "NSFontDescriptor", "NSSearchField", "NSMutableString"}) {
        const auto cls = support.objc.find_class(name);
        if (!cls) continue;
        classes.push_back(cls);
        probes.push_back(support.objc.allocate(cls));
    }
    std::string selector;
    std::size_t missing = 0;
    while (in >> selector) {
        const auto sel = support.objc.selector(selector);
        bool answered = false;
        for (std::size_t index = 0; index < probes.size() && !answered; ++index)
            answered = support.objc.responds(probes[index], sel) || support.objc.responds(classes[index], sel);
        if (!answered) { diagnostic("audit: no host class answers " + selector); ++missing; }
    }
    diagnostic("audit: " + std::to_string(missing) + " selectors without a host implementation");
    for (const auto probe : probes) support.process.objects().release(probe);
}

} // namespace alchemy::gui::support_kit
