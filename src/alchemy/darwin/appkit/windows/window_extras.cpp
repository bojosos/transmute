#include "alchemy/darwin/appkit/windows/windows_internal.hpp"

#include <algorithm>
#include <cmath>
#include <set>

// NSWindow, NSView, NSApplication and NSEvent additions of QLab gap group
// G4:
//   * sheets: -beginSheet:completionHandler: (and the critical variant)
//     attaches a window as a document-modal sheet (a child window placed at
//     the top of the parent's content, centered; the parent takes no input
//     while it is attached, modal.cpp), -endSheet: / -endSheet:returnCode:
//     detach and close it and call the completion handler; -attachedSheet,
//     -sheets, -sheetParent, -isSheet;
//   * NSWindow properties: subtitle (shown after the title in the caption),
//     identifier, full-screen content size limits, content border
//     thickness, concurrent view drawing, backing type (always buffered, as
//     on macOS since 10.14), the content view controller;
//   * -[NSWindow nextEventMatchingMask:] (and the four-argument form):
//     events from the application's queue in the event-tracking mode;
//   * NSView: tool-tip rectangles, tracking rectangles (NSTrackingArea
//     based), -needsToDrawRect:, -mouse:inRect:, -replaceSubview:with:,
//     -setSubviews:, -removeFromSuperviewWithoutNeedingDisplay,
//     concurrent drawing, clipping, the view shadow;
//   * -[NSObject accessibilitySetOverrideValue:forAttribute:] (the nib
//     accessibility connectors) with -accessibilityAttributeValue:;
//   * -[NSTableRowView viewAtColumn:], -[NSEvent touchesMatchingPhase:inView:],
//     NSAnimationContext -groupingLevel;
//   * AppKit constants and functions: NSAccessibilityOutlineRowSubrole,
//     NSCollectionElementKindSectionHeader, NSImageNameComputer,
//     NSAccessibilityPostNotificationWithUserInfo, NSDrawThreePartImage.
namespace alchemy::gui::completion_kit::windows {
namespace {
using appkit::bool_arg;
using appkit::d;
using appkit::point_arg;
using appkit::put_point;
using appkit::put_rect;
using appkit::put_size;
using appkit::rect_arg;
using appkit::result_rect;
using appkit::result_size;
using appkit::return_bool;
using appkit::return_rect;
using appkit::return_size;
using appkit::set_d;
using appkit::size_arg;

constexpr std::int64_t modal_response_stop = -1000;   // NSModalResponseStop

Address block_copy(Address block) { return block ? kit().guest_call(appkit::libsystem_library, "__Block_copy", {block}) : 0; }
void block_release(Address block) { if (block) static_cast<void>(kit().guest_call(appkit::libsystem_library, "__Block_release", {block})); }

// ---- sheets ----
void begin_sheet(Address parent, Address sheet, Address handler) {
    auto& k = kit();
    if (!sheet || !k.is_kind(sheet, k.window)) completion_kit::fail("-[NSWindow beginSheet:completionHandler:] needs a window");
    const auto parent_state = k.window_state(parent);
    if (parent_state->attached_sheet)
        completion_kit::unsupported("a second sheet on a window that already has one (queued sheets)");
    const auto sheet_state = k.window_state(sheet);
    parent_state->attached_sheet = k.retain(sheet);
    sheet_state->sheet_parent = parent;
    sheet_state->sheet_handler = block_copy(handler);
    // At the top of the parent's content area, centered horizontally.
    const auto content = k.content_rect(parent);
    const auto frame = result_rect(k.send(sheet, "frame"));
    CallRegs origin;
    put_point(origin, 0, {content.origin.x + (content.size.width - frame.size.width) / 2,
                          content.origin.y + content.size.height - frame.size.height});
    static_cast<void>(k.send(sheet, "setFrameOrigin:", origin));
    static_cast<void>(k.send_id(parent, "addChildWindow:ordered:", {sheet, 1}));
    static_cast<void>(k.send_id(sheet, "makeKeyAndOrderFront:", {0}));
    k.post(appkit::note::window_will_begin_sheet, parent);
}

void end_sheet(Address parent, Address sheet, std::int64_t code) {
    auto& k = kit();
    const auto parent_state = k.window_state(parent);
    if (!sheet || parent_state->attached_sheet != sheet) return;
    const auto sheet_state = k.window_state(sheet);
    const auto handler = std::exchange(sheet_state->sheet_handler, 0);
    sheet_state->sheet_parent = 0;
    parent_state->attached_sheet = 0;
    static_cast<void>(k.send_id(parent, "removeChildWindow:", {sheet}));
    static_cast<void>(k.send_id(sheet, "orderOut:", {0}));
    k.post(appkit::note::window_did_end_sheet, parent);
    if (parent_state->visible && !parent_state->closed) static_cast<void>(k.send_id(parent, "makeKeyWindow"));
    try {
        if (handler) static_cast<void>(k.invoke_block(handler, {static_cast<Address>(code)}));
    } catch (...) {
        block_release(handler);
        k.release(sheet);
        throw;
    }
    block_release(handler);
    k.release(sheet);
}

std::int64_t next_tag = 1;

// Accessibility overrides: an NSMutableDictionary (attribute -> value)
// kept with the object (released with it).
Address overrides_key() {
    static Address key{};
    if (!key) key = kit().process.allocate_guest(8);
    return key;
}
Address overrides_of(Address object, bool create) {
    auto& k = kit();
    // Views keep the table in their state; other objects as an associated object.
    if (k.is_kind(object, k.view)) {
        const auto state = k.view_state(object);
        if (!state->accessibility_overrides && create) state->accessibility_overrides = k.cf.dictionary({}, true);
        return state->accessibility_overrides;
    }
    auto table = k.guest_call(objc_library, "_objc_getAssociatedObject", {object, overrides_key()});
    if (!table && create) {
        table = k.cf.dictionary({}, true);
        static_cast<void>(k.guest_call(objc_library, "_objc_setAssociatedObject", {object, overrides_key(), table, 1}));   // RETAIN_NONATOMIC
        k.release(table);
    }
    return table;
}

// NSDrawThreePartImage: the start and end caps at their sizes, the center
// image tiled (or stretched when its edge is 0) between them.
void draw_three_part(Rect frame, Address start, Address center, Address end, bool vertical, std::uint64_t operation, double alpha,
                     bool flipped) {
    auto& k = kit();
    static_cast<void>(operation);
    const auto size_of = [&](Address image) { return image ? result_size(k.send(image, "size")) : Size{}; };
    const auto draw = [&](Address image, Rect rect) {
        if (!image || rect.size.width <= 0 || rect.size.height <= 0) return;
        k.draw_image_in(image, rect, alpha, flipped);
    };
    const auto a = size_of(start), c = size_of(center), b = size_of(end);
    if (!vertical) {
        draw(start, {frame.origin, {a.width, frame.size.height}});
        draw(end, {{frame.origin.x + frame.size.width - b.width, frame.origin.y}, {b.width, frame.size.height}});
        const double left = frame.origin.x + a.width, right = frame.origin.x + frame.size.width - b.width;
        if (c.width <= 0) { draw(center, {{left, frame.origin.y}, {right - left, frame.size.height}}); return; }
        for (double x = left; x < right; x += c.width) draw(center, {{x, frame.origin.y}, {std::min(c.width, right - x), frame.size.height}});
    } else {
        // Top to bottom on screen: the start cap is at the top.
        const double top_y = flipped ? frame.origin.y : frame.origin.y + frame.size.height - a.height;
        const double bottom_y = flipped ? frame.origin.y + frame.size.height - b.height : frame.origin.y;
        draw(start, {{frame.origin.x, top_y}, {frame.size.width, a.height}});
        draw(end, {{frame.origin.x, bottom_y}, {frame.size.width, b.height}});
        const double low = frame.origin.y + (flipped ? a.height : b.height);
        const double high = frame.origin.y + frame.size.height - (flipped ? b.height : a.height);
        if (c.height <= 0) { draw(center, {{frame.origin.x, low}, {frame.size.width, high - low}}); return; }
        for (double y = low; y < high; y += c.height) draw(center, {{frame.origin.x, y}, {frame.size.width, std::min(c.height, high - y)}});
    }
}
} // namespace

void install_window_extras(Support& support) {
    auto& k = kit();
    const auto w = k.window, v = k.view;

    // ---- sheets ----
    for (const auto* name : {"beginSheet:completionHandler:", "beginCriticalSheet:completionHandler:"})
        k.method(w, name, [](CpuState& s, GuestMemory&) { begin_sheet(s.x[0], s.x[2], s.x[3]); }, "v@:@@?");
    k.method(w, "endSheet:", [](CpuState& s, GuestMemory&) { end_sheet(s.x[0], s.x[2], modal_response_stop); }, "v@:@");
    k.method(w, "endSheet:returnCode:", [](CpuState& s, GuestMemory&) {
        end_sheet(s.x[0], s.x[2], static_cast<std::int64_t>(s.x[3]));
    }, "v@:@q");
    k.method(w, "attachedSheet", [](CpuState& s, GuestMemory&) { s.x[0] = kit().window_state(s.x[0])->attached_sheet; });
    k.method(w, "sheets", [](CpuState& s, GuestMemory&) {
        const auto sheet = kit().window_state(s.x[0])->attached_sheet;
        s.x[0] = kit().auto_array(sheet ? std::vector<Address>{sheet} : std::vector<Address>{});
    });
    k.method(w, "sheetParent", [](CpuState& s, GuestMemory&) { s.x[0] = kit().window_state(s.x[0])->sheet_parent; });
    k.method(w, "isSheet", [](CpuState& s, GuestMemory&) { return_bool(s, kit().window_state(s.x[0])->sheet_parent != 0); }, "B@:");

    // ---- window properties ----
    k.method(w, "subtitle", [](CpuState& s, GuestMemory&) {
        const auto value = kit().window_state(s.x[0])->subtitle;
        s.x[0] = value ? value : kit().auto_string("");
    });
    k.method(w, "setSubtitle:", [](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        const auto state = kk.window_state(s.x[0]);
        kk.release(std::exchange(state->subtitle, s.x[2] ? kk.cf.copy(s.x[2]) : 0));
        if (state->id) appkit::require_window_server("-[NSWindow setSubtitle:]").set_title(state->id, appkit::caption(kk, *state));
    }, "v@:@");
    k.method(w, "identifier", [](CpuState& s, GuestMemory&) { s.x[0] = kit().window_state(s.x[0])->identifier; });
    k.method(w, "setIdentifier:", [](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        kk.release(std::exchange(kk.window_state(s.x[0])->identifier, s.x[2] ? kk.cf.copy(s.x[2]) : 0));
    }, "v@:@");
    k.method(w, "minFullScreenContentSize", [](CpuState& s, GuestMemory&) { return_size(s, kit().window_state(s.x[0])->min_full_screen); }, "{CGSize=dd}@:");
    k.method(w, "setMinFullScreenContentSize:", [](CpuState& s, GuestMemory&) { kit().window_state(s.x[0])->min_full_screen = size_arg(s); }, "v@:{CGSize=dd}");
    k.method(w, "maxFullScreenContentSize", [](CpuState& s, GuestMemory&) { return_size(s, kit().window_state(s.x[0])->max_full_screen); }, "{CGSize=dd}@:");
    k.method(w, "setMaxFullScreenContentSize:", [](CpuState& s, GuestMemory&) { kit().window_state(s.x[0])->max_full_screen = size_arg(s); }, "v@:{CGSize=dd}");
    const auto edge = [](Address value) -> std::size_t {
        if (value > 3) completion_kit::fail("NSRectEdge " + std::to_string(value));
        return static_cast<std::size_t>(value);
    };
    k.method(w, "contentBorderThicknessForEdge:", [edge](CpuState& s, GuestMemory&) { set_d(s, 0, kit().window_state(s.x[0])->content_border[edge(s.x[2])]); }, "d@:Q");
    k.method(w, "setContentBorderThickness:forEdge:", [edge](CpuState& s, GuestMemory&) {
        kit().window_state(s.x[0])->content_border[edge(s.x[2])] = d(s, 0);
    }, "v@:dQ");
    k.method(w, "autorecalculatesContentBorderThicknessForEdge:", [edge](CpuState& s, GuestMemory&) {
        return_bool(s, kit().window_state(s.x[0])->content_border_automatic[edge(s.x[2])]);
    }, "B@:Q");
    k.method(w, "setAutorecalculatesContentBorderThickness:forEdge:", [edge](CpuState& s, GuestMemory&) {
        kit().window_state(s.x[0])->content_border_automatic[edge(s.x[3])] = bool_arg(s, 2);
    }, "v@:BQ");
    // Views draw on the main thread; concurrent drawing is a permission.
    k.method(w, "allowsConcurrentViewDrawing", [](CpuState& s, GuestMemory&) { return_bool(s, kit().window_state(s.x[0])->concurrent_drawing); }, "B@:");
    k.method(w, "setAllowsConcurrentViewDrawing:", [](CpuState& s, GuestMemory&) { kit().window_state(s.x[0])->concurrent_drawing = bool_arg(s, 2); }, "v@:B");
    // NSBackingStoreRetained / Nonretained are treated as buffered (macOS 10.14+).
    k.method(w, "backingType", [](CpuState& s, GuestMemory&) { s.x[0] = 2; }, "Q@:");
    k.method(w, "setBackingType:", [](CpuState& s, GuestMemory&) {
        if (s.x[2] > 2) completion_kit::fail("NSBackingStoreType " + std::to_string(s.x[2]));
    }, "v@:Q");
    k.method(w, "contentViewController", [](CpuState& s, GuestMemory&) { s.x[0] = kit().window_state(s.x[0])->content_view_controller; });
    k.method(w, "setContentViewController:", [](CpuState& s, GuestMemory&) {
        // The window shows the controller's view at that view's size.
        auto& kk = kit();
        const auto self = s.x[0], controller = s.x[2];
        const auto state = kk.window_state(self);
        if (state->content_view_controller == controller) return;
        kk.assign(state->content_view_controller, controller);
        if (!controller) return;
        const auto view = kk.send(controller, "view").x0;
        if (!view) completion_kit::fail("-[NSWindow setContentViewController:] with a controller that has no view");
        const auto size = kk.view_state(view)->frame.size;
        if (size.width > 0 && size.height > 0) {
            CallRegs registers;
            put_size(registers, 0, size);
            static_cast<void>(kk.send(self, "setContentSize:", registers));
        }
        static_cast<void>(kk.send_id(self, "setContentView:", {view}));
    }, "v@:@");
    // -nextEventMatchingMask: of a window: the application's queue, waiting
    // in the event-tracking mode (mouse tracking loops).
    k.method(w, "nextEventMatchingMask:", [](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        const auto future = kk.send(kk.objc.find_class("NSDate"), "distantFuture").x0;
        s.x[0] = kk.send_id(kk.send(kk.application, "sharedApplication").x0, "nextEventMatchingMask:untilDate:inMode:dequeue:",
                            {s.x[2], future, kk.auto_string(event_tracking_mode), 1});
    }, "@@:Q");
    k.method(w, "nextEventMatchingMask:untilDate:inMode:dequeue:", [](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        s.x[0] = kk.send_id(kk.send(kk.application, "sharedApplication").x0, "nextEventMatchingMask:untilDate:inMode:dequeue:",
                            {s.x[2], s.x[3], s.x[4], s.x[5]});
    }, "@@:Q@@B");

    // ---- NSView ----
    k.method(v, "addToolTipRect:owner:userData:", [](CpuState& s, GuestMemory&) {
        const auto tag = next_tag++;
        kit().view_state(s.x[0])->tool_tips.push_back({tag, rect_arg(s), s.x[2], s.x[3]});
        s.x[0] = static_cast<Address>(tag);
    }, "q@:{CGRect={CGPoint=dd}{CGSize=dd}}@^v");
    k.method(v, "removeToolTip:", [](CpuState& s, GuestMemory&) {
        auto& list = kit().view_state(s.x[0])->tool_tips;
        const auto tag = static_cast<std::int64_t>(s.x[2]);
        list.erase(std::remove_if(list.begin(), list.end(), [tag](const auto& t) { return t.tag == tag; }), list.end());
    }, "v@:q");
    // The view's tool tip (Qt's providers may have added it first).
    if (!k.objc.responds(v, k.sel("setToolTip:"))) {
        k.method(v, "toolTip", [](CpuState& s, GuestMemory&) { s.x[0] = kit().view_state(s.x[0])->tool_tip; });
        k.method(v, "setToolTip:", [](CpuState& s, GuestMemory&) {
            auto& kk = kit();
            kk.release(std::exchange(kk.view_state(s.x[0])->tool_tip, s.x[2] ? kk.cf.copy(s.x[2]) : 0));
        }, "v@:@");
    }
    k.method(v, "removeAllToolTips", [](CpuState& s, GuestMemory&) { kit().view_state(s.x[0])->tool_tips.clear(); }, "v@:");
    // Old-style tracking rectangles: an NSTrackingArea (entered / exited,
    // active in the active application) whose tag is returned.
    k.method(v, "addTrackingRect:owner:userData:assumeInside:", [](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        const auto self = s.x[0];
        const auto rect = rect_arg(s);
        CallRegs init;
        put_rect(init, 0, rect);
        init.x[2] = 0x01 | 0x40;   // NSTrackingMouseEnteredAndExited | NSTrackingActiveInActiveApp
        init.x[3] = s.x[2];
        init.x[4] = 0;
        const auto area = kk.send(kk.send(kk.tracking_area, "alloc").x0, "initWithRect:options:owner:userInfo:", init).x0;
        static_cast<void>(kk.send_id(self, "addTrackingArea:", {area}));
        if (bool_arg(s, 4)) kk.view_state(self)->inside_areas.push_back(area);
        const auto tag = next_tag++;
        kk.view_state(self)->tracking_rects[tag] = area;   // the view's tracking-area list holds it
        kk.release(area);
        s.x[0] = static_cast<Address>(tag);
    }, "q@:{CGRect={CGPoint=dd}{CGSize=dd}}@^vB");
    k.method(v, "removeTrackingRect:", [](CpuState& s, GuestMemory&) {
        auto& rects = kit().view_state(s.x[0])->tracking_rects;
        const auto found = rects.find(static_cast<std::int64_t>(s.x[2]));
        if (found == rects.end()) return;
        const auto area = found->second;
        rects.erase(found);
        static_cast<void>(kit().send_id(s.x[0], "removeTrackingArea:", {area}));
    }, "v@:q");
    // Whether `rect` intersects what the current display pass draws of the
    // view (its visible part).
    k.method(v, "needsToDrawRect:", [](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        const auto rect = rect_arg(s);
        const auto visible = kk.visible_rect(s.x[0]);
        return_bool(s, rect.origin.x < visible.origin.x + visible.size.width && visible.origin.x < rect.origin.x + rect.size.width &&
                           rect.origin.y < visible.origin.y + visible.size.height && visible.origin.y < rect.origin.y + rect.size.height);
    }, "B@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    k.method(v, "getRectsBeingDrawn:count:", [](CpuState& s, GuestMemory& m) {
        // One rectangle: the visible part being drawn.
        static Address storage{};
        auto& kk = kit();
        if (!storage) storage = kk.process.allocate_guest(32);
        const auto visible = kk.visible_rect(s.x[0]);
        const std::array<double, 4> values{visible.origin.x, visible.origin.y, visible.size.width, visible.size.height};
        for (std::size_t i = 0; i < 4; ++i) m.write64(storage + 8 * i, std::bit_cast<std::uint64_t>(values[i]));
        if (s.x[2]) m.write64(s.x[2], storage);
        if (s.x[3]) m.write64(s.x[3], 1);
    }, "v@:^^{CGRect}^q");
    // NSMouseInRect: a point on the top (flipped) or bottom edge belongs to the rectangle.
    k.method(v, "mouse:inRect:", [](CpuState& s, GuestMemory&) {
        const auto point = point_arg(s, 0);
        const Rect rect{{d(s, 2), d(s, 3)}, {d(s, 4), d(s, 5)}};
        const bool flipped = kit().send_bool(s.x[0], "isFlipped");
        const bool x = point.x >= rect.origin.x && point.x < rect.origin.x + rect.size.width;
        const bool y = flipped ? point.y >= rect.origin.y && point.y < rect.origin.y + rect.size.height
                               : point.y > rect.origin.y && point.y <= rect.origin.y + rect.size.height;
        return_bool(s, x && y);
    }, "B@:{CGPoint=dd}{CGRect={CGPoint=dd}{CGSize=dd}}");
    k.method(v, "replaceSubview:with:", [](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        const auto self = s.x[0], old_view = s.x[2], new_view = s.x[3];
        const auto& list = kk.view_state(self)->subviews;
        const auto found = std::find(list.begin(), list.end(), old_view);
        if (found == list.end()) completion_kit::fail("-[NSView replaceSubview:with:]: not a subview");
        if (old_view == new_view) return;
        const auto index = static_cast<std::size_t>(found - list.begin());
        const auto keep = kk.retain(old_view);
        static_cast<void>(kk.send(old_view, "removeFromSuperview"));
        if (new_view) {
            const auto& now = kk.view_state(self)->subviews;
            const auto relative = index < now.size() ? now[index] : 0;
            CallRegs registers;
            registers.x[2] = new_view;
            registers.x[3] = static_cast<Address>(-1);   // NSWindowBelow
            registers.x[4] = relative;
            static_cast<void>(kk.send(self, relative ? "addSubview:positioned:relativeTo:" : "addSubview:", registers));
        }
        kk.release(keep);
    }, "v@:@@");
    k.method(v, "setSubviews:", [](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        const auto self = s.x[0];
        const auto wanted = kk.items(s.x[2]);
        const std::set<Address> keep(wanted.begin(), wanted.end());
        for (const auto child : std::vector<Address>(kk.view_state(self)->subviews))
            if (!keep.contains(child)) static_cast<void>(kk.send(child, "removeFromSuperview"));
        for (const auto child : wanted)
            if (kk.view_state(child)->superview != self) static_cast<void>(kk.send_id(self, "addSubview:", {child}));
        // The order of the array (back to front).
        auto& list = kk.view_state(self)->subviews;
        std::stable_sort(list.begin(), list.end(), [&](Address a, Address b) {
            return std::find(wanted.begin(), wanted.end(), a) < std::find(wanted.begin(), wanted.end(), b);
        });
        kk.invalidate_view(self);
    }, "v@:@");
    k.method(v, "removeFromSuperviewWithoutNeedingDisplay", [](CpuState& s, GuestMemory&) {
        static_cast<void>(kit().send(s.x[0], "removeFromSuperview"));
    }, "v@:");
    k.method(v, "canDrawConcurrently", [](CpuState& s, GuestMemory&) { return_bool(s, kit().view_state(s.x[0])->draws_concurrently); }, "B@:");
    k.method(v, "setCanDrawConcurrently:", [](CpuState& s, GuestMemory&) { kit().view_state(s.x[0])->draws_concurrently = bool_arg(s, 2); }, "v@:B");
    k.method(v, "clipsToBounds", [](CpuState& s, GuestMemory&) { return_bool(s, kit().view_state(s.x[0])->clips_to_bounds); }, "B@:");
    k.method(v, "setClipsToBounds:", [](CpuState& s, GuestMemory&) {
        kit().view_state(s.x[0])->clips_to_bounds = bool_arg(s, 2);
        kit().invalidate_view(s.x[0]);
    }, "v@:B");
    k.method(v, "shadow", [](CpuState& s, GuestMemory&) { s.x[0] = kit().view_state(s.x[0])->shadow; });
    k.method(v, "setShadow:", [](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        const auto copy = s.x[2] ? kk.send(s.x[2], "copy").x0 : 0;
        kk.release(std::exchange(kk.view_state(s.x[0])->shadow, copy));
        kk.invalidate_view(s.x[0]);
    }, "v@:@");

