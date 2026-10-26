#include "alchemy/darwin/appkit/appkit_internal.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

// NSWindow's frame view: AppKit's private NSFrameView classes (NSFrameView :
// NSView, NSTitledFrame : NSFrameView, NSThemeFrame : NSTitledFrame,
// NSNextStepFrame : NSFrameView) and the standard window buttons
// (_NSThemeWidget : NSButton). Applications subclass them (Chrome's
// NativeWidgetMacNSWindow returns its own NSThemeFrame / NSNextStepFrame
// subclasses), so each is a real, subclassable host class.
// - The frame view is the root of the window's view tree: the content view's
//   superview. Its class is +frameViewClassForStyleMask:, sent to the
//   window's class (a subclass may override it): NSThemeFrame for titled
//   windows, NSNextStepFrame otherwise. -setStyleMask: replaces it when the
//   class for the new mask differs and moves the content view over.
// - Geometry: the frame view's frame is {0, 0} and the window's size in
//   AppKit's terms, the content size plus the title bar (-_titlebarHeight,
//   sent so an override counts) unless the content view is full size
//   (NSWindowStyleMaskFullSizeContentView), where it fills the frame view.
//   The content view sits at the origin, so frame-view coordinates are the
//   window base coordinates. Win32 draws the real border and caption: the
//   frame view draws nothing and takes no events (both start at the content
//   view), and it follows the window's size directly, like AppKit's.
// - Titled windows get the close, miniaturize and zoom buttons, enabled by
//   the style (closable, miniaturizable, resizable), whose actions are the
//   window's -performClose:, -performMiniaturize: and -performZoom:. They
//   are laid out as AppKit's title bar lays them out: 14 x 16 points, 20
//   points apart from -_minXTitlebarWidgetInset (7), 6 points below the top
//   of the frame, or centred in -_titlebarHeight with
//   -_shouldCenterTrafficLights, mirrored to the right edge when
//   -windowTitlebarLayoutDirection is right-to-left and
//   -_shouldFlipTrafficLightsForRTL. The 28-point title bar and these button
//   metrics are macOS 11+'s as observed, not public-header values.
// - -[NSWindow contentLayoutRect]: the content view's bounds, less the title
//   bar for a full-size content view.
// - -[_NSThemeWidget revealAmount] (0..1, how far a full-screen title bar
//   shows the button; Chrome sets it in immersive full screen) and
//   -[NSFrameView _inFullScreen] are kept / answered from the window.
namespace alchemy::gui::appkit {
namespace {
constexpr double standard_titlebar_height = 28;   // unsure: observed macOS 11+ value, no header constant
constexpr double button_width = 14, button_height = 16, button_spacing = 20, button_inset = 7, button_top = 6;   // unsure: observed
constexpr std::int64_t right_to_left = 1;   // NSUserInterfaceLayoutDirectionRightToLeft
constexpr std::array<const char*, 3> button_actions{"performClose:", "performMiniaturize:", "performZoom:"};
constexpr std::array<std::uint64_t, 3> button_styles{style::closable, style::miniaturizable, style::resizable};

struct FrameViewState final : ObjectPayload {
    Address window{};                          // not retained: the window holds the frame view
    std::array<Address, 3> buttons{};          // by NSWindowButton; held by the frame view's subviews
    // -[_NSThemeWidget revealAmount] of each button: how far a full-screen
    // title bar shows it (0 hidden .. 1 shown).
    std::array<double, 3> reveal{1, 1, 1};
};
} // namespace

// The frame view adopts `child` as its first subview (below the title-bar
// buttons) without moving it to another window.
static void adopt_subview(AppKit& kit, Address frame, Address child) {
    const auto info = kit.view_state(child);
    if (info->superview == frame) return;
    if (const auto parent = info->superview) {
        auto& list = kit.view_state(parent)->subviews;
        list.erase(std::remove(list.begin(), list.end(), child), list.end());
        kit.release(child);
    }
    auto& list = kit.view_state(frame)->subviews;
    list.insert(list.begin(), kit.retain(child));
    info->superview = frame;
}

void AppKit::detach_from_frame_view(Address target, Address child) {
    const auto frame = window_state(target)->frame_view;
    if (!frame || !child || view_state(child)->superview != frame) return;
    auto& list = view_state(frame)->subviews;
    list.erase(std::remove(list.begin(), list.end(), child), list.end());
    view_state(child)->superview = 0;
    release(child);
}

void AppKit::attach_to_frame_view(Address target, Address child) {
    if (const auto frame = window_state(target)->frame_view) adopt_subview(*this, frame, child);
}

void AppKit::update_frame_view(Address target) {
    const auto state = window_state(target);
    CallRegs mask;
    mask.x[2] = state->style;
    const auto chosen = send(objc.object_class(target), "frameViewClassForStyleMask:", mask).x0;
    if (!chosen || !(send_id(chosen, "isSubclassOfClass:", {frame_view_class_root}) & 0xff))
        fail("+frameViewClassForStyleMask: did not return an NSFrameView class");
    const auto old = state->frame_view;
    if (old && objc.object_class(old) == chosen) { layout_frame_view(target); return; }

    CallRegs frame;
    put_rect(frame, 0, {{0, 0}, content_rect(target).size});
    const auto created = send(send(chosen, "alloc").x0, "initWithFrame:", frame).x0;
    if (!created) fail("the frame view class refused -initWithFrame:");
    extension<FrameViewState>(created)->window = target;
    view_state(created)->next_responder = target;
    state->frame_view = created;
    if (state->style & style::titled) {
        const auto widget = objc.find_class("_NSThemeWidget");
        auto& buttons = extension<FrameViewState>(created)->buttons;
        for (std::size_t index = 0; index < buttons.size(); ++index) {
            CallRegs bounds;
            put_rect(bounds, 0, {{0, 0}, {button_width, button_height}});
            const auto button = send(send(widget, "alloc").x0, "initWithFrame:", bounds).x0;
            static_cast<void>(send_id(button, "setBordered:", {0}));
            static_cast<void>(send_id(button, "setTarget:", {target}));
            static_cast<void>(send_id(button, "setAction:", {sel(button_actions[index])}));
            static_cast<void>(send_id(button, "setEnabled:", {(state->style & button_styles[index]) ? 1ULL : 0ULL}));
            static_cast<void>(send_id(created, "addSubview:", {button}));
            buttons[index] = button;
            release(button);
        }
    }
    if (old) {
        if (const auto content = state->content_view) adopt_subview(*this, created, content);
        if (const auto info = find_extension<FrameViewState>(old)) { info->window = 0; info->buttons = {}; }
    } else if (const auto content = state->content_view) {
        adopt_subview(*this, created, content);
    }
    set_view_window(created, target);   // the frame view and its buttons (the content view is there already)
    layout_frame_view(target);
    if (old) {
        set_view_window(old, 0);
        release(old);
    }
}

double AppKit::titlebar_height(Address target) {
    const auto state = window_state(target);
    if (!state->frame_view || !(state->style & style::titled)) return 0;
    return std::max(0.0, send(state->frame_view, "_titlebarHeight").d(0));
}

void AppKit::layout_frame_view(Address target) {
    const auto state = window_state(target);
    const auto frame = state->frame_view;
    if (!frame) return;
    const auto content = content_rect(target).size;
    const auto titlebar = titlebar_height(target);
    const bool full_size = (state->style & style::full_size_content_view) != 0;
    const Size size{content.width, content.height + state->strip + (full_size ? 0 : titlebar)};
    view_state(frame)->frame = {{0, 0}, size};
    layout_top_strip(target);
    const auto info = find_extension<FrameViewState>(frame);
    if (!info || !info->buttons[0]) return;
    std::int64_t direction = 0;
    if (const auto answer = send_optional(target, "windowTitlebarLayoutDirection")) direction = static_cast<std::int64_t>(answer->x0);
    const bool mirrored = direction == right_to_left && send_bool(frame, "_shouldFlipTrafficLightsForRTL");
    const auto inset = send(frame, "_minXTitlebarWidgetInset").d(0);
    const auto top = send_bool(frame, "_shouldCenterTrafficLights") ? (titlebar - button_height) / 2 : button_top;
    for (std::size_t index = 0; index < info->buttons.size(); ++index) {
        auto x = inset + static_cast<double>(index) * button_spacing;
        if (mirrored) x = size.width - x - button_width;
        CallRegs registers;
        put_rect(registers, 0, {{x, size.height - top - button_height}, {button_width, button_height}});
        static_cast<void>(send(info->buttons[index], "setFrame:", registers));
    }
}

Address AppKit::standard_window_button(Address target, std::uint64_t which) {
    const auto frame = window_state(target)->frame_view;
    const auto info = frame ? find_extension<FrameViewState>(frame) : nullptr;
    return info && which < info->buttons.size() ? info->buttons[which] : 0;
}

void AppKit::install_frame_views() {
    const auto frame = declare_class("NSFrameView", "NSView");
    const auto titled = declare_class("NSTitledFrame", "NSFrameView");
    static_cast<void>(declare_class("NSThemeFrame", "NSTitledFrame"));
    static_cast<void>(declare_class("NSNextStepFrame", "NSFrameView"));
    const auto widget = declare_class("_NSThemeWidget", "NSButton");
    frame_view_class_root = frame;
    method(frame, "_titlebarHeight", [](CpuState& s, GuestMemory&) { set_d(s, 0, 0); }, "d@:");
    method(titled, "_titlebarHeight", [](CpuState& s, GuestMemory&) { set_d(s, 0, standard_titlebar_height); }, "d@:");
    method(frame, "_minXTitlebarWidgetInset", [](CpuState& s, GuestMemory&) { set_d(s, 0, button_inset); }, "d@:");
    method(frame, "_shouldCenterTrafficLights", [](CpuState& s, GuestMemory&) { return_bool(s, false); }, "B@:");
    method(frame, "_shouldFlipTrafficLightsForRTL", [](CpuState& s, GuestMemory&) { return_bool(s, true); }, "B@:");
    method(frame, "_usesCustomDrawing", [](CpuState& s, GuestMemory&) { return_bool(s, false); }, "B@:");
    // Whether the window is in full screen (its style mask's NSWindowStyleMaskFullScreen).
    method(frame, "_inFullScreen", [this](CpuState& s, GuestMemory&) {
        const auto info = find_extension<FrameViewState>(s.x[0]);
        return_bool(s, info && info->window && window_state(info->window)->full_screen);
    }, "B@:");
    // The full-screen title bar reveals its buttons gradually (Chrome sets
    // it to show them in immersive full screen). Win32 draws the caption, so
    // the amount is kept for the getter.
    // A widget belongs to a frame view, which keeps the amount.
    const auto reveal_slot = [this](Address button) -> double* {
        const auto frame_view = view_state(button)->superview;
        const auto info = frame_view ? find_extension<FrameViewState>(frame_view) : nullptr;
        if (!info) return nullptr;
        for (std::size_t index = 0; index < info->buttons.size(); ++index)
            if (info->buttons[index] == button) return &info->reveal[index];
        return nullptr;
    };
    method(widget, "revealAmount", [reveal_slot](CpuState& s, GuestMemory&) {
        const auto slot = reveal_slot(s.x[0]);
        set_d(s, 0, slot ? *slot : 1);
    }, "d@:");
    method(widget, "setRevealAmount:", [reveal_slot](CpuState& s, GuestMemory&) {
        const auto slot = reveal_slot(s.x[0]);
        if (!slot) throw std::runtime_error("unsupported: Alchemy: -[_NSThemeWidget setRevealAmount:] on a button outside a window's title bar");
        *slot = std::clamp(d(s, 0), 0.0, 1.0);
    }, "v@:d");
    // AppKit sizes and lays out its frame view; a guest call relays it out.
    method(frame, "_tileTitlebarAndRedisplay:", [this](CpuState& s, GuestMemory&) {
        if (const auto info = find_extension<FrameViewState>(s.x[0]); info && info->window) layout_frame_view(info->window);
    }, "v@:B");

    const auto w = window;
    class_method(w, "frameViewClassForStyleMask:", [this](CpuState& s, GuestMemory&) {
        s.x[0] = objc.find_class((s.x[2] & style::titled) ? "NSThemeFrame" : "NSNextStepFrame");
    }, "#@:Q");
    // NSWindowButton: close 0, miniaturize 1, zoom 2; the toolbar and
    // document buttons do not exist (nil, as for a window without them).
    method(w, "standardWindowButton:", [this](CpuState& s, GuestMemory&) { s.x[0] = standard_window_button(s.x[0], s.x[2]); }, "@@:Q");
    method(w, "contentLayoutRect", [this](CpuState& s, GuestMemory&) {
        const auto size = content_rect(s.x[0]).size;
        const bool full_size = (window_state(s.x[0])->style & style::full_size_content_view) != 0;
        const auto titlebar = full_size ? titlebar_height(s.x[0]) : 0;
        return_rect(s, {{0, 0}, {size.width, std::max(0.0, size.height - titlebar)}});
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
}

} // namespace alchemy::gui::appkit
