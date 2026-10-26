// AppKit public-API coverage tests (the frameworks wave): every provider
// installed as run_program does (alchemy/darwin_providers.hpp) over the fake
// window server, and the methods added for the selectors real applications
// send: responders, cells and controls, views, windows, menus, images,
// alerts and panels, pasteboards, cursors, the text view's actions, and the
// container views and controllers. Native classes stand in for guest
// subclasses, so calls go through ObjcRuntime::send.
#include "alchemy/darwin/appkit/appkit_internal.hpp"
#include "alchemy/darwin/appkit/tests/fake_window_server.hpp"
#include "alchemy/darwin/coregraphics/quartz2d.hpp"
#include "alchemy/gui/tests/fill_fixture.hpp"

#include <bit>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {
namespace gui = alchemy::gui;
namespace appkit = alchemy::gui::appkit;
using Address = std::uint64_t;
using alchemy::CallRegs;
using alchemy::CallResult;
using transmute::CpuState;
using transmute::GuestMemory;
using fill_test::require;

std::vector<std::string> journal;
bool logged(const std::string& entry) { return std::find(journal.begin(), journal.end(), entry) != journal.end(); }
bool near(double a, double b) { return std::abs(a - b) < 1e-6; }

// The fake window server is AppKit's before any provider installs.
struct ServerFirst {
    gui::testing::FakeWindowServer server;
    ServerFirst() {
        appkit::set_window_server_override(&server);
        gui::quartz2d::set_screens_override([this] { return server.screens(); });
    }
    ~ServerFirst() {
        appkit::set_window_server_override(nullptr);
        gui::quartz2d::set_screens_override({});
        appkit::set_alert_presenter_override({});
    }
};

struct Fixture : ServerFirst, fill_test::Fixture {
    Address app{};
    Fixture() { app = id(cls("NSApplication"), "sharedApplication"); }
    CallResult msg(Address receiver, std::string_view selector, CallRegs registers = {}) {
        return objc.send(receiver, objc.selector(selector), std::move(registers));
    }
    Address id(Address receiver, std::string_view selector, std::initializer_list<Address> arguments = {}) {
        CallRegs registers;
        unsigned index = 2;
        for (const auto value : arguments) registers.x[index++] = value;
        return msg(receiver, selector, registers).x0;
    }
    Address str(std::string_view text) { return cf.string(text); }
    Address array(const std::vector<Address>& values) { return cf.array(values); }
    std::string text(Address object) { return object ? cf.utf8(object) : std::string("(nil)"); }
    Address alloc_init(std::string_view name) { return id(id(cls(name), "alloc"), "init"); }
    Address with_rect(Address receiver, std::string_view selector, gui::Rect rect, std::initializer_list<Address> arguments = {}) {
        CallRegs registers;
        appkit::put_rect(registers, 0, rect);
        unsigned index = 2;
        for (const auto value : arguments) registers.x[index++] = value;
        return msg(receiver, selector, registers).x0;
    }
    Address view(gui::Rect frame, std::string_view name = "NSView") { return with_rect(id(cls(name), "alloc"), "initWithFrame:", frame); }
    Address window(gui::Rect content, std::uint64_t style = 1 | 2 | 8) {   // titled, closable, resizable
        CallRegs registers;
        appkit::put_rect(registers, 0, content);
        registers.x[2] = style;
        registers.x[3] = 2;   // NSBackingStoreBuffered
        registers.x[4] = 0;
        const auto made = msg(id(cls("NSWindow"), "alloc"), "initWithContentRect:styleMask:backing:defer:", registers).x0;
        static_cast<void>(id(made, "setReleasedWhenClosed:", {0}));
        return made;
    }
    Address subclass(std::string name, std::string_view super) { return objc.register_native_class(std::move(name), cls(super), 8); }
    void add(Address target, std::string name, transmute::GuestCallHandler handler, std::string types = "v@:@", bool class_method = false) {
        static_cast<void>(objc.add_native_method(target, std::move(name), std::move(handler), class_method, std::move(types)));
    }
    gui::Rect rect(const CallResult& r) { return {{r.d(0), r.d(1)}, {r.d(2), r.d(3)}}; }
    void spin() {
        if (auto* loop = appkit::run_loop_or_null()) static_cast<void>(loop->run_once("kCFRunLoopDefaultMode", 0.05, false));
    }
};