    // ---- accessibility overrides (nib accessibility connectors) ----
    const auto object_class = k.objc.find_class("NSObject");
    k.method(object_class, "accessibilitySetOverrideValue:forAttribute:", [](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        if (!s.x[3]) completion_kit::fail("-accessibilitySetOverrideValue:forAttribute: without an attribute");
        const auto table = overrides_of(s.x[0], true);
        if (s.x[2]) static_cast<void>(kk.send_id(table, "setObject:forKey:", {s.x[2], s.x[3]}));
        else static_cast<void>(kk.send_id(table, "removeObjectForKey:", {s.x[3]}));
        s.x[0] = 1;
    }, "B@:@@");
    if (!k.objc.responds(object_class, k.sel("accessibilityAttributeValue:")))
        k.method(object_class, "accessibilityAttributeValue:", [](CpuState& s, GuestMemory&) {
            const auto table = overrides_of(s.x[0], false);
            s.x[0] = table ? kit().send_id(table, "objectForKey:", {s.x[2]}) : 0;
        });

    // ---- NSTableRowView: its cell views are its subviews in column order ----
    if (const auto row_view = k.objc.find_class("NSTableRowView"))
        k.method(row_view, "viewAtColumn:", [](CpuState& s, GuestMemory&) {
            const auto& list = kit().view_state(s.x[0])->subviews;
            s.x[0] = s.x[2] < list.size() ? list[s.x[2]] : 0;
        }, "@@:q");
    // ---- NSEvent: Win32 pointer input carries no NSTouch objects ----
    k.method(k.event, "touchesMatchingPhase:inView:", [](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        s.x[0] = kk.send(kk.objc.find_class("NSSet"), "set").x0;
    }, "@@:Q@");

