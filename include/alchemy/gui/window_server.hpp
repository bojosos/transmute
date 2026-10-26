#pragma once

// Alchemy window server: the Win32 side of AppKit (the winemac.drv analog,
// inverted). Pure C++; no Objective-C types, no guest addresses except the
// opaque `owner` tags. Implemented by src/alchemy/windowserver (A3), used by
// AppKit (src/alchemy/appkit, A4), QuartzCore presentation
// (src/alchemy/quartzcore, A7) and the CoreGraphics/Carbon display and
// keyboard functions.
//
// ---- Coordinates ----------------------------------------------------------
// All geometry is in AppKit conventions, in points:
// - Screen space: origin at the bottom-left corner of the primary screen
//   (screens()[0]), y grows upward, one global point space spanning every
//   monitor. Points are Win32 physical pixels divided by the monitor's
//   backing scale (per-monitor DPI v2: scale = dpi / 96).
// - Window content space: origin at the bottom-left of the window's content
//   area (excluding the title bar), y up, points. Event locations of window
//   events use this space (NSEvent locationInWindow).
// - Pixel sizes (drawable/backing sizes) are physical pixels.
// The window server performs every Win32 <-> AppKit conversion; callers never
// see Win32 top-left coordinates.
//
// ---- Threading -------------------------------------------------------------
// Every member function must be called on the process main thread (the guest
// main thread, which owns every HWND), except those marked "any thread".
// Sink callbacks run on the main thread, synchronously, from inside
// dispatch_messages() (which runs DispatchMessage). A C++ exception thrown by
// a sink (including the fx engine's unwind of a stopping thread) is caught in
// the WndProc, the message completes with a default result, and the
// exception is rethrown from dispatch_messages() after DispatchMessage
// returns; nothing propagates through Win32 frames.
//
// ---- Lifetime --------------------------------------------------------------
// The window server is created by install_windowserver and lives until the
// process ends. WindowIds are never reused. A destroyed window's sink is
// released after its last callback; calls naming a destroyed or unknown
// window fail with std::runtime_error. Windows still open at process exit are
// destroyed by the window server.
//
// ---- Failure ---------------------------------------------------------------
// Operations Win32 cannot perform throw std::runtime_error naming the
// operation ("Alchemy window server: ..."). Queries that have a natural empty
// answer (no clipboard text) return std::nullopt instead.

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy::gui {

using WindowId = std::uint64_t;   // 0 = no window
using CursorId = std::uint64_t;   // 0 = no cursor
using StatusItemId = std::uint64_t;   // 0 = no status item
using ScreenId = std::uint32_t;   // CGDirectDisplayID; 0 = none

struct Point { double x{}, y{}; };
struct Size { double width{}, height{}; };
struct Rect { Point origin; Size size; };

// Straight (non-premultiplied) RGBA8, rows top to bottom, no padding.
struct Image {
    std::uint32_t width{}, height{};
    std::vector<std::uint8_t> rgba;   // width * height * 4 bytes
};

// ---- windows ---------------------------------------------------------------
// Style bits use NSWindowStyleMask values so AppKit passes them through.
namespace style {
inline constexpr std::uint64_t borderless = 0;
inline constexpr std::uint64_t titled = 1ULL << 0;
inline constexpr std::uint64_t closable = 1ULL << 1;
inline constexpr std::uint64_t miniaturizable = 1ULL << 2;
inline constexpr std::uint64_t resizable = 1ULL << 3;
inline constexpr std::uint64_t utility_window = 1ULL << 4;
inline constexpr std::uint64_t full_screen = 1ULL << 14;
inline constexpr std::uint64_t full_size_content_view = 1ULL << 15;
} // namespace style

// NSWindowLevel values (normal 0, floating 3, modal panel 8, pop-up menu 101).
using WindowLevel = std::int64_t;

struct WindowDesc {
    Rect content_rect;               // screen points
    std::uint64_t style_mask{style::titled | style::closable | style::miniaturizable | style::resizable};
    WindowLevel level{0};
    std::string title;               // UTF-8
    std::uint64_t owner{};           // opaque tag (the guest NSWindow), echoed in events, for diagnostics
    WindowId parent{};               // child/owned window (NSWindow addChildWindow:, panels); 0 = top level
    bool visible{false};             // shown at creation (orderFront:) or created hidden
    bool accepts_mouse_moved{false}; // NSWindow acceptsMouseMovedEvents
    bool opaque{true};
};

enum class ShowCommand : std::uint32_t {
    order_front,                     // show, keep focus where it is
    make_key_and_order_front,        // show and give keyboard focus
    order_back,
    order_out,                       // hide
};

// ---- events ----------------------------------------------------------------
// Input events carry NSEventType numbers so AppKit can build NSEvents
// directly; window notifications become NSWindow/NSApplication delegate calls
// and notifications.
namespace event_type {   // NSEventType
inline constexpr std::uint32_t left_mouse_down = 1, left_mouse_up = 2, right_mouse_down = 3, right_mouse_up = 4,
    mouse_moved = 5, left_mouse_dragged = 6, right_mouse_dragged = 7, mouse_entered = 8, mouse_exited = 9,
    key_down = 10, key_up = 11, flags_changed = 12, app_kit_defined = 13, system_defined = 14,
    application_defined = 15, periodic = 16, cursor_update = 17, rotate = 18, begin_gesture = 19,
    end_gesture = 20, scroll_wheel = 22, tablet_point = 23, tablet_proximity = 24, other_mouse_down = 25,
    other_mouse_up = 26, other_mouse_dragged = 27, gesture = 29, magnify = 30, swipe = 31,
    smart_magnify = 32, pressure = 34;
} // namespace event_type

namespace modifier {     // NSEventModifierFlags
inline constexpr std::uint64_t caps_lock = 1ULL << 16, shift = 1ULL << 17, control = 1ULL << 18,
    option = 1ULL << 19, command = 1ULL << 20, numeric_pad = 1ULL << 21, help = 1ULL << 22,
    function = 1ULL << 23;
// Windows key -> command, Alt -> option, Ctrl -> control (A3 owns the mapping
// table and may make it configurable; this is the default).
} // namespace modifier

namespace phase {        // NSEventPhase
inline constexpr std::uint64_t none = 0, began = 1, stationary = 2, changed = 4, ended = 8, cancelled = 16,
    may_begin = 32;
} // namespace phase

namespace pointing_device {   // NSPointingDeviceType
inline constexpr std::uint64_t unknown = 0, pen = 1, cursor = 2, eraser = 3;
} // namespace pointing_device

enum class EventKind : std::uint32_t {
    input,           // an NSEvent (type is an event_type value)
    notification,    // a window or application state change (notification is set)
    text,            // IME / text input (text members are set)
};

enum class Notification : std::uint32_t {
    none,
    close_requested,      // title-bar close or Alt+F4: windowShouldClose: then close; the window stays until destroyed
    resized,              // content size changed; rect = new content rect (screen points)
    moved,                // rect = new content rect (screen points)
    expose,               // content needs redraw; rect = dirty rect (content points)
    became_key,           // keyboard focus gained
    resigned_key,
    miniaturized,
    deminiaturized,
    entered_full_screen,
    exited_full_screen,
    backing_changed,      // backing scale or screen changed; scale and screen are set
    occlusion_changed,    // visible = new visibility
    live_resize_started,  // WM_ENTERSIZEMOVE: Win32 runs a modal loop until live_resize_ended
    live_resize_ended,
    modal_tick,           // timer inside a Win32 modal loop (size/move, menu): run timers and redraw now
    application_activated,
    application_deactivated,
    quit_requested,       // WM_QUERYENDSESSION / last-chance close of the app: applicationShouldTerminate:
    screens_changed,      // monitors or their arrangement changed
    files_dropped,        // paths are set; location = drop point (content points)
    keyboard_layout_changed,
    // A file drag over a window that accepts file drops (draggingEntered:,
    // draggingUpdated:, draggingExited:). paths = the dragged files,
    // location / screen_location = pointer position, drag_operations = the
    // NSDragOperation mask the source allows (after modifier keys). After
    // drag_entered / drag_updated the window server asks the sink's
    // drag_operation() what it accepts. A drag ends with drag_exited (left the
    // window or cancelled) or with files_dropped (which carries the same
    // payload and is not preceded by drag_exited).
    drag_entered,
    drag_updated,
    drag_exited,
    // An application event: a status item (create_status_item) was clicked;
    // status_item, button_number (0 left, 1 right) and screen_location are set.
    status_item_clicked,
};

namespace drag_operation {   // NSDragOperation
inline constexpr std::uint64_t none = 0, copy = 1, link = 2, generic = 4, private_ = 8, move = 16, delete_ = 32,
    every = ~0ULL;
} // namespace drag_operation

struct Event {
    EventKind kind{EventKind::input};
    std::uint32_t type{};                 // event_type value when kind == input
    Notification notification{Notification::none};
    WindowId window{};                    // 0 for application-wide events
    std::uint64_t owner{};                // the window's WindowDesc::owner
    double timestamp{};                   // seconds, monotonic clock of gui::monotonic_seconds (NSEvent timestamp)

    // Pointer state.
    Point location;                       // content points of `window`, or screen points when window == 0
    Point screen_location;                // screen points
    std::int64_t click_count{};           // 1, 2, 3... for mouse down/up
    std::int64_t button_number{};         // 0 left, 1 right, 2 middle, 3+ extra buttons
    std::uint64_t pressed_buttons{};      // bit n = button n down after this event (NSEvent pressedMouseButtons)
    double delta_x{}, delta_y{}, delta_z{};   // mouse motion (points) or scroll deltas
    bool precise_scrolling{};             // scroll deltas are points (touchpad), not lines (wheel)
    std::uint64_t scroll_phase{phase::none};
    std::uint64_t momentum_phase{phase::none};
    bool direction_inverted{};            // isDirectionInvertedFromDevice
    // Tablet / pen (WM_POINTER). Mouse events from a pen carry these too.
    std::uint64_t pointing_device_type{pointing_device::unknown};
    float pressure{};                     // 0..1 (mouse buttons: 1 while down)
    Point tilt;                           // -1..1 each axis
    float rotation{};                     // degrees
    std::uint64_t device_id{};            // tablet / pointer identity (stable while in proximity)
    bool entering_proximity{};            // tablet_proximity events
    double magnification{};               // magnify gesture delta

    // Keyboard state.
    std::uint64_t modifier_flags{};       // modifier:: bits after this event
    std::uint16_t key_code{};             // macOS virtual key code (kVK_ANSI_A = 0x00, kVK_Return = 0x24, ...)
    bool is_repeat{};
    std::string characters;               // UTF-8, with modifiers applied (NSEvent characters)
    std::string characters_ignoring_modifiers;   // UTF-8, as if only Shift were considered

    // Text input (kind == text), from IMM32 or WM_CHAR outside a key binding.
    std::string text;                     // committed text (insertText:) or marked text (setMarkedText:)
    bool marked{};                        // true: composition update (marked text); false: commit
    std::uint64_t selection_location{}, selection_length{};   // within the marked text, UTF-16 units

    // Notification payloads.
    Rect rect;                            // see Notification
    double backing_scale{};               // backing_changed
    ScreenId screen{};                    // backing_changed
    bool visible{};                       // occlusion_changed
    std::vector<std::string> paths;       // files_dropped, drag_*: host paths, UTF-8 (AppKit maps them to guest paths)
    std::uint64_t drag_operations{};      // drag_*, files_dropped: drag_operation:: mask the source allows
    StatusItemId status_item{};           // status_item_clicked
};

// Receives a window's events. Implemented by AppKit, one per window. All
// callbacks run on the main thread inside dispatch_messages(); they may call
// back into the window server (except destroy_window for the window being
// reported, which must be deferred until the callback returns).
class WindowEventSink {
public:
    virtual ~WindowEventSink() = default;
    // Every input event, notification and text event for the window. AppKit
    // queues input as NSEvents (nextEventMatchingMask: returns them) and
    // handles notifications synchronously.
    virtual void handle_event(const Event& event) = 0;
    // WM_SIZING during live resize: the proposed content size in points;
    // returns the size to use (windowWillResize:toSize:, content min/max).
    virtual Size will_resize(WindowId, Size proposed) { return proposed; }
    // IME candidate placement: the caret rect in content points
    // (firstRectForCharacterRange:actualRange:), or nullopt when unknown.
    virtual std::optional<Rect> text_caret_rect(WindowId) { return std::nullopt; }
    // Right after delivering drag_entered / drag_updated: the drag_operation::
    // mask the window accepts (the draggingEntered: / draggingUpdated:
    // answer); none refuses the drop. The window server shows the first of
    // copy, link, move, generic that both sides allow.
    virtual std::uint64_t drag_operation(WindowId) { return gui::drag_operation::every; }
};

// ---- screens, cursors, keyboard -------------------------------------------
struct ScreenInfo {
    ScreenId id{};
    Rect frame;                // screen points
    Rect visible_frame;        // without the taskbar
    double backing_scale{1};
    double refresh_rate{};     // Hz, 0 when unknown
    std::uint32_t pixel_width{}, pixel_height{};
    double millimeters_width{}, millimeters_height{};
    std::string name;          // UTF-8 (localizedName)
    bool primary{};
};

enum class SystemCursor : std::uint32_t {
    arrow, ibeam, crosshair, closed_hand, open_hand, pointing_hand, resize_left, resize_right,
    resize_left_right, resize_up, resize_down, resize_up_down, disappearing_item, operation_not_allowed,
    drag_link, drag_copy, contextual_menu, ibeam_vertical, wait,
};

struct KeyboardLayout {
    std::string identifier;    // TISInputSource id, e.g. "com.apple.keylayout.US"
    std::string localized_name;
    std::string language;      // BCP 47
};

// UCKeyTranslate state: dead keys carry across calls. no_dead_keys is
// kUCKeyTranslateNoDeadKeysBit: a dead key yields its spacing character and
// leaves no state.
struct DeadKeyState {
    std::uint32_t value{};
    bool no_dead_keys{};
};

class WindowServer {
public:
    virtual ~WindowServer() = default;

    // ---- windows ---------------------------------------------------------
    [[nodiscard]] virtual WindowId create_window(const WindowDesc& description, std::shared_ptr<WindowEventSink> sink) = 0;
    virtual void destroy_window(WindowId window) = 0;
    virtual void show(WindowId window, ShowCommand command) = 0;
    // -[NSWindow orderWindow:relativeTo:] naming another window: shows
    // `window` without activating it, directly above (`above`) or below
    // `relative` in the z-order. Window servers without relative ordering
    // fail explicitly.
    virtual void order_relative(WindowId window, WindowId relative, bool above) {
        static_cast<void>(window); static_cast<void>(relative); static_cast<void>(above);
        throw std::runtime_error("Alchemy window server: relative window ordering is not supported");
    }
    // -[NSWindow addChildWindow:ordered:] / -removeChildWindow:: `window`
    // becomes a child of `parent` (0: top level again), as WindowDesc::parent
    // makes it at creation: it stays above its parent and moves with it.
    // Window servers without child windows fail explicitly.
    virtual void set_parent(WindowId window, WindowId parent) {
        static_cast<void>(window); static_cast<void>(parent);
        throw std::runtime_error("Alchemy window server: changing a window's parent is not supported");
    }
    virtual void set_title(WindowId window, std::string_view title) = 0;
    virtual void set_style(WindowId window, std::uint64_t style_mask) = 0;
    virtual void set_level(WindowId window, WindowLevel level) = 0;
    virtual void set_content_rect(WindowId window, Rect content_rect) = 0;   // screen points
    [[nodiscard]] virtual Rect content_rect(WindowId window) const = 0;
    [[nodiscard]] virtual Rect frame_rect(WindowId window) const = 0;        // including title bar
    // NSWindow frameRectForContentRect:styleMask: and the inverse, for the
    // screen the rect lies on.
    [[nodiscard]] virtual Rect frame_for_content(Rect content_rect, std::uint64_t style_mask) const = 0;
    [[nodiscard]] virtual Rect content_for_frame(Rect frame_rect, std::uint64_t style_mask) const = 0;
    virtual void set_content_limits(WindowId window, Size minimum, Size maximum) = 0;
    virtual void miniaturize(WindowId window, bool miniaturized) = 0;
    virtual void zoom(WindowId window) = 0;
    virtual void set_full_screen(WindowId window, bool full_screen) = 0;
    // -[NSWindow performWindowDragWithEvent:]: the user moves `window` with
    // the mouse button that is down, as dragging its title bar does; returns
    // when the button is released. Window servers without it fail explicitly.
    virtual void begin_window_drag(WindowId window) {
        static_cast<void>(window);
        throw std::runtime_error("Alchemy window server: moving a window with the mouse is not supported");
    }
    // -[NSWindow setSharingType:]: NSWindowSharingNone (`shared` false) keeps
    // the window's contents out of screen capture. Window servers without
    // capture exclusion fail explicitly when asked for it.
    virtual void set_capture_excluded(WindowId window, bool excluded) {
        static_cast<void>(window);
        if (excluded) throw std::runtime_error("Alchemy window server: excluding a window from screen capture is not supported");
    }
    // ---- status items (NSStatusItem) ---------------------------------------
    // An icon in the host's notification area showing `image` with
    // `tool_tip` (UTF-8); clicks reach the application sink as
    // Notification::status_item_clicked. Window servers without a
    // notification area fail explicitly.
    [[nodiscard]] virtual StatusItemId create_status_item(const Image& image, std::string_view tool_tip) {
        static_cast<void>(image); static_cast<void>(tool_tip);
        throw std::runtime_error("Alchemy window server: status items are not supported");
    }
    virtual void update_status_item(StatusItemId item, const Image& image, std::string_view tool_tip) {
        static_cast<void>(item); static_cast<void>(image); static_cast<void>(tool_tip);
        throw std::runtime_error("Alchemy window server: status items are not supported");
    }
    virtual void remove_status_item(StatusItemId item) { static_cast<void>(item); }
    virtual void set_accepts_mouse_moved(WindowId window, bool accepts) = 0;
    virtual void set_accepts_file_drops(WindowId window, bool accepts) = 0;
    virtual void invalidate(WindowId window, std::optional<Rect> content_rect = std::nullopt) = 0;   // queue an expose
    [[nodiscard]] virtual WindowId key_window() const = 0;
    [[nodiscard]] virtual bool is_visible(WindowId window) const = 0;
    // `window` was created and is not destroyed yet (any other call on a
    // destroyed window fails). Main thread. (Added for QuartzCore's layer
    // tree compositor, which outlives the windows it drew into.)
    [[nodiscard]] virtual bool window_exists(WindowId window) const = 0;
    [[nodiscard]] virtual double backing_scale(WindowId window) const = 0;
    [[nodiscard]] virtual ScreenId window_screen(WindowId window) const = 0;
    // Drawable size of the content area in physical pixels (CAMetalLayer
    // drawableSize default).
    [[nodiscard]] virtual Size content_pixel_size(WindowId window) const = 0;
    // The HWND of the content area, for VkSurfaceKHR creation
    // (vkCreateWin32SurfaceKHR). Valid until destroy_window. Any thread.
    [[nodiscard]] virtual void* native_handle(WindowId window) const = 0;

    // ---- input state (NSEvent class methods, CGEventSource) --------------
    [[nodiscard]] virtual std::uint64_t modifier_flags() const = 0;
    [[nodiscard]] virtual std::uint64_t pressed_mouse_buttons() const = 0;
    [[nodiscard]] virtual Point mouse_location() const = 0;   // screen points
    [[nodiscard]] virtual double double_click_interval() const = 0;   // seconds
    [[nodiscard]] virtual double key_repeat_delay() const = 0;
    [[nodiscard]] virtual double key_repeat_interval() const = 0;

    // ---- cursors ---------------------------------------------------------
    [[nodiscard]] virtual CursorId system_cursor(SystemCursor cursor) = 0;
    [[nodiscard]] virtual CursorId create_cursor(const Image& image, Point hot_spot) = 0;   // hot spot in image points
    virtual void destroy_cursor(CursorId cursor) = 0;
    virtual void set_cursor(CursorId cursor) = 0;
    virtual void set_cursor_visible(bool visible) = 0;          // [NSCursor hide] / unhide, counted like AppKit
    virtual void warp_cursor(Point screen_location) = 0;        // CGWarpMouseCursorPosition
    // CGAssociateMouseAndMouseCursorPosition(false): relative mouse mode, the
    // cursor stays put and mouse events carry deltas only.
    virtual void associate_mouse_and_cursor(bool associated) = 0;
    // Confines the cursor to a content rect of `window`; nullopt releases it.
    virtual void confine_cursor(WindowId window, std::optional<Rect> content_rect) = 0;

    // ---- clipboard (NSPasteboard generalPasteboard) ----------------------
    [[nodiscard]] virtual std::optional<std::string> clipboard_text() const = 0;   // UTF-8
    virtual void set_clipboard_text(std::string_view text) = 0;
    [[nodiscard]] virtual std::optional<Image> clipboard_image() const = 0;
    virtual void set_clipboard_image(const Image& image) = 0;
    [[nodiscard]] virtual std::int64_t clipboard_change_count() const = 0;   // NSPasteboard changeCount

    // ---- screens (NSScreen, CGDisplay*) ----------------------------------
    // [0] is the primary screen (its frame origin is 0,0). Any thread.
    [[nodiscard]] virtual std::vector<ScreenInfo> screens() const = 0;

    // ---- keyboard and text input -----------------------------------------
    [[nodiscard]] virtual KeyboardLayout keyboard_layout() const = 0;   // TISCopyCurrentKeyboardInputSource
    // UCKeyTranslate for the current layout: the UTF-8 produced by mac key
    // code `key_code` with modifier:: flags, updating dead-key state.
    [[nodiscard]] virtual std::string translate_key(std::uint16_t key_code, std::uint64_t modifier_flags,
                                                    DeadKeyState& state) const = 0;
    // NSTextInputContext activate/deactivate: IME composition for `window`.
    virtual void set_text_input_enabled(WindowId window, bool enabled) = 0;
    virtual void discard_marked_text(WindowId window) = 0;

    // ---- message pump hook -----------------------------------------------
    // PeekMessage/DispatchMessage for every queued Win32 message of the main
    // thread, delivering sink callbacks; rethrows a sink exception (see
    // Threading). Returns true when at least one message was dispatched.
    // Called by the run loop's main pump; never blocks.
    virtual bool dispatch_messages() = 0;

    // ---- application events ----------------------------------------------
    // Receives the application-wide events (Event::window == 0):
    // application_activated / application_deactivated, quit_requested,
    // screens_changed and keyboard_layout_changed, with the same threading
    // and exception rules as window sinks. Set by AppKit (NSApplication);
    // nullptr detaches. Without a sink those events are dropped.
    virtual void set_application_sink(std::shared_ptr<WindowEventSink> sink) {
        static_cast<void>(sink);
        throw std::runtime_error("Alchemy window server: application events are not supported");
    }

    // ---- software presentation (non-layer-backed NSView drawing) ---------
    // Shows a window backing store drawn by AppKit (-drawRect: of views that
    // are not layer-hosting) in `window`'s content area. Only the part inside
    // `content_rect` (content points; nullopt: the whole content area) is
    // updated; pixel (0,0) of `pixels` is the top-left corner of the content
    // area and the bitmap covers it at the window's backing scale (it is
    // stretched when the sizes differ, e.g. during a live resize). The pixels
    // are copied (or shown) before the call returns. Main thread. Window
    // servers without software presentation fail explicitly.
    struct BitmapView {
        std::uint32_t width{}, height{};       // physical pixels
        std::size_t stride{};                  // bytes per row
        const std::uint8_t* bgra{};            // BGRA8, premultiplied alpha, rows top to bottom
    };
    virtual void present_bitmap(WindowId window, const BitmapView& pixels, std::optional<Rect> content_rect = std::nullopt) {
        static_cast<void>(window); static_cast<void>(pixels); static_cast<void>(content_rect);
        throw std::runtime_error("Alchemy window server: software presentation (present_bitmap) is not supported");
    }

    // ---- misc --------------------------------------------------------------
    virtual void beep() = 0;   // NSBeep
    virtual void request_attention(bool critical) = 0;   // requestUserAttention: (FlashWindowEx)
};

// The process's window server. Throws std::runtime_error("Alchemy window
// server is not available") until install_windowserver created one (or when
// the host has no interactive desktop). Any thread.
[[nodiscard]] WindowServer& window_server();
[[nodiscard]] bool window_server_available() noexcept;

} // namespace alchemy::gui
