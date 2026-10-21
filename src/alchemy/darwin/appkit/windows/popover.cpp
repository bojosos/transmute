#include "alchemy/darwin/appkit/windows/windows_internal.hpp"

#include <algorithm>

// NSPopover (QLab gap group G4): content from a view controller shown in a
// borderless child window of the positioning view's window, placed on the
// preferred edge of the positioning rectangle (or the opposite edge when the
// screen has no room there, slid along it to stay on screen: layout_geometry
// popover_frame), the content offset from the anchor by the arrow's height.
// Behaviors: application-defined popovers close when told; transient ones
// also when a mouse button goes down outside them and when the application
// resigns active; semitransient ones on clicks in other windows of the
// application. -performClose: asks the delegate (-popoverShouldClose:); the
// will / did show and close notifications and delegate messages are sent.
// Detaching (dragging the popover into its own window) is not offered.
namespace alchemy::gui::completion_kit::windows {
namespace {
using appkit::bool_arg;
using appkit::d;
using appkit::put_rect;
using appkit::put_size;
using appkit::rect_arg;
using appkit::result_rect;
using appkit::result_size;
using appkit::return_bool;
using appkit::return_rect;
using appkit::return_size;
using appkit::size_arg;

constexpr double arrow_height = 13;
constexpr std::int64_t behavior_application_defined = 0, behavior_transient = 1, behavior_semitransient = 2;

struct PopoverState final : ObjectPayload {
    Address controller{}, appearance{};       // +1
    Address delegate{};                       // weak
    Address window{};                         // +1 while shown
    Address positioning_view{}, parent{};     // weak while shown
    Rect positioning_rect;
    Size content_size;
    std::int64_t behavior{behavior_application_defined};
    bool animates{true}, shown{}, full_size{};
};

Address popover_class{};
std::vector<Address>& shown_popovers() {
    static std::vector<Address> list;
    return list;
}

std::shared_ptr<PopoverState> popover(Address object) { return kit().objc.host_state<PopoverState>(object); }

Size content_size_of(Address object) {
    auto& k = kit();
    const auto state = popover(object);
    if (state->content_size.width > 0 && state->content_size.height > 0) return state->content_size;
    if (!state->controller) return {};
    if (k.responds(state->controller, "preferredContentSize")) {
        const auto preferred = result_size(k.send(state->controller, "preferredContentSize"));
        if (preferred.width > 0 && preferred.height > 0) return preferred;
    }
    const auto view = k.send(state->controller, "view").x0;
    return view ? k.view_state(view)->frame.size : Size{};
}

void notify(Address object, const char* name, const char* delegate_selector) {
    kit().post(name, object, popover(object)->delegate, delegate_selector);
}

void close_popover(Address object) {
    auto& k = kit();
    const auto state = popover(object);
    if (!state->shown) return;
    notify(object, "NSPopoverWillCloseNotification", "popoverWillClose:");
    if (state->controller) static_cast<void>(k.send_optional(state->controller, "viewWillDisappear"));
    const auto window = std::exchange(state->window, 0);
    state->shown = false;
    auto& list = shown_popovers();
    list.erase(std::remove(list.begin(), list.end(), object), list.end());
    if (window) {
        if (state->parent) static_cast<void>(k.send_id(state->parent, "removeChildWindow:", {window}));
        // The content view stays the controller's; the window lets it go.
        static_cast<void>(k.send_id(window, "setContentView:", {0}));
        static_cast<void>(k.send_id(window, "orderOut:", {0}));
        static_cast<void>(k.send(window, "close"));
        k.release(window);
    }
    state->parent = 0;
    state->positioning_view = 0;
    if (state->controller) static_cast<void>(k.send_optional(state->controller, "viewDidDisappear"));
    notify(object, "NSPopoverDidCloseNotification", "popoverDidClose:");
    k.release(object);   // the popover was kept alive while shown
}

void show_popover(Address object, Rect rect, Address view, std::int64_t edge) {
    auto& k = kit();
    const auto state = popover(object);
    if (!view || !k.is_kind(view, k.view)) completion_kit::fail("-[NSPopover showRelativeToRect:ofView:preferredEdge:] needs a view");
    const auto parent = k.view_state(view)->window;
    if (!parent) completion_kit::fail("-[NSPopover showRelativeToRect:ofView:preferredEdge:]: the view is not in a window");
    if (!state->controller) completion_kit::fail("NSPopover shown without a content view controller");
    if (edge < 0 || edge > 3) completion_kit::fail("NSPopover preferred edge " + std::to_string(edge));
    if (rect.size.width <= 0 && rect.size.height <= 0) rect = AppKit::view_bounds(*k.view_state(view));   // NSZeroRect: the view's bounds
    const auto content_view = k.send(state->controller, "view").x0;
    if (!content_view) completion_kit::fail("NSPopover content view controller without a view");
    if (state->shown) close_popover(object);
    // The anchor on screen.
    CallRegs convert;
    put_rect(convert, 0, rect);
    convert.x[2] = 0;
    const auto in_window = result_rect(k.send(view, "convertRect:toView:", convert));
    CallRegs to_screen;
    put_rect(to_screen, 0, in_window);
    const auto anchor = result_rect(k.send(parent, "convertRectToScreen:", to_screen));
    const auto screen = k.send(parent, "screen").x0;
    const auto visible = screen ? result_rect(k.send(screen, "visibleFrame")) : Rect{{-1e6, -1e6}, {2e6, 2e6}};
    const auto size = content_size_of(object);
    const auto placed = layout::popover_frame({anchor.origin.x, anchor.origin.y, anchor.size.width, anchor.size.height}, {size.width, size.height},
                                              static_cast<int>(edge), k.send_bool(view, "isFlipped"),
                                              {visible.origin.x, visible.origin.y, visible.size.width, visible.size.height}, arrow_height);
    // The window is the content; the arrow's strip next to the anchor stays empty.
    Rect frame{{placed.frame.x, placed.frame.y}, {placed.frame.width, placed.frame.height}};
    switch (placed.edge) {
    case 0: frame.size.width -= arrow_height; break;
    case 2: frame.origin.x += arrow_height; frame.size.width -= arrow_height; break;
    case 1: frame.size.height -= arrow_height; break;
    case 3: frame.origin.y += arrow_height; frame.size.height -= arrow_height; break;
    default: break;
    }
    notify(object, "NSPopoverWillShowNotification", "popoverWillShow:");
    static_cast<void>(k.send_optional(state->controller, "viewWillAppear"));
    CallRegs init;
    put_rect(init, 0, frame);
    init.x[2] = 0;   // NSWindowStyleMaskBorderless
    init.x[3] = 2;
    init.x[4] = 0;
    const auto window = k.send(k.send(k.window, "alloc").x0, "initWithContentRect:styleMask:backing:defer:", init).x0;
    static_cast<void>(k.send_id(window, "setReleasedWhenClosed:", {0}));
    if (state->appearance) static_cast<void>(k.send_id(window, "setAppearance:", {state->appearance}));
    static_cast<void>(k.send_id(window, "setContentView:", {content_view}));
    state->window = window;
    state->parent = parent;
    state->positioning_view = view;
    state->positioning_rect = rect;
    state->shown = true;
    k.retain(object);
    shown_popovers().push_back(object);
    static_cast<void>(k.send_id(parent, "addChildWindow:ordered:", {window, 1}));
    static_cast<void>(k.send_id(window, "makeKeyAndOrderFront:", {0}));
    static_cast<void>(k.send_optional(state->controller, "viewDidAppear"));
    notify(object, "NSPopoverDidShowNotification", "popoverDidShow:");
}

// Clicks outside: transient popovers close on any click outside them,
// semitransient ones on clicks in another window of the application.
void mouse_down(Address window) {
    for (const auto object : std::vector<Address>(shown_popovers())) {
        const auto state = popover(object);
        if (!state->shown || window == state->window) continue;
        if (state->behavior == behavior_transient || state->behavior == behavior_semitransient) close_popover(object);
    }
}
} // namespace

void install_popover(Support& support) {
    static_cast<void>(support);
    auto& k = kit();
    shown_popovers().clear();
    popover_class = k.declare_class("NSPopover", "NSResponder");
    const auto p = popover_class;
    const auto previous_hook = k.mouse_down_hook;
    k.mouse_down_hook = [previous_hook](Address window) {
        mouse_down(window);
        if (previous_hook) previous_hook(window);
    };
    k.method(p, "init", [p](CpuState& s, GuestMemory&) {
        const auto self = kit().objc.send_super(s.x[0], p, kit().sel("init")).x0;
        s.x[0] = self;
        if (self) static_cast<void>(popover(self));
    });
    k.method(p, "contentViewController", [](CpuState& s, GuestMemory&) { s.x[0] = popover(s.x[0])->controller; });
    k.method(p, "setContentViewController:", [](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        const auto state = popover(s.x[0]);
        kk.assign(state->controller, s.x[2]);
        if (state->shown && state->window && s.x[2])
            static_cast<void>(kk.send_id(state->window, "setContentView:", {kk.send(s.x[2], "view").x0}));
    }, "v@:@");
    k.method(p, "contentSize", [](CpuState& s, GuestMemory&) { return_size(s, content_size_of(s.x[0])); }, "{CGSize=dd}@:");
    k.method(p, "setContentSize:", [](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        const auto state = popover(s.x[0]);
        state->content_size = size_arg(s);
        if (state->shown && state->window) {
            CallRegs registers;
            put_size(registers, 0, state->content_size);
            static_cast<void>(kk.send(state->window, "setContentSize:", registers));
        }
    }, "v@:{CGSize=dd}");
    k.method(p, "behavior", [](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(popover(s.x[0])->behavior); }, "q@:");
    k.method(p, "setBehavior:", [](CpuState& s, GuestMemory&) {
        const auto value = static_cast<std::int64_t>(s.x[2]);
        if (value < 0 || value > 2) completion_kit::fail("NSPopoverBehavior " + std::to_string(value));
        popover(s.x[0])->behavior = value;
    }, "v@:q");
    k.method(p, "animates", [](CpuState& s, GuestMemory&) { return_bool(s, popover(s.x[0])->animates); }, "B@:");
    // Popovers appear at once (no animation engine); the flag is the request.
    k.method(p, "setAnimates:", [](CpuState& s, GuestMemory&) { popover(s.x[0])->animates = bool_arg(s, 2); }, "v@:B");
    k.method(p, "appearance", [](CpuState& s, GuestMemory&) { s.x[0] = popover(s.x[0])->appearance; });
    k.method(p, "setAppearance:", [](CpuState& s, GuestMemory&) {
        const auto state = popover(s.x[0]);
        kit().assign(state->appearance, s.x[2]);
        if (state->window) static_cast<void>(kit().send_id(state->window, "setAppearance:", {s.x[2]}));
    }, "v@:@");
    k.method(p, "effectiveAppearance", [](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        const auto state = popover(s.x[0]);
        s.x[0] = state->appearance ? state->appearance : kk.send(kk.send(kk.application, "sharedApplication").x0, "effectiveAppearance").x0;
    });
    k.method(p, "delegate", [](CpuState& s, GuestMemory&) { s.x[0] = popover(s.x[0])->delegate; });
    k.method(p, "setDelegate:", [](CpuState& s, GuestMemory&) { popover(s.x[0])->delegate = s.x[2]; }, "v@:@");
    k.method(p, "isShown", [](CpuState& s, GuestMemory&) { return_bool(s, popover(s.x[0])->shown); }, "B@:");
    k.method(p, "isDetached", [](CpuState& s, GuestMemory&) { return_bool(s, false); }, "B@:");
    k.method(p, "hasFullSizeContent", [](CpuState& s, GuestMemory&) { return_bool(s, popover(s.x[0])->full_size); }, "B@:");
    k.method(p, "setHasFullSizeContent:", [](CpuState& s, GuestMemory&) { popover(s.x[0])->full_size = bool_arg(s, 2); }, "v@:B");
    k.method(p, "positioningRect", [](CpuState& s, GuestMemory&) { return_rect(s, popover(s.x[0])->positioning_rect); }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
    k.method(p, "setPositioningRect:", [](CpuState& s, GuestMemory&) {
        const auto state = popover(s.x[0]);
        state->positioning_rect = rect_arg(s);
        if (state->shown) completion_kit::unsupported("-[NSPopover setPositioningRect:] while shown (moving a shown popover)");
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    k.method(p, "showRelativeToRect:ofView:preferredEdge:", [](CpuState& s, GuestMemory&) {
        show_popover(s.x[0], rect_arg(s), s.x[2], static_cast<std::int64_t>(s.x[3]));
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}@Q");
    k.method(p, "close", [](CpuState& s, GuestMemory&) { close_popover(s.x[0]); }, "v@:");
    k.method(p, "performClose:", [](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        const auto state = popover(s.x[0]);
        if (state->delegate && kk.responds(state->delegate, "popoverShouldClose:") && !kk.send_bool(state->delegate, "popoverShouldClose:", {s.x[0]}))
            return;
        close_popover(s.x[0]);
    }, "v@:@");
    k.method(p, "dealloc", [p](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        if (const auto state = kk.objc.find_host_state<PopoverState>(s.x[0])) {
            kk.release(std::exchange(state->controller, 0));
            kk.release(std::exchange(state->appearance, 0));
        }
        static_cast<void>(kk.objc.send_super(s.x[0], p, kk.sel("dealloc")));
    }, "v@:");
    // Archived popovers (Interface Builder): behavior, animation, content
    // size; the content view controller comes from an outlet connection.
    k.method(p, "initWithCoder:", [p](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        const auto self = kk.objc.send_super(s.x[0], p, kk.sel("init")).x0;
        s.x[0] = self;
        if (!self) return;
        const auto coder = s.x[2];
        const auto key = [&](const char* name) { return kk.auto_string(name); };
        const auto has = [&](const char* name) { return kk.send_bool(coder, "containsValueForKey:", {key(name)}); };
        const auto real = [&](const char* name) {
            CallRegs registers;
            registers.x[2] = key(name);
            return kk.send(coder, "decodeDoubleForKey:", registers).d(0);
        };
        const auto state = popover(self);
        if (has("NSBehavior")) state->behavior = static_cast<std::int64_t>(kk.send_id(coder, "decodeIntegerForKey:", {key("NSBehavior")}));
        if (has("NSAnimates")) state->animates = kk.send_bool(coder, "decodeBoolForKey:", {key("NSAnimates")});
        if (has("NSHasFullSizeContent")) state->full_size = kk.send_bool(coder, "decodeBoolForKey:", {key("NSHasFullSizeContent")});
        if (has("NSContentWidth") && has("NSContentHeight")) state->content_size = {real("NSContentWidth"), real("NSContentHeight")};
        if (const auto appearance = kk.send_id(coder, "decodeObjectForKey:", {key("NSPopoverAppearanceCustomization")}))
            kk.assign(state->appearance, appearance);
        // NSNextResponder: the responder chain follows the window it is shown in.
        static_cast<void>(has("NSNextResponder"));
    });
    // The application resigning active closes transient popovers.
    const auto previous_resign = k.resign_active_hook;
    k.resign_active_hook = [previous_resign] {
        for (const auto object : std::vector<Address>(shown_popovers()))
            if (popover(object)->behavior == behavior_transient) close_popover(object);
        if (previous_resign) previous_resign();
    };
}

} // namespace alchemy::gui::completion_kit::windows