    // ---- NSAnimationContext ----
    // The animation context of the main thread: groups nest; there is no
    // animation engine, so changes made through animator proxies apply at
    // once (animator.cpp) and a group is complete when it ends: its
    // completion handler runs then. (Installed here, before the fill-only
    // framework gaps, so this class is complete in one place.)
    {
        struct AnimationContextState {
            Address context{};
            double duration{0.25};
            bool implicit{};
            Address timing{};                  // +1
            std::vector<Address> completions;  // one per open group: a copied block or 0
        };
        static AnimationContextState animation;
        animation = {};
        const auto context = k.declare_class("NSAnimationContext", "NSObject");
        const auto current = [context] {
            if (!animation.context) {
                animation.context = kit().objc.allocate(context);
                if (const auto record = kit().process.objects().find(animation.context)) record->immortal = true;
            }
            return animation.context;
        };
        const auto end_group = [] {
            if (animation.completions.empty()) return;
            const auto completion = animation.completions.back();
            animation.completions.pop_back();
            if (!completion) return;
            try { static_cast<void>(kit().invoke_block(completion, {})); } catch (...) { block_release(completion); throw; }
            block_release(completion);
        };
        k.class_method(context, "currentContext", [current](CpuState& s, GuestMemory&) { s.x[0] = current(); });
        k.class_method(context, "beginGrouping", [](CpuState&, GuestMemory&) { animation.completions.push_back(0); }, "v@:");
        k.class_method(context, "endGrouping", [end_group](CpuState&, GuestMemory&) { end_group(); }, "v@:");
        k.class_method(context, "runAnimationGroup:completionHandler:", [current, end_group](CpuState& s, GuestMemory&) {
            animation.completions.push_back(block_copy(s.x[3]));
            if (s.x[2]) static_cast<void>(kit().invoke_block(s.x[2], {current()}));
            end_group();
        }, "v@:@?@?");
        k.class_method(context, "runAnimationGroup:", [current](CpuState& s, GuestMemory&) {
            animation.completions.push_back(0);
            if (s.x[2]) static_cast<void>(kit().invoke_block(s.x[2], {current()}));
            if (!animation.completions.empty()) animation.completions.pop_back();
        }, "v@:@?");
        k.method(context, "groupingLevel", [](CpuState& s, GuestMemory&) { s.x[0] = animation.completions.size(); }, "q@:");
        k.method(context, "duration", [](CpuState& s, GuestMemory&) { set_d(s, 0, animation.duration); }, "d@:");
        k.method(context, "setDuration:", [](CpuState& s, GuestMemory&) { animation.duration = std::max(0.0, d(s, 0)); }, "v@:d");
        k.method(context, "allowsImplicitAnimation", [](CpuState& s, GuestMemory&) { return_bool(s, animation.implicit); }, "B@:");
        k.method(context, "setAllowsImplicitAnimation:", [](CpuState& s, GuestMemory&) { animation.implicit = bool_arg(s, 2); }, "v@:B");
        k.method(context, "timingFunction", [](CpuState& s, GuestMemory&) { s.x[0] = animation.timing; });
        k.method(context, "setTimingFunction:", [](CpuState& s, GuestMemory&) { kit().assign(animation.timing, s.x[2]); }, "v@:@");
        k.method(context, "completionHandler", [](CpuState& s, GuestMemory&) {
            s.x[0] = animation.completions.empty() ? 0 : animation.completions.back();
        }, "@?@:");
        k.method(context, "setCompletionHandler:", [](CpuState& s, GuestMemory&) {
            const auto block = block_copy(s.x[2]);
            if (animation.completions.empty()) {
                // Outside a group the "animation" is already complete.
                if (block) {
                    try { static_cast<void>(kit().invoke_block(block, {})); } catch (...) { block_release(block); throw; }
                    block_release(block);
                }
                return;
            }
            block_release(std::exchange(animation.completions.back(), block));
        }, "v@:@?");
    }