void responders_and_cells(Fixture& f) {
    // NSResponder's menu and restorable state.
    const auto plain = f.view({{0, 0}, {10, 10}});
    const auto menu = f.alloc_init("NSMenu");
    static_cast<void>(f.id(plain, "setMenu:", {menu}));
    require(f.id(plain, "menu") == menu, "-[NSResponder setMenu:] / -menu");
    require(f.id(plain, "validRequestorForSendType:returnType:", {f.str("public.utf8-plain-text"), 0}) == 0,
            "a responder chain without services answers no requestor");
    const auto restorable = f.subclass("ApiRestorableView", "NSView");
    f.add(restorable, "restorableStateKeyPaths", [&f](CpuState& s, GuestMemory&) { s.x[0] = f.array({f.str("toolTip")}); }, "@@:", true);
    const auto coder_class = f.subclass("ApiCoder", "NSObject");
    f.add(coder_class, "encodeObject:forKey:", [&f](CpuState& s, GuestMemory&) { journal.push_back("encode " + f.text(s.x[3]) + "=" + f.text(s.x[2])); }, "v@:@@");
    f.add(coder_class, "containsValueForKey:", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:@");
    f.add(coder_class, "decodeObjectForKey:", [&f](CpuState& s, GuestMemory&) { s.x[0] = f.str("restored"); }, "@@:@");
    const auto restoring = f.view({{0, 0}, {10, 10}}, "ApiRestorableView");
    static_cast<void>(f.id(restoring, "setToolTip:", {f.str("saved")}));
    const auto coder = f.alloc_init("ApiCoder");
    static_cast<void>(f.id(restoring, "encodeRestorableStateWithCoder:", {coder}));
    require(logged("encode toolTip=saved"), "restorable key paths are encoded");
    static_cast<void>(f.id(restoring, "restoreStateWithCoder:", {coder}));
    require(f.text(f.id(restoring, "toolTip")) == "restored", "restorable key paths are restored");

    // NSCell.
    const auto a = f.id(f.id(f.cls("NSCell"), "alloc"), "initTextCell:", {f.str("apple")});
    const auto b = f.id(f.id(f.cls("NSCell"), "alloc"), "initTextCell:", {f.str("banana")});
    require(static_cast<std::int64_t>(f.id(a, "compare:", {b})) == -1, "-[NSCell compare:] orders by string value");
    static_cast<void>(f.id(a, "setMenu:", {menu}));
    require(f.id(a, "menu") == menu, "-[NSCell menu]");
    require(static_cast<std::int64_t>(f.id(a, "baseWritingDirection")) == -1, "cells start with the natural writing direction");
    static_cast<void>(f.id(a, "setBaseWritingDirection:", {1}));
    static_cast<void>(f.id(a, "setUserInterfaceLayoutDirection:", {1}));
    static_cast<void>(f.id(a, "setBackgroundStyle:", {1}));
    require(f.id(a, "baseWritingDirection") == 1 && f.id(a, "userInterfaceLayoutDirection") == 1 && f.id(a, "backgroundStyle") == 1 &&
                f.id(a, "interiorBackgroundStyle") == 1, "cell direction and background style");
    require((f.id(a, "allowsUndo") & 0xff) == 1, "cells allow undo by default");
    static_cast<void>(f.id(a, "setEditable:", {1}));
    require((f.id(a, "acceptsFirstResponder") & 0xff) == 1, "an editable cell accepts first responder");
    static_cast<void>(f.id(a, "setRefusesFirstResponder:", {1}));
    require((f.id(a, "acceptsFirstResponder") & 0xff) == 0, "a cell that refuses first responder does not accept it");

    // NSControl.
    const auto button = f.view({{0, 0}, {80, 30}}, "NSButton");
    static_cast<void>(f.id(button, "setTag:", {7}));
    require(f.id(button, "selectedTag") == 7, "-[NSControl selectedTag] is the selected cell's tag");
    static_cast<void>(f.id(button, "setBaseWritingDirection:", {1}));
    require(f.id(f.id(button, "cell"), "baseWritingDirection") == 1, "the control's writing direction is its cell's");
    const auto field = f.view({{0, 0}, {80, 22}}, "NSTextField");
    const auto delegate_class = f.subclass("ApiFieldDelegate", "NSObject");
    f.add(delegate_class, "control:textShouldBeginEditing:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:@@");
    const auto delegate = f.alloc_init("ApiFieldDelegate");
    require((f.id(field, "textShouldBeginEditing:", {0}) & 0xff) == 1, "editing begins when the delegate does not object");
    static_cast<void>(f.id(field, "setDelegate:", {delegate}));
    require((f.id(field, "textShouldBeginEditing:", {0}) & 0xff) == 0, "the delegate's -control:textShouldBeginEditing: decides");
    require((f.id(field, "textShouldEndEditing:", {0}) & 0xff) == 1, "a delegate without -control:textShouldEndEditing: agrees");
}

void views(Fixture& f) {
    const auto journaling = f.subclass("ApiHidingView", "NSView");
    for (const auto* name : {"viewDidHide", "viewDidUnhide"}) {
        const std::string selector = name;
        f.add(journaling, selector, [selector](CpuState& s, GuestMemory&) { journal.push_back(selector + " " + std::to_string(s.x[0])); }, "v@:");
    }
    const auto parent = f.view({{0, 0}, {100, 100}}, "ApiHidingView");
    const auto child = f.view({{0, 0}, {10, 10}}, "ApiHidingView");
    const auto hidden_child = f.view({{0, 0}, {10, 10}}, "ApiHidingView");
    static_cast<void>(f.id(parent, "addSubview:", {child}));
    static_cast<void>(f.id(parent, "addSubview:", {hidden_child}));
    static_cast<void>(f.id(hidden_child, "setHidden:", {1}));
    journal.clear();
    static_cast<void>(f.id(parent, "setHidden:", {1}));
    require(logged("viewDidHide " + std::to_string(parent)) && logged("viewDidHide " + std::to_string(child)),
            "-viewDidHide reaches the view and its visible descendants");
    require(!logged("viewDidHide " + std::to_string(hidden_child)), "a descendant hidden itself hears nothing");
    static_cast<void>(f.id(parent, "setHidden:", {0}));
    require(logged("viewDidUnhide " + std::to_string(child)), "-viewDidUnhide on unhiding");

    static_cast<void>(f.id(child, "setUserInterfaceLayoutDirection:", {1}));
    require(f.id(child, "userInterfaceLayoutDirection") == 1, "views keep their layout direction");
    require(f.id(child, "animationForKey:", {f.str("frameOrigin")}) == 0, "no animation by default");
    const auto animation = f.alloc_init("NSObject");
    const std::vector<std::pair<Address, Address>> entries{{f.str("frameOrigin"), animation}};
    static_cast<void>(f.id(child, "setAnimations:", {f.cf.dictionary(entries)}));
    require(f.id(child, "animationForKey:", {f.str("frameOrigin")}) == animation, "-animationForKey: answers the view's animations");

    // Safe area under the titlebar of a full-size content window.
    const auto plain_window = f.window({{100, 100}, {300, 200}});
    const auto content = f.id(plain_window, "contentView");
    auto insets = f.msg(content, "safeAreaInsets");
    require(near(insets.d(0), 0), "no safe-area inset without a full-size content view");
    const auto full = f.window({{100, 100}, {300, 200}}, 1 | 2 | 8 | (1ULL << 15));
    const auto full_content = f.id(full, "contentView");
    insets = f.msg(full_content, "safeAreaInsets");
    require(insets.d(0) >= 0 && near(insets.d(1), 0) && near(insets.d(2), 0), "safe-area insets of a full-size content view");

    // Backing alignment: inward edges at 2x.
    const auto aligned = appkit::backing_aligned({{0.3, 0.3}, {10.4, 10.4}}, 2, 0xF);   // NSAlignAllEdgesInward
    require(near(aligned.origin.x, 0.5) && near(aligned.size.width, 10.0), "inward edges at scale 2");
    const auto outward = appkit::backing_aligned({{0.3, 0.3}, {10.4, 10.4}}, 1, 0xF00);   // NSAlignAllEdgesOutward
    require(near(outward.origin.x, 0) && near(outward.size.width, 11), "outward edges at scale 1");
    const auto nearest = appkit::backing_aligned({{0.4, 0.6}, {9.2, 9.2}}, 1, (1ULL << 16) | (1ULL << 17) | (1ULL << 20) | (1ULL << 21));
    require(near(nearest.origin.x, 0) && near(nearest.origin.y, 1) && near(nearest.size.width, 9), "nearest origin and size");
    const auto in_view = f.rect(f.msg(full_content, "backingAlignedRect:options:", [] {
        CallRegs r; appkit::put_rect(r, 0, {{1.2, 1.2}, {5.5, 5.5}}); r.x[2] = 0xF00; return r; }()));
    require(in_view.size.width >= 5.5 && in_view.origin.x <= 1.2, "-[NSView backingAlignedRect:options:] covers the rect outward");
}

void windows(Fixture& f) {
    const auto target = f.window({{200, 200}, {400, 300}});
    static_cast<void>(f.id(target, "makeKeyAndOrderFront:", {0}));
    const auto kit = appkit::instance();
    const auto id = kit->window_state(target)->id;
    CallRegs ratio;
    appkit::put_size(ratio, 0, {16, 9});
    static_cast<void>(f.msg(target, "setContentAspectRatio:", ratio));
    const auto resized = kit->will_resize(id, {800, 100});
    require(near(resized.width, 800) && near(resized.height, 450), "the content aspect ratio derives the height while resizing");
    static_cast<void>(f.msg(target, "setAspectRatio:", ratio));
    const auto kept = f.msg(target, "contentAspectRatio");
    require(near(kept.d(0), 0) && near(kept.d(1), 0), "-setAspectRatio: replaces the content aspect ratio");

    // Constraining to the screen: the top edge comes down onto the visible frame.
    const auto screen = f.id(target, "screen");
    const auto visible = f.rect(f.msg(screen, "visibleFrame"));
    const gui::Rect high{{0, visible.origin.y + visible.size.height}, {100, 100}};
    const auto constrained = f.rect(f.msg(target, "constrainFrameRect:toScreen:", [&] {
        CallRegs r; appkit::put_rect(r, 0, high); r.x[2] = screen; return r; }()));
    require(near(constrained.origin.y + constrained.size.height, visible.origin.y + visible.size.height), "constrained top edge");

    // Cascading moves the window and offsets the next point.
    const gui::Point start{visible.origin.x + 50, visible.origin.y + visible.size.height - 50};
    CallRegs cascade;
    appkit::put_point(cascade, 0, start);
    const auto next = f.msg(target, "cascadeTopLeftFromPoint:", cascade);
    const auto frame = f.rect(f.msg(target, "frame"));
    require(near(frame.origin.x, start.x) && near(frame.origin.y + frame.size.height, start.y), "the window's top-left is the point");
    require(next.d(0) > start.x && next.d(1) < start.y, "the next cascade point is right and down");

    // canHide: NSApp's hide leaves the window.
    static_cast<void>(f.id(target, "setCanHide:", {0}));
    const auto other = f.window({{50, 50}, {200, 100}});
    static_cast<void>(f.id(other, "makeKeyAndOrderFront:", {0}));
    static_cast<void>(f.id(f.app, "hide:", {0}));
    require((f.id(target, "isVisible") & 0xff) == 1 && (f.id(other, "isVisible") & 0xff) == 0, "-setCanHide:NO keeps the window shown");
    static_cast<void>(f.id(f.app, "unhide:", {0}));
    require(static_cast<std::int64_t>(f.id(other, "orderedIndex")) == 0, "the last window ordered front has index 0");
    static_cast<void>(f.id(other, "setOrderedIndex:", {1}));
    require(static_cast<std::int64_t>(f.id(target, "orderedIndex")) == 0, "-setOrderedIndex: moves the window back");

    const auto taller = f.rect(f.msg(target, "frame"));
    const auto time = f.msg(target, "animationResizeTime:", [&] {
        CallRegs r; appkit::put_rect(r, 0, {taller.origin, {taller.size.width, taller.size.height + 150}}); return r; }()).d(0);
    require(near(time, 0.2), "0.2 seconds per 150 points of height change: " + std::to_string(time));
    static_cast<void>(f.id(target, "setToolbarStyle:", {3}));
    require(f.id(target, "toolbarStyle") == 3, "the toolbar style is kept");
}

void menus(Fixture& f) {
    const auto menu = f.alloc_init("NSMenu");
    const auto target_class = f.subclass("ApiMenuTarget", "NSObject");
    f.add(target_class, "picked:", [](CpuState& s, GuestMemory&) { journal.push_back("picked " + std::to_string(s.x[2])); });
    const auto target = f.alloc_init("ApiMenuTarget");
    const auto first = f.id(menu, "addItemWithTitle:action:keyEquivalent:", {f.str("First"), f.objc.selector("picked:"), f.str("")});
    static_cast<void>(f.id(menu, "addItem:", {f.id(f.cls("NSMenuItem"), "separatorItem")}));
    const auto second = f.id(menu, "addItemWithTitle:action:keyEquivalent:", {f.str("Second"), f.objc.selector("picked:"), f.str("")});
    static_cast<void>(f.id(second, "setTarget:", {target}));
    static_cast<void>(f.id(first, "setRepresentedObject:", {f.str("one")}));
    const auto size = f.msg(menu, "size");
    require(size.d(0) > 44 && near(size.d(1), 5 + 22 + 11 + 22 + 5), "-[NSMenu size]: rows, separator and margins");
    require(f.id(menu, "indexOfItemWithRepresentedObject:", {f.str("one")}) == 0, "an equal represented object is found");
    require(f.id(menu, "indexOfItemWithTarget:andAction:", {target, f.objc.selector("picked:")}) == 2, "target and action");
    require(f.id(menu, "indexOfItemWithTarget:andAction:", {target, 0}) == 2, "a NULL action matches the target alone");
    journal.clear();
    static_cast<void>(f.id(menu, "performActionForItemAtIndex:", {2}));
    require(logged("picked " + std::to_string(second)), "-performActionForItemAtIndex: sends the item's action");
    // Submenus: parentItem and hidden ancestors.
    const auto submenu = f.alloc_init("NSMenu");
    const auto inner = f.id(submenu, "addItemWithTitle:action:keyEquivalent:", {f.str("Inner"), 0, f.str("")});
    static_cast<void>(f.id(first, "setSubmenu:", {submenu}));
    require(f.id(inner, "parentItem") == first, "-parentItem is the item holding the submenu");
    require((f.id(inner, "isHiddenOrHasHiddenAncestor") & 0xff) == 0, "visible chain");
    static_cast<void>(f.id(first, "setHidden:", {1}));
    require((f.id(inner, "isHiddenOrHasHiddenAncestor") & 0xff) == 1, "a hidden parent item hides the inner item");
    // Popup cells and buttons.
    const auto popup = f.with_rect(f.id(f.cls("NSPopUpButton"), "alloc"), "initWithFrame:pullsDown:", {{0, 0}, {120, 26}}, {0});
    static_cast<void>(f.id(popup, "addItemsWithTitles:", {f.array({f.str("A"), f.str("B")})}));
    const auto cell = f.id(popup, "cell");
    require(f.id(cell, "numberOfItems") == 2 && f.cf.length(f.id(cell, "itemArray")) == 2, "popup cell items");
    static_cast<void>(f.id(popup, "setPreferredEdge:", {3}));
    require(f.id(cell, "preferredEdge") == 3 && f.id(popup, "preferredEdge") == 3, "the preferred edge is the cell's");
    static_cast<void>(f.id(popup, "setShowsStateColumn:", {0}));
    require((f.id(f.id(popup, "menu"), "showsStateColumn") & 0xff) == 0, "the state column is the menu's");
    static_cast<void>(f.id(popup, "setAutoenablesItems:", {0}));
    require((f.id(f.id(popup, "menu"), "autoenablesItems") & 0xff) == 0, "item enabling is the menu's");
}

void images_and_cursors(Fixture& f) {
    const auto image = f.alloc_init("NSImage");
    require(near(f.msg(f.id(image, "backgroundColor"), "alphaComponent").d(0), 0), "images draw over a clear background");
    static_cast<void>(f.id(image, "setAccessibilityDescription:", {f.str("logo")}));
    require(f.text(f.id(image, "accessibilityDescription")) == "logo", "-accessibilityDescription");
    CallRegs insets;
    for (unsigned i = 0; i < 4; ++i) insets.set_d(i, i + 1.0);
    static_cast<void>(f.msg(image, "setCapInsets:", insets));
    const auto caps = f.msg(image, "capInsets");
    require(near(caps.d(0), 1) && near(caps.d(3), 4), "-capInsets");
    static_cast<void>(f.id(image, "setResizingMode:", {1}));
    require(f.id(image, "resizingMode") == 1, "-resizingMode");
    const auto copy = f.id(image, "copyWithZone:", {0});
    require(f.text(f.id(copy, "accessibilityDescription")) == "logo" && f.id(copy, "resizingMode") == 1, "copies keep the new properties");

    // Bitmap pixels and CGImage.
    const auto rep = appkit::instance()->make_bitmap(4, 4);
    const auto samples = f.block(32);
    for (unsigned i = 0; i < 4; ++i) f.engine.write64(samples + 8 * i, 10 + i);
    static_cast<void>(f.id(rep, "setPixel:atX:y:", {samples, 2, 1}));
    const auto back = f.block(32);
    static_cast<void>(f.id(rep, "getPixel:atX:y:", {back, 2, 1}));
    require(f.engine.read64(back) == 10 && f.engine.read64(back + 24) == 13, "-setPixel:atX:y: / -getPixel:atX:y: round trip");
    require((f.id(rep, "isOpaque") & 0xff) == 0, "a bitmap with alpha is not opaque");
    static_cast<void>(f.id(rep, "setOpaque:", {1}));
    require((f.id(rep, "isOpaque") & 0xff) == 1, "-setOpaque:");
    const auto cg_image = f.id(rep, "CGImageForProposedRect:context:hints:", {0, 0, 0});
    require(cg_image && f.call("/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics", "_CGImageGetWidth", {cg_image}) == 4,
            "a bitmap rep answers its CGImage");

    // Cursors.
    const auto cursor_class = f.cls("NSCursor");
    require(f.id(cursor_class, "columnResizeCursor") == f.id(cursor_class, "resizeLeftRightCursor"), "column resizing");
    const auto zoom = f.id(cursor_class, "zoomInCursor");
    require(zoom && zoom == f.id(cursor_class, "zoomInCursor") && zoom != f.id(cursor_class, "zoomOutCursor"), "zoom cursors are singletons");
    const auto zoom_size = f.msg(f.id(zoom, "image"), "size");
    require(near(zoom_size.d(0), 24) && near(zoom_size.d(1), 24), "the zoom cursor image");
    const auto corner = f.id(cursor_class, "frameResizeCursorFromPosition:inDirections:", {1 | 2, 3});
    const auto opposite = f.id(cursor_class, "frameResizeCursorFromPosition:inDirections:", {4 | 8, 3});
    const auto other_corner = f.id(cursor_class, "frameResizeCursorFromPosition:inDirections:", {1 | 8, 3});
    require(corner == opposite && corner != other_corner, "top-left and bottom-right share the diagonal shape");
    require(f.id(cursor_class, "frameResizeCursorFromPosition:inDirections:", {1, 2}) == f.id(cursor_class, "resizeUpCursor"),
            "outward from the top edge is up");
}

void alerts_and_panels(Fixture& f) {
    const auto alert = f.alloc_init("NSAlert");
    const auto ok = f.id(alert, "addButtonWithTitle:", {f.str("OK")});
    const auto cancel = f.id(alert, "addButtonWithTitle:", {f.str("Cancel")});
    require(ok && cancel && f.cf.length(f.id(alert, "buttons")) == 2, "-addButtonWithTitle: makes NSButtons");
    require(f.id(ok, "tag") == 1000 && f.id(cancel, "tag") == 1001, "button tags are the responses");
    require(f.text(f.id(ok, "keyEquivalent")) == "\r" && f.text(f.id(cancel, "keyEquivalent")) == "\x1b", "Return and Escape");
    static_cast<void>(f.id(cancel, "setTitle:", {f.str("Not now")}));
    std::vector<std::string> shown;
    appkit::set_alert_presenter_override([&shown](const appkit::AlertRequest& request) { shown = request.buttons; return std::size_t{1}; });
    const auto delegate_class = f.subclass("ApiAlertDelegate", "NSObject");
    f.add(delegate_class, "alertDidEnd:returnCode:contextInfo:", [](CpuState& s, GuestMemory&) {
        journal.push_back("ended " + std::to_string(s.x[3]) + " " + std::to_string(s.x[4]));
    }, "v@:@q^v");
    const auto delegate = f.alloc_init("ApiAlertDelegate");
    const auto owner = f.window({{10, 10}, {300, 200}});
    journal.clear();
    static_cast<void>(f.id(alert, "beginSheetModalForWindow:modalDelegate:didEndSelector:contextInfo:",
                           {owner, delegate, f.objc.selector("alertDidEnd:returnCode:contextInfo:"), 0x1234}));
    for (int i = 0; i < 5 && !logged("ended 1001 4660"); ++i) f.spin();
    require(logged("ended 1001 4660"), "the delegate form of the alert sheet reports the response and context");
    require(shown.size() == 2 && shown[1] == "Not now", "the task dialog shows the buttons' current titles");

    // NSApplication: ending a sheet through NSApp.
    const auto parent = f.window({{10, 10}, {400, 300}});
    static_cast<void>(f.id(parent, "makeKeyAndOrderFront:", {0}));
    const auto sheet = f.window({{0, 0}, {200, 100}});
    static_cast<void>(f.id(parent, "beginSheet:completionHandler:", {sheet, 0}));
    require(f.id(sheet, "sheetParent") == parent, "the sheet is attached");
    static_cast<void>(f.id(f.app, "endSheet:returnCode:", {sheet, 5}));
    require(f.id(parent, "attachedSheet") == 0, "-[NSApplication endSheet:returnCode:] ends it through the parent");

    // Remote notifications fail through the delegate.
    const auto app_delegate_class = f.subclass("ApiAppDelegate", "NSObject");
    f.add(app_delegate_class, "application:didFailToRegisterForRemoteNotificationsWithError:", [&f](CpuState& s, GuestMemory&) {
        journal.push_back("remote " + f.text(f.id(s.x[3], "domain")));
    }, "v@:@@");
    static_cast<void>(f.id(f.app, "setDelegate:", {f.alloc_init("ApiAppDelegate")}));
    journal.clear();
    static_cast<void>(f.id(f.app, "registerForRemoteNotifications"));
    for (int i = 0; i < 5 && !logged("remote NSCocoaErrorDomain"); ++i) f.spin();
    require(logged("remote NSCocoaErrorDomain"), "registering for remote notifications fails through the delegate");

    // NSSavePanel.
    const auto panel = f.id(f.cls("NSSavePanel"), "savePanel");
    static_cast<void>(f.id(panel, "setTagNames:", {f.array({f.str("Red")})}));
    static_cast<void>(f.id(panel, "setCanSelectHiddenExtension:", {1}));
    static_cast<void>(f.id(panel, "setShowsTagField:", {0}));
    require(f.cf.length(f.id(panel, "tagNames")) == 1 && (f.id(panel, "canSelectHiddenExtension") & 0xff) == 1 &&
                (f.id(panel, "showsTagField") & 0xff) == 0, "save panel tag and extension settings");
    static_cast<void>(f.id(panel, "cancel:", {0}));   // not running: nothing to end

    // NSFontManager target and action.
    const auto fonts = f.id(f.cls("NSFontManager"), "sharedFontManager");
    require(f.id(fonts, "action") == f.objc.selector("changeFont:"), "the font manager sends -changeFont:");
    static_cast<void>(f.id(fonts, "setTarget:", {delegate}));
    static_cast<void>(f.id(fonts, "setEnabled:", {0}));
    require(f.id(fonts, "target") == delegate && (f.id(fonts, "isEnabled") & 0xff) == 0, "font manager target and enabling");
    static_cast<void>(f.id(fonts, "setEnabled:", {1}));
}

void pasteboards_and_text(Fixture& f) {
    const auto board = f.id(f.cls("NSPasteboard"), "pasteboardWithUniqueName");
    const auto before = f.id(board, "changeCount");
    static_cast<void>(f.id(board, "prepareForNewContentsWithOptions:", {1}));
    require(f.id(board, "changeCount") > before, "-prepareForNewContentsWithOptions: starts new contents");
    const auto provider_class = f.subclass("ApiProvider", "NSObject");
    f.add(provider_class, "pasteboard:provideDataForType:", [&f](CpuState& s, GuestMemory&) {
        static_cast<void>(f.id(s.x[2], "setString:forType:", {f.str("provided"), s.x[3]}));
    }, "v@:@@");
    const auto type = f.str("public.utf8-plain-text");
    require((f.id(board, "setDataProvider:forTypes:", {f.alloc_init("ApiProvider"), f.array({type})}) & 0xff) == 1, "provider set");
    require(f.text(f.id(board, "stringForType:", {type})) == "provided", "the provider's data is on the pasteboard");
    require(f.id(board, "accessBehavior") == 2, "Windows always allows clipboard access");

    // NSTextView actions.
    const auto text_view = f.view({{0, 0}, {200, 100}}, "NSTextView");
    static_cast<void>(f.id(text_view, "setString:", {f.str("hello world")}));
    CallRegs range;
    range.x[2] = 5;
    range.x[3] = 6;
    static_cast<void>(f.msg(text_view, "setSelectedRange:", range));
    const auto copy_board = f.id(f.cls("NSPasteboard"), "pasteboardWithUniqueName");
    require((f.id(text_view, "writeSelectionToPasteboard:types:", {copy_board, f.array({type})}) & 0xff) == 1, "selection written");
    require(f.text(f.id(copy_board, "stringForType:", {type})) == " world", "the selection is on the pasteboard");
    static_cast<void>(f.id(text_view, "delete:", {0}));
    require(f.text(f.id(text_view, "string")) == "hello", "-delete: removes the selection");
    static_cast<void>(f.id(text_view, "insertTabIgnoringFieldEditor:", {0}));
    require(f.text(f.id(text_view, "string")) == "hello\t", "-insertTabIgnoringFieldEditor:");
    require((f.id(text_view, "readSelectionFromPasteboard:", {copy_board}) & 0xff) == 1 &&
                f.text(f.id(text_view, "string")) == "hello\t world", "-readSelectionFromPasteboard: inserts its text");
    const bool spelling = f.id(text_view, "isContinuousSpellCheckingEnabled") & 0xff;
    static_cast<void>(f.id(text_view, "toggleContinuousSpellChecking:", {0}));
    require(static_cast<bool>(f.id(text_view, "isContinuousSpellCheckingEnabled") & 0xff) != spelling, "the toggle flips the setting");
    static_cast<void>(f.id(text_view, "setBaseWritingDirection:", {1}));
    require(f.id(text_view, "baseWritingDirection") == 1, "text writing direction");
}

void containers_and_controllers(Fixture& f) {
    // NSTabView.
    const auto tabs = f.view({{0, 0}, {300, 200}}, "NSTabView");
    std::vector<Address> items;
    for (const auto* name : {"one", "two"}) {
        const auto item = f.id(f.id(f.cls("NSTabViewItem"), "alloc"), "initWithIdentifier:", {f.str(name)});
        items.push_back(item);
    }
    static_cast<void>(f.id(tabs, "setTabViewItems:", {f.array(items)}));
    require(f.id(tabs, "numberOfTabViewItems") == 2 && f.id(tabs, "tabViewItemAtIndex:", {1}) == items[1], "-setTabViewItems:");
    static_cast<void>(f.id(tabs, "setControlSize:", {1}));
    require(f.id(tabs, "controlSize") == 1, "tab view control size");
    // NSSplitView arranged subviews.
    const auto split = f.view({{0, 0}, {300, 100}}, "NSSplitView");
    const auto a = f.view({{0, 0}, {100, 100}}), b = f.view({{0, 0}, {100, 100}}), c = f.view({{0, 0}, {100, 100}});
    static_cast<void>(f.id(split, "addArrangedSubview:", {a}));
    static_cast<void>(f.id(split, "addArrangedSubview:", {c}));
    static_cast<void>(f.id(split, "insertArrangedSubview:atIndex:", {b, 1}));
    const auto arranged = f.id(split, "arrangedSubviews");
    require(f.cf.element(arranged, 0) == a && f.cf.element(arranged, 1) == b && f.cf.element(arranged, 2) == c, "insertion order");
    static_cast<void>(f.id(split, "removeArrangedSubview:", {b}));
    require(f.cf.length(f.id(split, "arrangedSubviews")) == 2 && f.id(b, "superview") == 0, "-removeArrangedSubview:");
    // NSGridView rows.
    const auto grid = f.id(f.cls("NSGridView"), "gridViewWithNumberOfColumns:rows:", {1, 3});
    const auto first_row = f.id(grid, "rowAtIndex:", {0});
    static_cast<void>(f.id(grid, "moveRowAtIndex:toIndex:", {0, 2}));
    require(f.id(grid, "rowAtIndex:", {2}) == first_row, "-moveRowAtIndex:toIndex:");
    // Controllers.
    const auto controller = f.id(f.id(f.cls("NSWindowController"), "alloc"), "initWithWindow:", {0});
    const auto content = f.alloc_init("NSViewController");
    static_cast<void>(f.id(content, "setView:", {f.view({{0, 0}, {50, 50}})}));
    static_cast<void>(f.id(controller, "setContentViewController:", {content}));
    require(f.id(controller, "contentViewController") == content, "pending content view controller");
    const auto shown = f.window({{0, 0}, {200, 200}});
    static_cast<void>(f.id(controller, "setWindow:", {shown}));
    require(f.id(shown, "contentViewController") == content, "the window gets the content view controller");
    static_cast<void>(f.id(controller, "setDocumentEdited:", {1}));
    require((f.id(shown, "isDocumentEdited") & 0xff) == 1, "-setDocumentEdited: reaches the window");
    static_cast<void>(f.id(content, "setIdentifier:", {f.str("main")}));
    require(f.text(f.id(content, "identifier")) == "main", "view controller identifier");
    // Controls.
    const auto box = f.view({{0, 0}, {100, 100}}, "NSBox");
    CallRegs width;
    width.set_d(0, 3);
    static_cast<void>(f.msg(box, "setBorderWidth:", width));
    require(near(f.msg(box, "borderWidth").d(0), 3), "box border width");
    // Values.
    const auto rgb = f.id(f.cls("NSColorSpace"), "sRGBColorSpace");
    const auto components = f.block(32);
    const double values[] = {0.25, 0.5, 0.75, 0.5};
    for (unsigned i = 0; i < 4; ++i) f.engine.write64(components + 8 * i, std::bit_cast<std::uint64_t>(values[i]));
    const auto made = f.id(f.cls("NSColor"), "colorWithColorSpace:components:count:", {rgb, components, 4});
    require(near(f.msg(made, "greenComponent").d(0), 0.5) && near(f.msg(made, "alphaComponent").d(0), 0.5), "colour from components");
    require(f.id(f.cls("NSColor"), "currentControlTint") == 1, "blue control tint");
    const auto list = f.id(f.id(f.cls("NSColorList"), "alloc"), "initWithName:", {f.str("api")});
    static_cast<void>(f.id(list, "setColor:forKey:", {made, f.str("b")}));
    static_cast<void>(f.id(list, "insertColor:key:atIndex:", {made, f.str("a"), 0}));
    require(f.text(f.cf.element(f.id(list, "allKeys"), 0)) == "a", "-insertColor:key:atIndex:");
    const auto attachment = f.alloc_init("NSTextAttachment");
    static_cast<void>(f.with_rect(attachment, "setBounds:", {{0, -2}, {16, 16}}));
    const auto bounds = f.rect(f.msg(attachment, "bounds"));
    require(near(bounds.origin.y, -2) && near(bounds.size.width, 16), "text attachment bounds");
    const auto spell = f.cls("NSSpellChecker");
    const auto tag = f.id(spell, "uniqueSpellDocumentTag");
    require(f.id(spell, "uniqueSpellDocumentTag") == tag + 1, "+uniqueSpellDocumentTag counts");
    const auto toolbar_item = f.id(f.id(f.cls("NSToolbarItem"), "alloc"), "initWithItemIdentifier:", {f.str("item")});
    static_cast<void>(f.id(toolbar_item, "setTitle:", {f.str("Go")}));
    static_cast<void>(f.id(toolbar_item, "setHidden:", {1}));
    require(f.text(f.id(toolbar_item, "title")) == "Go" && (f.id(toolbar_item, "isHidden") & 0xff) == 1 &&
                (f.id(toolbar_item, "isVisible") & 0xff) == 0, "toolbar item title and visibility");
}

} // namespace

int main() {
#ifdef _WIN32
    _putenv_s("ALCHEMY_GUI_NO_ACTIVATE", "1");
#endif
    try {
        Fixture f;
        for (const auto& [name, test] : std::initializer_list<std::pair<const char*, void (*)(Fixture&)>>{
                 {"responders_and_cells", responders_and_cells}, {"views", views}, {"windows", windows}, {"menus", menus},
                 {"images_and_cursors", images_and_cursors}, {"alerts_and_panels", alerts_and_panels},
                 {"pasteboards_and_text", pasteboards_and_text}, {"containers_and_controllers", containers_and_controllers}}) {
            std::cout << "[appkit api] " << name << std::endl;
            test(f);
        }
    } catch (const std::exception& error) {
        std::cerr << "AppKit API test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Alchemy AppKit API tests passed\n";
    return 0;
}