    // ---- NSApplication ----
    k.method(k.application, "orderFrontColorPanel:", [](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        const auto panel = kk.send(kk.objc.find_class("NSColorPanel"), "sharedColorPanel").x0;
        static_cast<void>(kk.send_id(panel, "orderFront:", {s.x[2]}));
    }, "v@:@");
    k.method(k.application, "orderFrontCharacterPalette:", [](CpuState&, GuestMemory&) {
        completion_kit::unsupported("-[NSApplication orderFrontCharacterPalette:] (the Windows emoji panel is opened with Win+.)");
    }, "v@:@");

    // ---- constants and functions ----
    for (const auto& [symbol, value] : std::initializer_list<std::pair<const char*, const char*>>{
             {"_NSAccessibilityOutlineRowSubrole", "AXOutlineRow"},
             {"_NSCollectionElementKindSectionHeader", "UICollectionElementKindSectionHeader"},
             {"_NSCollectionElementKindSectionFooter", "UICollectionElementKindSectionFooter"},
             {"_NSImageNameComputer", "NSComputer"}})
        if (!k.process.resolve(appkit::appkit_library, symbol)) k.string_constant(symbol, value);
    // No assistive application is connected under Windows, so a posted
    // accessibility notification has no observer.
    k.function(appkit::appkit_library, "_NSAccessibilityPostNotificationWithUserInfo", [](CpuState&, GuestMemory&) {});
    // void NSDrawThreePartImage(NSRect frame, NSImage *start, NSImage *fill,
    //   NSImage *end, BOOL vertical, NSCompositingOperation op, CGFloat alpha, BOOL flipped)
    k.function(appkit::appkit_library, "_NSDrawThreePartImage", [](CpuState& s, GuestMemory&) {
        draw_three_part({{d(s, 0), d(s, 1)}, {d(s, 2), d(s, 3)}}, s.x[0], s.x[1], s.x[2], bool_arg(s, 3), s.x[4], d(s, 4), bool_arg(s, 5));
    });
    static_cast<void>(support);
}

} // namespace alchemy::gui::completion_kit::windows
