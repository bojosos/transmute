// Linux host backend, area "ui": the window server over X11 (XCB), the
// counterpart of the Win32 one (include/alchemy/gui/window_server.hpp).
//
// - One X connection ($DISPLAY), opened by the first request. Windows are
//   top-level X windows the window manager decorates. Screens are RandR
//   monitors (the root window without RandR). AppKit's screen points have
//   their origin at the primary monitor's bottom-left with y up, so a point
//   is X root pixels flipped against that monitor's bottom edge (backing
//   scale 1 for now).
// - Events are read in dispatch_messages and delivered to the window's sink
//   on the main thread; an exception from a sink is rethrown after the
//   batch. Expose, configure (move and resize), focus, map/unmap and the
//   WM_DELETE_WINDOW request become notifications; keys go through
//   xkbcommon (characters with and without modifiers, macOS key codes from
//   the evdev code); buttons 4-7 are wheel steps.
// - present_bitmap puts the BGRA pixels with PutImage (the X server's 24/32-bit
//   TrueColor layout is BGRX on little-endian hosts), in strips that fit the
//   maximum request size.
// - The clipboard is the X CLIPBOARD selection: a hidden window owns it
//   while this process holds the clipboard (UTF-8 text, or an image as PNG)
//   and answers other clients' requests; reading converts the selection
//   (INCR transfers too). XFixes reports other owners for the change count.
// - Cursors come from the cursor theme by name (xcb-cursor; the core cursor
//   font's names as fallbacks); images become Render ARGB cursors.
// - Not yet: HiDPI, drag and drop, IME, status items, cursor confinement
//   and relative mouse mode.
#include "alchemy/host/linux/gui/x11.hpp"
#include "alchemy/host/gpu.hpp"
#include "alchemy/host/ui.hpp"
#include "alchemy/gui/runloop.hpp"

#include <xcb/randr.h>
#include <xcb/render.h>
#include <xcb/xcb_cursor.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_renderutil.h>
#include <xcb/xfixes.h>
#include <xkbcommon/xkbcommon-x11.h>
#include <xkbcommon/xkbcommon.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>

namespace alchemy::host::linux_gui {

namespace {
xcb_connection_t* g_connection = nullptr;
std::deque<xcb_generic_event_t*>& pending() {
    static auto* queue = new std::deque<xcb_generic_event_t*>;
    return *queue;
}
} // namespace

xcb_connection_t* connection() noexcept { return g_connection; }

bool events_queued() noexcept {
    if (!g_connection) return false;
    if (!pending().empty()) return true;
    if (auto* event = xcb_poll_for_queued_event(g_connection)) {
        pending().push_back(event);
        return true;
    }
    return false;
}

} // namespace alchemy::host::linux_gui

namespace alchemy::host::ui {

namespace {

using namespace alchemy::gui;

// ---- keys -----------------------------------------------------------------------------
// macOS kVK_* codes (Carbon HIToolbox Events.h, positional) by Linux evdev
// code (X keycode - 8). The evdev codes of the main block equal the PC set-1
// scan codes the Win32 table uses.
constexpr std::uint16_t kNoKey = 0xffff;
std::uint16_t mac_key_code(std::uint32_t evdev) noexcept {
    static constexpr std::uint16_t table[] = {
        kNoKey, 0x35, 0x12, 0x13, 0x14, 0x15, 0x17, 0x16, 0x1a, 0x1c,   // 0 none, esc, 1..8
        0x19, 0x1d, 0x1b, 0x18, 0x33, 0x30, 0x0c, 0x0d, 0x0e, 0x0f,     // 9 0 - = bksp tab q w e r
        0x11, 0x10, 0x20, 0x22, 0x1f, 0x23, 0x21, 0x1e, 0x24, 0x3b,     // t y u i o p [ ] enter lctrl
        0x00, 0x01, 0x02, 0x03, 0x05, 0x04, 0x26, 0x28, 0x25, 0x29,     // a s d f g h j k l ;
        0x27, 0x32, 0x38, 0x2a, 0x06, 0x07, 0x08, 0x09, 0x0b, 0x2d,     // ' ` lshift \ z x c v b n
        0x2e, 0x2b, 0x2f, 0x2c, 0x3c, 0x43, 0x3a, 0x31, 0x39, 0x7a,     // m , . / rshift kp* lalt space caps f1
        0x78, 0x63, 0x76, 0x60, 0x61, 0x62, 0x64, 0x65, 0x6d, 0x47,     // f2..f10 numlock(clear)
        0x6b, 0x59, 0x5b, 0x5c, 0x4e, 0x56, 0x57, 0x58, 0x45, 0x53,     // scroll(f14) kp7 kp8 kp9 kp- kp4 kp5 kp6 kp+ kp1
        0x54, 0x55, 0x52, 0x41, kNoKey, kNoKey, 0x0a, 0x67, 0x6f,       // kp2 kp3 kp0 kp. - - 102nd f11 f12
    };
    if (evdev < sizeof(table) / sizeof(table[0])) return table[evdev];
    switch (evdev) {
    case 96: return 0x4c;    // KEY_KPENTER
    case 97: return 0x3e;    // KEY_RIGHTCTRL
    case 98: return 0x4b;    // KEY_KPSLASH
    case 99: return 0x69;    // KEY_SYSRQ (F13 position)
    case 100: return 0x3d;   // KEY_RIGHTALT
    case 102: return 0x73;   // KEY_HOME
    case 103: return 0x7e;   // KEY_UP
    case 104: return 0x74;   // KEY_PAGEUP
    case 105: return 0x7b;   // KEY_LEFT
    case 106: return 0x7c;   // KEY_RIGHT
    case 107: return 0x77;   // KEY_END
    case 108: return 0x7d;   // KEY_DOWN
    case 109: return 0x79;   // KEY_PAGEDOWN
    case 110: return 0x72;   // KEY_INSERT (Help position)
    case 111: return 0x75;   // KEY_DELETE
    case 113: return 0x4a;   // KEY_MUTE
    case 114: return 0x49;   // KEY_VOLUMEDOWN
    case 115: return 0x48;   // KEY_VOLUMEUP
    case 117: return 0x51;   // KEY_KPEQUAL
    case 119: return 0x71;   // KEY_PAUSE (F15 position)
    case 121: return 0x5f;   // KEY_KPCOMMA
    case 125: return 0x37;   // KEY_LEFTMETA -> Command
    case 126: return 0x36;   // KEY_RIGHTMETA
    case 127: return 0x6e;   // KEY_COMPOSE -> contextual menu
    default: return kNoKey;
    }
}

std::uint32_t evdev_for_mac_key(std::uint16_t key) noexcept {
    for (std::uint32_t code = 1; code < 128; ++code)
        if (mac_key_code(code) == key) return code;
    return 0;
}

// NSEvent's function-key characters (NSUpArrowFunctionKey = 0xF700, ...) and
// the control characters AppKit gives Return, Tab, Delete and Escape.
std::optional<char32_t> special_character(std::uint16_t key) noexcept {
    switch (key) {
    case 0x7e: return 0xf700;
    case 0x7d: return 0xf701;
    case 0x7b: return 0xf702;
    case 0x7c: return 0xf703;
    case 0x7a: return 0xf704;
    case 0x78: return 0xf705;
    case 0x63: return 0xf706;
    case 0x76: return 0xf707;
    case 0x60: return 0xf708;
    case 0x61: return 0xf709;
    case 0x62: return 0xf70a;
    case 0x64: return 0xf70b;
    case 0x65: return 0xf70c;
    case 0x6d: return 0xf70d;
    case 0x67: return 0xf70e;
    case 0x6f: return 0xf70f;
    case 0x75: return 0xf728;
    case 0x73: return 0xf729;
    case 0x77: return 0xf72b;
    case 0x74: return 0xf72c;
    case 0x79: return 0xf72d;
    case 0x72: return 0xf746;   // Help
    case 0x24: case 0x4c: return U'\r';
    case 0x30: return U'\t';
    case 0x33: return 0x7f;
    case 0x35: return 0x1b;
    default: return std::nullopt;
    }
}

void append_utf8(std::string& out, char32_t c) {
    if (c < 0x80) out += static_cast<char>(c);
    else if (c < 0x800) { out += static_cast<char>(0xc0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 0x3f)); }
    else if (c < 0x10000) {
        out += static_cast<char>(0xe0 | (c >> 12)); out += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (c & 0x3f));
    } else {
        out += static_cast<char>(0xf0 | (c >> 18)); out += static_cast<char>(0x80 | ((c >> 12) & 0x3f));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3f)); out += static_cast<char>(0x80 | (c & 0x3f));
    }
}

bool is_modifier_key(std::uint16_t key) noexcept { return key >= 0x36 && key <= 0x3f; }

// ---- the window server -----------------------------------------------------------------

struct WindowRecord {
    WindowId id{};
    xcb_window_t xid{};
    std::shared_ptr<WindowEventSink> sink;
    std::uint64_t owner{};
    std::uint64_t style{};
    WindowLevel level{};
    Rect content;                // screen points (AppKit)
    bool visible{};
    bool accepts_mouse_moved{};
    bool mouse_inside{};
    WindowId parent{};
    Size minimum{}, maximum{};
};

class X11WindowServer final : public WindowSystem {
public:
    X11WindowServer() = default;
    ~X11WindowServer() override { shutdown(); }

    bool interactive() const noexcept override { return const_cast<X11WindowServer*>(this)->open(); }

    void shutdown() noexcept override {
        if (!connection_) return;
        for (auto& [id, record] : windows_) xcb_destroy_window(connection_, record.xid);
        windows_.clear();
        by_xid_.clear();
        xcb_flush(connection_);
    }

    // ---- windows ----
    WindowId create_window(const WindowDesc& description, std::shared_ptr<WindowEventSink> sink) override {
        require_open("create_window");
        WindowRecord record;
        record.id = next_window_++;
        record.xid = xcb_generate_id(connection_);
        record.sink = std::move(sink);
        record.owner = description.owner;
        record.style = description.style_mask;
        record.level = description.level;
        record.content = description.content_rect;
        record.accepts_mouse_moved = description.accepts_mouse_moved;
        record.parent = description.parent;
        const auto pixels = to_x(record.content);
        const std::uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK | XCB_CW_CURSOR;
        const std::uint32_t values[] = {
            screen_->white_pixel,
            // Borderless windows at a level above normal (menus, pop-ups,
            // tool tips) are placed by AppKit, not the window manager.
            (description.style_mask == style::borderless && description.level > 0) ? 1u : 0u,
            XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_KEY_PRESS |
                XCB_EVENT_MASK_KEY_RELEASE | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
                XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW |
                XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_PROPERTY_CHANGE,
            current_cursor_};   // set_cursor's cursor (XCB_NONE: the parent's)
        xcb_create_window(connection_, XCB_COPY_FROM_PARENT, record.xid, screen_->root, static_cast<std::int16_t>(pixels.x),
                          static_cast<std::int16_t>(pixels.y), static_cast<std::uint16_t>(std::max(1, pixels.width)),
                          static_cast<std::uint16_t>(std::max(1, pixels.height)), 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                          screen_->root_visual, mask, values);
        xcb_atom_t protocols[] = {wm_delete_window_};
        xcb_icccm_set_wm_protocols(connection_, record.xid, wm_protocols_, 1, protocols);
        // The position AppKit asked for, not one the window manager picks.
        xcb_size_hints_t hints{};
        xcb_icccm_size_hints_set_position(&hints, 1, pixels.x, pixels.y);
        xcb_icccm_size_hints_set_size(&hints, 1, pixels.width, pixels.height);
        if (!(description.style_mask & style::resizable) && description.style_mask != style::borderless) {
            xcb_icccm_size_hints_set_min_size(&hints, pixels.width, pixels.height);
            xcb_icccm_size_hints_set_max_size(&hints, pixels.width, pixels.height);
        }
        xcb_icccm_set_wm_normal_hints(connection_, record.xid, &hints);
        set_class(record.xid);
        windows_[record.id] = record;
        by_xid_[record.xid] = record.id;
        set_title(record.id, description.title);
        if (description.parent) set_parent(record.id, description.parent);
        if (description.visible) show(record.id, ShowCommand::order_front);
        xcb_flush(connection_);
        return record.id;
    }

    void destroy_window(WindowId window) override {
        auto& record = find(window, "destroy_window");
        xcb_destroy_window(connection_, record.xid);
        by_xid_.erase(record.xid);
        windows_.erase(window);
        if (key_window_ == window) key_window_ = 0;
        xcb_flush(connection_);
    }

    void show(WindowId window, ShowCommand command) override {
        auto& record = find(window, "show");
        switch (command) {
        case ShowCommand::order_out:
            xcb_unmap_window(connection_, record.xid);
            record.visible = false;
            break;
        case ShowCommand::order_back: {
            const std::uint32_t below = XCB_STACK_MODE_BELOW;
            xcb_map_window(connection_, record.xid);
            xcb_configure_window(connection_, record.xid, XCB_CONFIG_WINDOW_STACK_MODE, &below);
            record.visible = true;
            break;
        }
        case ShowCommand::order_front:
        case ShowCommand::make_key_and_order_front: {
            const std::uint32_t above = XCB_STACK_MODE_ABOVE;
            xcb_map_window(connection_, record.xid);
            xcb_configure_window(connection_, record.xid, XCB_CONFIG_WINDOW_STACK_MODE, &above);
            record.visible = true;
            if (command == ShowCommand::make_key_and_order_front) {
                send_net_wm(record.xid, net_active_window_, 1, XCB_CURRENT_TIME, 0);
                pending_focus_ = record.id;   // takes focus once it is viewable (MapNotify)
            }
            break;
        }
        }
        xcb_flush(connection_);
    }

    void set_parent(WindowId window, WindowId parent) override {
        auto& record = find(window, "set_parent");
        record.parent = parent;
        if (const auto it = windows_.find(parent); it != windows_.end())
            xcb_icccm_set_wm_transient_for(connection_, record.xid, it->second.xid);
        xcb_flush(connection_);
    }

    void set_title(WindowId window, std::string_view title) override {
        auto& record = find(window, "set_title");
        xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, record.xid, net_wm_name_, utf8_string_, 8,
                            static_cast<std::uint32_t>(title.size()), title.data());
        xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, record.xid, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
                            static_cast<std::uint32_t>(title.size()), title.data());
        xcb_flush(connection_);
    }

    void set_style(WindowId window, std::uint64_t style_mask) override { find(window, "set_style").style = style_mask; }

    void set_level(WindowId window, WindowLevel level) override {
        auto& record = find(window, "set_level");
        record.level = level;
        send_net_wm(record.xid, net_wm_state_, level > 0 ? 1 : 0, net_wm_state_above_, 0);
        xcb_flush(connection_);
    }

    void set_content_rect(WindowId window, Rect content_rect) override {
        auto& record = find(window, "set_content_rect");
        record.content = content_rect;
        const auto pixels = to_x(content_rect);
        const std::uint32_t values[] = {static_cast<std::uint32_t>(pixels.x), static_cast<std::uint32_t>(pixels.y),
                                        static_cast<std::uint32_t>(std::max(1, pixels.width)),
                                        static_cast<std::uint32_t>(std::max(1, pixels.height))};
        xcb_configure_window(connection_, record.xid,
                             XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                             values);
        xcb_flush(connection_);
    }

    Rect content_rect(WindowId window) const override { return find(window, "content_rect").content; }
    Rect frame_rect(WindowId window) const override {
        const auto& record = find(window, "frame_rect");
        return frame_for_content(record.content, record.style);
    }
    Rect frame_for_content(Rect content, std::uint64_t style_mask) const override {
        if (!(style_mask & style::titled) || (style_mask & style::full_size_content_view)) return content;
        content.size.height += kTitleBar;
        return content;
    }
    Rect content_for_frame(Rect frame, std::uint64_t style_mask) const override {
        if (!(style_mask & style::titled) || (style_mask & style::full_size_content_view)) return frame;
        frame.size.height = std::max(0.0, frame.size.height - kTitleBar);
        return frame;
    }

    void set_content_limits(WindowId window, Size minimum, Size maximum) override {
        auto& record = find(window, "set_content_limits");
        record.minimum = minimum;
        record.maximum = maximum;
        xcb_size_hints_t hints{};
        xcb_icccm_size_hints_set_min_size(&hints, static_cast<std::int32_t>(minimum.width), static_cast<std::int32_t>(minimum.height));
        if (maximum.width > 0 && maximum.height > 0 && maximum.width < 1e6 && maximum.height < 1e6)
            xcb_icccm_size_hints_set_max_size(&hints, static_cast<std::int32_t>(maximum.width), static_cast<std::int32_t>(maximum.height));
        xcb_icccm_set_wm_normal_hints(connection_, record.xid, &hints);
        xcb_flush(connection_);
    }

    void miniaturize(WindowId window, bool miniaturized) override {
        auto& record = find(window, "miniaturize");
        if (miniaturized) {
            xcb_client_message_event_t message{};
            message.response_type = XCB_CLIENT_MESSAGE;
            message.format = 32;
            message.window = record.xid;
            message.type = wm_change_state_;
            message.data.data32[0] = XCB_ICCCM_WM_STATE_ICONIC;
            xcb_send_event(connection_, 0, screen_->root,
                           XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                           reinterpret_cast<const char*>(&message));
        } else {
            xcb_map_window(connection_, record.xid);
        }
        xcb_flush(connection_);
    }

    void zoom(WindowId window) override {
        auto& record = find(window, "zoom");
        send_net_wm(record.xid, net_wm_state_, 2, net_wm_state_maximized_vert_, net_wm_state_maximized_horz_);
        xcb_flush(connection_);
    }

    void set_full_screen(WindowId window, bool full_screen) override {
        auto& record = find(window, "set_full_screen");
        send_net_wm(record.xid, net_wm_state_, full_screen ? 1 : 0, net_wm_state_fullscreen_, 0);
        xcb_flush(connection_);
    }

    void set_accepts_mouse_moved(WindowId window, bool accepts) override {
        find(window, "set_accepts_mouse_moved").accepts_mouse_moved = accepts;
    }
    void set_accepts_file_drops(WindowId window, bool) override { find(window, "set_accepts_file_drops"); }

    void invalidate(WindowId window, std::optional<Rect> content_rect) override {
        auto& record = find(window, "invalidate");
        Rect dirty = content_rect.value_or(Rect{{0, 0}, record.content.size});
        invalidations_.push_back({window, dirty});
    }

    WindowId key_window() const override { return key_window_; }
    bool is_visible(WindowId window) const override { return find(window, "is_visible").visible; }
    bool window_exists(WindowId window) const override { return windows_.count(window) != 0; }
    double backing_scale(WindowId window) const override { find(window, "backing_scale"); return 1.0; }
    ScreenId window_screen(WindowId window) const override {
        const auto& record = find(window, "window_screen");
        // The monitor with the largest share of the window (the primary when none).
        ScreenId best = monitors_.empty() ? 1 : monitors_.front().info.id;
        double best_area = 0;
        for (const auto& monitor : monitors_) {
            const auto& frame = monitor.info.frame;
            const double width = std::min(frame.origin.x + frame.size.width, record.content.origin.x + record.content.size.width) -
                                 std::max(frame.origin.x, record.content.origin.x);
            const double height = std::min(frame.origin.y + frame.size.height, record.content.origin.y + record.content.size.height) -
                                  std::max(frame.origin.y, record.content.origin.y);
            if (width > 0 && height > 0 && width * height > best_area) best_area = width * height, best = monitor.info.id;
        }
        return best;
    }
    Size content_pixel_size(WindowId window) const override { return find(window, "content_pixel_size").content.size; }
    void* native_handle(WindowId window) const override {
        return reinterpret_cast<void*>(static_cast<std::uintptr_t>(find(window, "native_handle").xid));
    }

    // ---- input state ----
    std::uint64_t modifier_flags() const override { return modifiers_; }
    std::uint64_t pressed_mouse_buttons() const override { return buttons_; }
    Point mouse_location() const override {
        if (!const_cast<X11WindowServer*>(this)->open()) return {};
        auto* reply = xcb_query_pointer_reply(connection_, xcb_query_pointer(connection_, screen_->root), nullptr);
        if (!reply) return {};
        const Point point = to_appkit(reply->root_x, reply->root_y);
        std::free(reply);
        return point;
    }
    double double_click_interval() const override { return 0.5; }
    double key_repeat_delay() const override { return 0.5; }
    double key_repeat_interval() const override { return 1.0 / 30; }

    // ---- cursors ----
    // System cursors keep the ids enum + 1; images get ids from 1000 on.
    CursorId system_cursor(SystemCursor cursor) override { return static_cast<CursorId>(cursor) + 1; }

    CursorId create_cursor(const Image& image, Point hot_spot) override {
        require_open("create_cursor");
        if (!image.width || !image.height || image.rgba.size() < static_cast<std::size_t>(image.width) * image.height * 4)
            throw std::runtime_error("Alchemy window server: create_cursor: an empty or short image");
        const xcb_render_pictforminfo_t* format = nullptr;
        if (const auto* formats = render_formats())
            format = xcb_render_util_find_standard_format(formats, XCB_PICT_STANDARD_ARGB_32);
        if (!format) return system_cursor(SystemCursor::arrow);   // no Render: the arrow stands in
        // Render cursors take premultiplied ARGB32 (BGRA in memory on little-endian hosts).
        std::vector<std::uint8_t> argb(image.rgba.size());
        for (std::size_t i = 0; i + 3 < image.rgba.size(); i += 4) {
            const unsigned alpha = image.rgba[i + 3];
            argb[i + 0] = static_cast<std::uint8_t>(image.rgba[i + 2] * alpha / 255);
            argb[i + 1] = static_cast<std::uint8_t>(image.rgba[i + 1] * alpha / 255);
            argb[i + 2] = static_cast<std::uint8_t>(image.rgba[i + 0] * alpha / 255);
            argb[i + 3] = static_cast<std::uint8_t>(alpha);
        }
        const auto width = static_cast<std::uint16_t>(image.width), height = static_cast<std::uint16_t>(image.height);
        const xcb_pixmap_t pixmap = xcb_generate_id(connection_);
        xcb_create_pixmap(connection_, 32, pixmap, screen_->root, width, height);
        const xcb_gcontext_t gc = xcb_generate_id(connection_);
        xcb_create_gc(connection_, gc, pixmap, 0, nullptr);
        xcb_put_image(connection_, XCB_IMAGE_FORMAT_Z_PIXMAP, pixmap, gc, width, height, 0, 0, 0, 32,
                      static_cast<std::uint32_t>(argb.size()), argb.data());
        const xcb_render_picture_t picture = xcb_generate_id(connection_);
        xcb_render_create_picture(connection_, picture, pixmap, format->id, 0, nullptr);
        const xcb_cursor_t cursor = xcb_generate_id(connection_);
        xcb_render_create_cursor(connection_, cursor, picture,
                                 static_cast<std::uint16_t>(std::clamp(hot_spot.x, 0.0, static_cast<double>(width - 1))),
                                 static_cast<std::uint16_t>(std::clamp(hot_spot.y, 0.0, static_cast<double>(height - 1))));
        xcb_render_free_picture(connection_, picture);
        xcb_free_gc(connection_, gc);
        xcb_free_pixmap(connection_, pixmap);
        xcb_flush(connection_);
        const CursorId id = next_cursor_++;
        custom_cursors_[id] = cursor;
        return id;
    }

    void destroy_cursor(CursorId id) override {
        const auto it = custom_cursors_.find(id);
        if (it == custom_cursors_.end()) return;   // system cursors stay
        if (current_cursor_ == it->second) apply_cursor(XCB_NONE);
        xcb_free_cursor(connection_, it->second);
        custom_cursors_.erase(it);
        xcb_flush(connection_);
    }

    void set_cursor(CursorId id) override {
        if (!open()) return;
        xcb_cursor_t cursor = XCB_NONE;
        if (const auto it = custom_cursors_.find(id); it != custom_cursors_.end()) cursor = it->second;
        else if (id >= 1 && id <= static_cast<CursorId>(SystemCursor::wait) + 1) cursor = load_system_cursor(static_cast<SystemCursor>(id - 1));
        apply_cursor(cursor);
    }

    void set_cursor_visible(bool visible) override {
        if (!open() || !xfixes_ || visible == cursor_visible_) return;
        // XFixes hides the cursor while it is over this client's windows.
        if (visible) xcb_xfixes_show_cursor(connection_, screen_->root);
        else xcb_xfixes_hide_cursor(connection_, screen_->root);
        cursor_visible_ = visible;
        xcb_flush(connection_);
    }

    void warp_cursor(Point screen_location) override {
        require_open("warp_cursor");
        const auto [x, y] = to_root(screen_location);
        xcb_warp_pointer(connection_, XCB_NONE, screen_->root, 0, 0, 0, 0, static_cast<std::int16_t>(x), static_cast<std::int16_t>(y));
        xcb_flush(connection_);
    }
    void associate_mouse_and_cursor(bool) override {}
    void confine_cursor(WindowId, std::optional<Rect>) override {}

    // ---- clipboard: the X CLIPBOARD selection ----
    std::optional<std::string> clipboard_text() const override {
        auto& self = const_cast<X11WindowServer&>(*this);
        if (!self.open()) return clipboard_text_;
        if (owns_clipboard()) return clipboard_text_;
        const auto offered = self.offered_targets();
        for (const xcb_atom_t target : {utf8_string_, text_plain_utf8_, static_cast<xcb_atom_t>(XCB_ATOM_STRING)}) {
            if (target == XCB_NONE || (offered && std::find(offered->begin(), offered->end(), target) == offered->end())) continue;
            if (auto bytes = self.convert_clipboard(target)) return std::string(bytes->begin(), bytes->end());
        }
        return std::nullopt;
    }

    void set_clipboard_text(std::string_view text) override {
        clipboard_text_ = std::string(text);
        clipboard_image_.reset();
        clipboard_png_.clear();
        take_clipboard();
    }

    std::optional<Image> clipboard_image() const override {
        auto& self = const_cast<X11WindowServer&>(*this);
        if (!self.open() || owns_clipboard()) return clipboard_image_;
        if (png_ == XCB_NONE) return std::nullopt;
        if (const auto offered = self.offered_targets(); offered && std::find(offered->begin(), offered->end(), png_) == offered->end())
            return std::nullopt;
        const auto bytes = self.convert_clipboard(png_);
        if (!bytes) return std::nullopt;
        auto pixels = gpu::decode_image(*bytes);
        if (!pixels) return std::nullopt;
        return Image{pixels->width, pixels->height, std::move(pixels->rgba)};
    }

    void set_clipboard_image(const Image& image) override {
        clipboard_image_ = image;
        clipboard_text_.reset();
        clipboard_png_.clear();
        if (auto png = gpu::encode_image(gpu::Pixels{image.width, image.height, image.rgba}, gpu::ImageFormat::png)) clipboard_png_ = std::move(*png);
        take_clipboard();
    }

    std::int64_t clipboard_change_count() const override {
        // Another client's new selection (XFixes reports it in dispatch_messages;
        // a changed owner is noticed here as well, between dispatches).
        auto& self = const_cast<X11WindowServer&>(*this);
        if (self.open()) {
            if (auto* reply = xcb_get_selection_owner_reply(connection_, xcb_get_selection_owner(connection_, clipboard_), nullptr)) {
                if (reply->owner != self.last_clipboard_owner_) {
                    self.last_clipboard_owner_ = reply->owner;
                    if (reply->owner != clipboard_window_) ++self.clipboard_changes_;
                }
                std::free(reply);
            }
        }
        return clipboard_changes_;
    }

    // ---- screens and keyboard ----
    std::vector<ScreenInfo> screens() const override {
        if (!const_cast<X11WindowServer*>(this)->open()) return {};
        std::vector<ScreenInfo> result;
        for (const auto& monitor : monitors_) result.push_back(monitor.info);
        return result;
    }

    KeyboardLayout keyboard_layout() const override {
        return {"com.apple.keylayout.US", "U.S.", "en"};
    }

    std::string translate_key(std::uint16_t key_code, std::uint64_t flags, DeadKeyState& state) const override {
        state.value = 0;
        if (const auto special = special_character(key_code)) {
            std::string text;
            append_utf8(text, *special);
            return text;
        }
        if (!const_cast<X11WindowServer*>(this)->open() || !keymap_) return {};
        const auto evdev = evdev_for_mac_key(key_code);
        if (!evdev) return {};
        const xkb_level_index_t level = (flags & modifier::shift) ? 1 : 0;
        const xkb_keysym_t* syms = nullptr;
        if (xkb_keymap_key_get_syms_by_level(keymap_, evdev + 8, 0, level, &syms) < 1) return {};
        char text[16]{};
        if (xkb_keysym_to_utf8(syms[0], text, sizeof(text)) <= 1) return {};
        return text;
    }

    void set_text_input_enabled(WindowId window, bool) override { find(window, "set_text_input_enabled"); }
    void discard_marked_text(WindowId window) override { find(window, "discard_marked_text"); }

    // ---- the message pump ----
    bool dispatch_messages() override {
        if (!connection_) return false;
        bool any = false;
        std::exception_ptr failure;
        const auto deliver_all = [&](xcb_generic_event_t* event) {
            try {
                handle(event);
            } catch (...) {
                if (!failure) failure = std::current_exception();
            }
            std::free(event);
            any = true;
        };
        auto& queue = linux_gui::pending();
        while (!queue.empty()) {
            auto* event = queue.front();
            queue.pop_front();
            deliver_all(event);
        }
        while (auto* event = xcb_poll_for_event(connection_)) deliver_all(event);
        // invalidate(): expose notifications the window server owes AppKit.
        auto requests = std::move(invalidations_);
        invalidations_.clear();
        for (const auto& [window, rect] : requests) {
            const auto it = windows_.find(window);
            if (it == windows_.end()) continue;
            Event event = notification(it->second, Notification::expose);
            event.rect = rect;
            try {
                deliver(it->second, event);
            } catch (...) {
                if (!failure) failure = std::current_exception();
            }
            any = true;
        }
        if (failure) std::rethrow_exception(failure);
        return any;
    }

    void set_application_sink(std::shared_ptr<WindowEventSink> sink) override { application_sink_ = std::move(sink); }

    void present_bitmap(WindowId window, const BitmapView& pixels, std::optional<Rect>) override {
        auto& record = find(window, "present_bitmap");
        if (!pixels.bgra || pixels.width == 0 || pixels.height == 0 || !record.visible) return;
        if (pixels.stride < static_cast<std::size_t>(pixels.width) * 4 || pixels.stride % 4 != 0)
            throw std::runtime_error("Alchemy window server: present_bitmap: the row stride must be a multiple of 4 covering the width");
        if (!gc_) {
            gc_ = xcb_generate_id(connection_);
            xcb_create_gc(connection_, gc_, screen_->root, 0, nullptr);
        }
        // Rows per PutImage request: the request (header + pixels) within the
        // server's maximum request length.
        const std::size_t row_bytes = static_cast<std::size_t>(pixels.width) * 4;
        const std::size_t max_bytes = static_cast<std::size_t>(xcb_get_maximum_request_length(connection_)) * 4 - 64;
        const std::uint32_t rows_per_request = static_cast<std::uint32_t>(std::max<std::size_t>(1, max_bytes / row_bytes));
        std::vector<std::uint8_t> strip;
        for (std::uint32_t row = 0; row < pixels.height; row += rows_per_request) {
            const std::uint32_t rows = std::min(rows_per_request, pixels.height - row);
            const std::uint8_t* source = pixels.bgra + static_cast<std::size_t>(row) * pixels.stride;
            const std::uint8_t* data = source;
            if (pixels.stride != row_bytes) {
                strip.resize(row_bytes * rows);
                for (std::uint32_t r = 0; r < rows; ++r) std::memcpy(strip.data() + r * row_bytes, source + r * pixels.stride, row_bytes);
                data = strip.data();
            }
            xcb_put_image(connection_, XCB_IMAGE_FORMAT_Z_PIXMAP, record.xid, gc_, static_cast<std::uint16_t>(pixels.width),
                          static_cast<std::uint16_t>(rows), 0, static_cast<std::int16_t>(row), 0, screen_->root_depth,
                          static_cast<std::uint32_t>(row_bytes * rows), data);
        }
        xcb_flush(connection_);
    }

    void beep() override {
        if (open()) {
            xcb_bell(connection_, 0);
            xcb_flush(connection_);
        }
    }
    void request_attention(bool) override {
        if (!connection_ || !key_window_) return;
        if (const auto it = windows_.find(key_window_); it != windows_.end())
            send_net_wm(it->second.xid, net_wm_state_, 1, net_wm_state_demands_attention_, 0);
        xcb_flush(connection_);
    }

private:
    static constexpr double kTitleBar = 0;   // the window manager's decorations are outside the X window

    struct XRect {
        int x, y, width, height;
    };

    bool open() {
        if (connection_) return true;
        if (failed_) return false;
        int preferred = 0;
        connection_ = xcb_connect(nullptr, &preferred);
        if (!connection_ || xcb_connection_has_error(connection_)) {
            if (connection_) xcb_disconnect(connection_);
            connection_ = nullptr;
            failed_ = true;
            return false;
        }
        auto iterator = xcb_setup_roots_iterator(xcb_get_setup(connection_));
        for (int i = 0; i < preferred && iterator.rem; ++i) xcb_screen_next(&iterator);
        screen_ = iterator.data;
        linux_gui::g_connection = connection_;
        wm_protocols_ = atom("WM_PROTOCOLS");
        wm_delete_window_ = atom("WM_DELETE_WINDOW");
        wm_change_state_ = atom("WM_CHANGE_STATE");
        net_wm_name_ = atom("_NET_WM_NAME");
        utf8_string_ = atom("UTF8_STRING");
        net_wm_state_ = atom("_NET_WM_STATE");
        net_wm_state_above_ = atom("_NET_WM_STATE_ABOVE");
        net_wm_state_fullscreen_ = atom("_NET_WM_STATE_FULLSCREEN");
        net_wm_state_maximized_vert_ = atom("_NET_WM_STATE_MAXIMIZED_VERT");
        net_wm_state_maximized_horz_ = atom("_NET_WM_STATE_MAXIMIZED_HORZ");
        net_wm_state_demands_attention_ = atom("_NET_WM_STATE_DEMANDS_ATTENTION");
        net_active_window_ = atom("_NET_ACTIVE_WINDOW");
        net_workarea_ = atom("_NET_WORKAREA");
        clipboard_ = atom("CLIPBOARD");
        targets_ = atom("TARGETS");
        incr_ = atom("INCR");
        text_atom_ = atom("TEXT");
        text_plain_utf8_ = atom("text/plain;charset=utf-8");
        png_ = atom("image/png");
        transfer_ = atom("ALCHEMY_SELECTION");
        // The hidden window that owns the clipboard and receives conversions.
        clipboard_window_ = xcb_generate_id(connection_);
        const std::uint32_t helper_events = XCB_EVENT_MASK_PROPERTY_CHANGE;
        xcb_create_window(connection_, XCB_COPY_FROM_PARENT, clipboard_window_, screen_->root, -10, -10, 1, 1, 0,
                          XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT, XCB_CW_EVENT_MASK, &helper_events);
        // RandR (screens and their changes), XFixes (other clients' clipboard
        // and cursor hiding), Render (colour cursors), xcb-cursor (themes).
        if (const auto* randr = xcb_get_extension_data(connection_, &xcb_randr_id); randr && randr->present) {
            if (auto* version = xcb_randr_query_version_reply(connection_, xcb_randr_query_version(connection_, 1, 5), nullptr)) {
                randr_event_ = randr->first_event;
                randr_monitors_ = version->major_version > 1 || version->minor_version >= 5;
                std::free(version);
                xcb_randr_select_input(connection_, screen_->root, XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE);
            }
        }
        if (const auto* xfixes = xcb_get_extension_data(connection_, &xcb_xfixes_id); xfixes && xfixes->present) {
            if (auto* version = xcb_xfixes_query_version_reply(connection_, xcb_xfixes_query_version(connection_, 5, 0), nullptr)) {
                std::free(version);
                xfixes_ = true;
                xfixes_event_ = xfixes->first_event;
                xcb_xfixes_select_selection_input(connection_, clipboard_window_, clipboard_,
                                                  XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER |
                                                      XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY |
                                                      XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE);
            }
        }
        if (xcb_cursor_context_new(connection_, screen_, &cursor_context_) < 0) cursor_context_ = nullptr;
        refresh_screens();
        xcb_flush(connection_);
        // Keyboard: xkbcommon's view of the X server's keymap.
        if (xkb_x11_setup_xkb_extension(connection_, XKB_X11_MIN_MAJOR_XKB_VERSION, XKB_X11_MIN_MINOR_XKB_VERSION,
                                        XKB_X11_SETUP_XKB_EXTENSION_NO_FLAGS, nullptr, nullptr, nullptr, nullptr)) {
            xkb_context_ = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
            const auto device = xkb_x11_get_core_keyboard_device_id(connection_);
            if (xkb_context_ && device >= 0) {
                keymap_ = xkb_x11_keymap_new_from_device(xkb_context_, connection_, device, XKB_KEYMAP_COMPILE_NO_FLAGS);
                if (keymap_) xkb_state_ = xkb_x11_state_new_from_device(keymap_, connection_, device);
            }
        }
        return true;
    }

    void require_open(const char* operation) {
        if (!open())
            throw std::runtime_error(std::string("Alchemy window server: ") + operation + ": no X display ($DISPLAY)");
    }

    xcb_atom_t atom(const char* name) {
        auto* reply = xcb_intern_atom_reply(connection_, xcb_intern_atom(connection_, 0, static_cast<std::uint16_t>(std::strlen(name)), name), nullptr);
        if (!reply) return XCB_NONE;
        const auto value = reply->atom;
        std::free(reply);
        return value;
    }

    void set_class(xcb_window_t xid) {
        static constexpr char name[] = "alchemy\0Alchemy";
        xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, xid, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, sizeof(name), name);
    }

    // An EWMH client message to the root window (_NET_WM_STATE, _NET_ACTIVE_WINDOW).
    void send_net_wm(xcb_window_t xid, xcb_atom_t type, std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        xcb_client_message_event_t message{};
        message.response_type = XCB_CLIENT_MESSAGE;
        message.format = 32;
        message.window = xid;
        message.type = type;
        message.data.data32[0] = a;
        message.data.data32[1] = b;
        message.data.data32[2] = c;
        message.data.data32[3] = 1;   // a normal application
        xcb_send_event(connection_, 0, screen_->root, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                       reinterpret_cast<const char*>(&message));
    }

    // ---- coordinates: AppKit points from the primary monitor's bottom-left, y up ----
    Point to_appkit(int root_x, int root_y) const {
        return {static_cast<double>(root_x - primary_left_), static_cast<double>(primary_bottom_ - root_y)};
    }
    std::pair<int, int> to_root(Point point) const {
        return {static_cast<int>(std::lround(point.x)) + primary_left_, primary_bottom_ - static_cast<int>(std::lround(point.y))};
    }
    XRect to_x(const Rect& rect) const {
        return {static_cast<int>(std::lround(rect.origin.x)) + primary_left_,
                primary_bottom_ - static_cast<int>(std::lround(rect.origin.y + rect.size.height)),
                static_cast<int>(std::lround(rect.size.width)), static_cast<int>(std::lround(rect.size.height))};
    }
    Rect from_x(int x, int y, int width, int height) const {
        return {{static_cast<double>(x - primary_left_), static_cast<double>(primary_bottom_ - (y + height))},
                {static_cast<double>(width), static_cast<double>(height)}};
    }

    // ---- screens ----
    struct MonitorRecord {
        ScreenInfo info;
        int x{}, y{}, width{}, height{};   // root pixels
    };

    std::string atom_name(xcb_atom_t value) {
        std::string name;
        if (auto* reply = xcb_get_atom_name_reply(connection_, xcb_get_atom_name(connection_, value), nullptr)) {
            name.assign(xcb_get_atom_name_name(reply), static_cast<std::size_t>(xcb_get_atom_name_name_length(reply)));
            std::free(reply);
        }
        return name;
    }

    // The refresh rate of the CRTC that drives `output` (0: unknown).
    double refresh_rate(xcb_randr_output_t output, const xcb_randr_get_screen_resources_current_reply_t* resources) {
        if (!resources) return 0;
        double rate = 0;
        auto* info = xcb_randr_get_output_info_reply(connection_, xcb_randr_get_output_info(connection_, output, resources->config_timestamp), nullptr);
        if (!info) return 0;
        if (info->crtc != XCB_NONE) {
            if (auto* crtc = xcb_randr_get_crtc_info_reply(connection_, xcb_randr_get_crtc_info(connection_, info->crtc, resources->config_timestamp), nullptr)) {
                const auto* modes = xcb_randr_get_screen_resources_current_modes(resources);
                const int count = xcb_randr_get_screen_resources_current_modes_length(resources);
                for (int i = 0; i < count; ++i)
                    if (modes[i].id == crtc->mode && modes[i].htotal && modes[i].vtotal)
                        rate = static_cast<double>(modes[i].dot_clock) / (static_cast<double>(modes[i].htotal) * modes[i].vtotal);
                std::free(crtc);
            }
        }
        std::free(info);
        return rate;
    }

    void refresh_screens() {
        std::vector<MonitorRecord> found;
        if (randr_monitors_) {
            auto* resources = xcb_randr_get_screen_resources_current_reply(
                connection_, xcb_randr_get_screen_resources_current(connection_, screen_->root), nullptr);
            if (auto* reply = xcb_randr_get_monitors_reply(connection_, xcb_randr_get_monitors(connection_, screen_->root, 1), nullptr)) {
                for (auto it = xcb_randr_get_monitors_monitors_iterator(reply); it.rem; xcb_randr_monitor_info_next(&it)) {
                    const auto* monitor = it.data;
                    MonitorRecord record;
                    record.x = monitor->x, record.y = monitor->y, record.width = monitor->width, record.height = monitor->height;
                    record.info.id = monitor->name ? monitor->name : static_cast<ScreenId>(found.size() + 1);
                    record.info.name = atom_name(monitor->name);
                    record.info.primary = monitor->primary;
                    record.info.pixel_width = monitor->width;
                    record.info.pixel_height = monitor->height;
                    record.info.millimeters_width = monitor->width_in_millimeters;
                    record.info.millimeters_height = monitor->height_in_millimeters;
                    if (xcb_randr_monitor_info_outputs_length(monitor) > 0)
                        record.info.refresh_rate = refresh_rate(xcb_randr_monitor_info_outputs(monitor)[0], resources);
                    found.push_back(std::move(record));
                }
                std::free(reply);
            }
            std::free(resources);
        }
        if (found.empty()) {   // no RandR 1.5 (or no monitors): the root window is the screen
            MonitorRecord record;
            record.width = screen_->width_in_pixels, record.height = screen_->height_in_pixels;
            record.info.id = 1;
            record.info.name = "X11 screen";
            record.info.primary = true;
            record.info.pixel_width = screen_->width_in_pixels;
            record.info.pixel_height = screen_->height_in_pixels;
            record.info.millimeters_width = screen_->width_in_millimeters;
            record.info.millimeters_height = screen_->height_in_millimeters;
            found.push_back(std::move(record));
        }
        // The primary first (NSScreen.screens[0] is the one with the origin).
        auto primary = std::find_if(found.begin(), found.end(), [](const MonitorRecord& record) { return record.info.primary; });
        if (primary == found.end()) primary = found.begin();
        primary->info.primary = true;
        std::rotate(found.begin(), primary, primary + 1);
        primary_left_ = found.front().x;
        primary_bottom_ = found.front().y + found.front().height;
        // The window manager's work area (_NET_WORKAREA: x, y, width, height of the current desktop).
        std::optional<XRect> work;
        if (net_workarea_ != XCB_NONE)
            if (auto* reply = xcb_get_property_reply(
                    connection_, xcb_get_property(connection_, 0, screen_->root, net_workarea_, XCB_ATOM_CARDINAL, 0, 4), nullptr)) {
                if (reply->format == 32 && xcb_get_property_value_length(reply) >= 16) {
                    const auto* values = static_cast<const std::uint32_t*>(xcb_get_property_value(reply));
                    work = XRect{static_cast<int>(values[0]), static_cast<int>(values[1]), static_cast<int>(values[2]), static_cast<int>(values[3])};
                }
                std::free(reply);
            }
        for (auto& record : found) {
            record.info.frame = from_x(record.x, record.y, record.width, record.height);
            record.info.visible_frame = record.info.frame;
            if (work) {
                const int left = std::max(record.x, work->x), top = std::max(record.y, work->y);
                const int right = std::min(record.x + record.width, work->x + work->width);
                const int bottom = std::min(record.y + record.height, work->y + work->height);
                if (right > left && bottom > top) record.info.visible_frame = from_x(left, top, right - left, bottom - top);
            }
        }
        monitors_ = std::move(found);
    }

    // ---- cursors ----
    const xcb_render_query_pict_formats_reply_t* render_formats() {
        if (!render_formats_ && !render_checked_) {
            render_checked_ = true;
            if (const auto* render = xcb_get_extension_data(connection_, &xcb_render_id); render && render->present)
                render_formats_ = xcb_render_util_query_formats(connection_);
        }
        return render_formats_;
    }

    xcb_cursor_t load_system_cursor(SystemCursor kind) {
        if (const auto it = system_cursors_.find(kind); it != system_cursors_.end()) return it->second;
        // Cursor-theme names first (the CSS set), then the core cursor font's.
        static const std::map<SystemCursor, std::vector<const char*>> names = {
            {SystemCursor::arrow, {"default", "left_ptr"}},
            {SystemCursor::ibeam, {"text", "xterm"}},
            {SystemCursor::crosshair, {"crosshair", "cross"}},
            {SystemCursor::closed_hand, {"grabbing", "closedhand", "fleur"}},
            {SystemCursor::open_hand, {"grab", "openhand", "hand1"}},
            {SystemCursor::pointing_hand, {"pointer", "hand2"}},
            {SystemCursor::resize_left, {"w-resize", "left_side"}},
            {SystemCursor::resize_right, {"e-resize", "right_side"}},
            {SystemCursor::resize_left_right, {"ew-resize", "col-resize", "sb_h_double_arrow"}},
            {SystemCursor::resize_up, {"n-resize", "top_side"}},
            {SystemCursor::resize_down, {"s-resize", "bottom_side"}},
            {SystemCursor::resize_up_down, {"ns-resize", "row-resize", "sb_v_double_arrow"}},
            {SystemCursor::disappearing_item, {"pirate", "X_cursor"}},
            {SystemCursor::operation_not_allowed, {"not-allowed", "crossed_circle", "circle"}},
            {SystemCursor::drag_link, {"alias", "dnd-link", "link"}},
            {SystemCursor::drag_copy, {"copy", "dnd-copy", "plus"}},
            {SystemCursor::contextual_menu, {"context-menu", "left_ptr"}},
            {SystemCursor::ibeam_vertical, {"vertical-text", "xterm"}},
            {SystemCursor::wait, {"wait", "watch"}},
        };
        xcb_cursor_t cursor = XCB_NONE;
        if (cursor_context_)
            if (const auto it = names.find(kind); it != names.end())
                for (const char* name : it->second)
                    if ((cursor = xcb_cursor_load_cursor(cursor_context_, name)) != XCB_NONE) break;
        system_cursors_[kind] = cursor;
        return cursor;
    }

    // X cursors belong to windows: set_cursor's cursor goes on every window
    // (and on the ones created later).
    void apply_cursor(xcb_cursor_t cursor) {
        current_cursor_ = cursor;
        for (const auto& [id, record] : windows_) xcb_change_window_attributes(connection_, record.xid, XCB_CW_CURSOR, &cursor);
        xcb_flush(connection_);
    }

    // ---- clipboard ----
    bool owns_clipboard() const noexcept { return owns_clipboard_; }

    void take_clipboard() {
        ++clipboard_changes_;
        if (!open()) return;
        xcb_set_selection_owner(connection_, clipboard_window_, clipboard_, XCB_CURRENT_TIME);
        owns_clipboard_ = false;
        if (auto* reply = xcb_get_selection_owner_reply(connection_, xcb_get_selection_owner(connection_, clipboard_), nullptr)) {
            owns_clipboard_ = reply->owner == clipboard_window_;
            last_clipboard_owner_ = reply->owner;
            std::free(reply);
        }
    }

    // Waits (up to `timeout_ms`) for the next event `accept` takes; other
    // events queue for dispatch_messages.
    template <class Accept>
    xcb_generic_event_t* wait_for_event(Accept accept, int timeout_ms) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            while (auto* event = xcb_poll_for_event(connection_)) {
                if (accept(event)) return event;
                linux_gui::pending().push_back(event);
            }
            if (xcb_connection_has_error(connection_) || std::chrono::steady_clock::now() >= deadline) return nullptr;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    // The targets the clipboard's owner offers (nullopt: it does not say, so
    // every target is worth asking for).
    std::optional<std::vector<xcb_atom_t>> offered_targets() {
        const auto bytes = convert_clipboard(targets_);
        if (!bytes || bytes->size() < sizeof(xcb_atom_t)) return std::nullopt;
        std::vector<xcb_atom_t> atoms(bytes->size() / sizeof(xcb_atom_t));
        std::memcpy(atoms.data(), bytes->data(), atoms.size() * sizeof(xcb_atom_t));
        return atoms;
    }

    // Another client's clipboard as `target` (INCR transfers included).
    std::optional<std::vector<std::uint8_t>> convert_clipboard(xcb_atom_t target) {
        xcb_delete_property(connection_, clipboard_window_, transfer_);
        xcb_convert_selection(connection_, clipboard_window_, clipboard_, target, transfer_, XCB_CURRENT_TIME);
        xcb_flush(connection_);
        auto* notify = wait_for_event(
            [&](xcb_generic_event_t* event) {
                return (event->response_type & 0x7f) == XCB_SELECTION_NOTIFY &&
                       reinterpret_cast<xcb_selection_notify_event_t*>(event)->requestor == clipboard_window_;
            },
            1000);
        if (!notify) return std::nullopt;
        const auto property = reinterpret_cast<xcb_selection_notify_event_t*>(notify)->property;
        std::free(notify);
        if (property == XCB_NONE) return std::nullopt;   // the owner cannot give that target
        const auto read = [&](bool& incremental) -> std::optional<std::vector<std::uint8_t>> {
            auto* reply = xcb_get_property_reply(
                connection_, xcb_get_property(connection_, 1, clipboard_window_, transfer_, XCB_GET_PROPERTY_TYPE_ANY, 0, 0x1fffffff), nullptr);
            if (!reply) return std::nullopt;
            incremental = reply->type == incr_;
            const auto* data = static_cast<const std::uint8_t*>(xcb_get_property_value(reply));
            std::vector<std::uint8_t> bytes(data, data + xcb_get_property_value_length(reply));
            std::free(reply);
            return bytes;
        };
        bool incremental = false;
        auto bytes = read(incremental);
        if (!bytes || !incremental) return bytes;
        // INCR: the owner writes chunks; each delete asks for the next, an empty one ends it.
        std::vector<std::uint8_t> whole;
        xcb_flush(connection_);
        for (;;) {
            auto* event = wait_for_event(
                [&](xcb_generic_event_t* candidate) {
                    const auto* change = reinterpret_cast<xcb_property_notify_event_t*>(candidate);
                    return (candidate->response_type & 0x7f) == XCB_PROPERTY_NOTIFY && change->window == clipboard_window_ &&
                           change->atom == transfer_ && change->state == XCB_PROPERTY_NEW_VALUE;
                },
                2000);
            if (!event) return std::nullopt;
            std::free(event);
            bool more = false;
            auto chunk = read(more);
            xcb_flush(connection_);
            if (!chunk) return std::nullopt;
            if (chunk->empty()) return whole;
            whole.insert(whole.end(), chunk->begin(), chunk->end());
        }
    }

    // Another client asks for our clipboard.
    void answer_selection_request(const xcb_selection_request_event_t& request) {
        xcb_selection_notify_event_t notify{};
        notify.response_type = XCB_SELECTION_NOTIFY;
        notify.time = request.time;
        notify.requestor = request.requestor;
        notify.selection = request.selection;
        notify.target = request.target;
        notify.property = XCB_NONE;
        const xcb_atom_t property = request.property != XCB_NONE ? request.property : request.target;   // obsolete clients
        const std::size_t limit = static_cast<std::size_t>(xcb_get_maximum_request_length(connection_)) * 4 - 64;
        const auto put = [&](xcb_atom_t type, std::uint8_t format, std::uint32_t count, const void* data) {
            xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, request.requestor, property, type, format, count, data);
            notify.property = property;
        };
        if (request.selection == clipboard_ && owns_clipboard_) {
            const bool text = clipboard_text_.has_value();
            const bool image = !clipboard_png_.empty();
            if (request.target == targets_) {
                std::vector<xcb_atom_t> offered{targets_};
                if (text) offered.insert(offered.end(), {utf8_string_, text_plain_utf8_, static_cast<xcb_atom_t>(XCB_ATOM_STRING), text_atom_});
                if (image) offered.push_back(png_);
                put(XCB_ATOM_ATOM, 32, static_cast<std::uint32_t>(offered.size()), offered.data());
            } else if (text && (request.target == utf8_string_ || request.target == text_plain_utf8_ || request.target == text_atom_ ||
                                request.target == XCB_ATOM_STRING)) {
                // STRING is Latin-1: characters past it become '?'.
                std::string bytes = *clipboard_text_;
                if (request.target == XCB_ATOM_STRING) {
                    std::string latin;
                    for (std::size_t i = 0; i < bytes.size();) {
                        const auto lead = static_cast<unsigned char>(bytes[i]);
                        char32_t c = lead;
                        std::size_t length = 1;
                        if (lead >= 0xc0 && lead < 0xe0 && i + 1 < bytes.size()) c = ((lead & 0x1f) << 6) | (bytes[i + 1] & 0x3f), length = 2;
                        else if (lead >= 0xe0 && lead < 0xf0) c = 0xfffd, length = 3;
                        else if (lead >= 0xf0) c = 0xfffd, length = 4;
                        latin += c < 0x100 ? static_cast<char>(c) : '?';
                        i += length;
                    }
                    bytes = std::move(latin);
                }
                if (bytes.size() <= limit)
                    put(request.target == XCB_ATOM_STRING ? XCB_ATOM_STRING : utf8_string_, 8, static_cast<std::uint32_t>(bytes.size()),
                        bytes.data());
            } else if (image && request.target == png_ && clipboard_png_.size() <= limit) {
                put(png_, 8, static_cast<std::uint32_t>(clipboard_png_.size()), clipboard_png_.data());
            }
        }
        xcb_send_event(connection_, 0, request.requestor, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<const char*>(&notify));
        xcb_flush(connection_);
    }

    WindowRecord& find(WindowId window, const char* operation) {
        const auto it = windows_.find(window);
        if (it == windows_.end())
            throw std::runtime_error(std::string("Alchemy window server: ") + operation + ": unknown window " + std::to_string(window));
        return it->second;
    }
    const WindowRecord& find(WindowId window, const char* operation) const {
        return const_cast<X11WindowServer*>(this)->find(window, operation);
    }

    WindowRecord* record_of(xcb_window_t xid) {
        const auto it = by_xid_.find(xid);
        if (it == by_xid_.end()) return nullptr;
        const auto record = windows_.find(it->second);
        return record == windows_.end() ? nullptr : &record->second;
    }

    Event notification(const WindowRecord& record, Notification kind) const {
        Event event;
        event.kind = EventKind::notification;
        event.notification = kind;
        event.window = record.id;
        event.owner = record.owner;
        event.timestamp = gui::monotonic_seconds();
        return event;
    }

    void deliver(WindowRecord& record, const Event& event) {
        if (auto sink = record.sink) sink->handle_event(event);
    }
    void deliver_application(Notification kind) {
        if (!application_sink_) return;
        Event event;
        event.kind = EventKind::notification;
        event.notification = kind;
        event.timestamp = gui::monotonic_seconds();
        application_sink_->handle_event(event);
    }

    void fill_pointer(Event& event, const WindowRecord& record, int x, int y, int root_x, int root_y) const {
        event.window = record.id;
        event.owner = record.owner;
        event.timestamp = gui::monotonic_seconds();
        event.location = {static_cast<double>(x), record.content.size.height - y};
        event.screen_location = to_appkit(root_x, root_y);
        event.modifier_flags = modifiers_;
        event.pressed_buttons = buttons_;
    }

    void update_modifiers() {
        if (!xkb_state_) return;
        std::uint64_t flags = 0;
        const auto active = [&](const char* name) {
            return xkb_state_mod_name_is_active(xkb_state_, name, XKB_STATE_MODS_EFFECTIVE) > 0;
        };
        if (active(XKB_MOD_NAME_SHIFT)) flags |= modifier::shift;
        if (active(XKB_MOD_NAME_CTRL)) flags |= modifier::control;
        if (active(XKB_MOD_NAME_ALT)) flags |= modifier::option;
        if (active(XKB_MOD_NAME_LOGO)) flags |= modifier::command;
        if (active(XKB_MOD_NAME_CAPS)) flags |= modifier::caps_lock;
        modifiers_ = flags;
    }

    std::string key_characters(xcb_keycode_t keycode, std::uint16_t key, bool ignoring_modifiers) const {
        std::string text;
        if (const auto special = special_character(key)) {
            append_utf8(text, *special);
            return text;
        }
        if (!xkb_state_) return text;
        if (ignoring_modifiers) {
            const xkb_level_index_t level = (modifiers_ & modifier::shift) ? 1 : 0;
            const xkb_keysym_t* syms = nullptr;
            if (xkb_keymap_key_get_syms_by_level(keymap_, keycode, xkb_state_key_get_layout(xkb_state_, keycode), level, &syms) > 0) {
                char buffer[16]{};
                if (xkb_keysym_to_utf8(syms[0], buffer, sizeof(buffer)) > 1) text = buffer;
            }
            return text;
        }
        char buffer[64]{};
        if (xkb_state_key_get_utf8(xkb_state_, keycode, buffer, sizeof(buffer)) > 0) text = buffer;
        return text;
    }

    void handle_key(const xcb_key_press_event_t& key, bool down) {
        auto* record = record_of(key.event);
        const auto code = mac_key_code(key.detail >= 8 ? key.detail - 8u : 0u);
        if (xkb_state_) xkb_state_update_key(xkb_state_, key.detail, down ? XKB_KEY_DOWN : XKB_KEY_UP);
        update_modifiers();
        if (!record || code == kNoKey) return;
        Event event;
        event.window = record->id;
        event.owner = record->owner;
        event.timestamp = gui::monotonic_seconds();
        event.modifier_flags = modifiers_;
        event.key_code = code;
        if (is_modifier_key(code)) {
            event.type = event_type::flags_changed;
        } else {
            event.type = down ? event_type::key_down : event_type::key_up;
            event.is_repeat = down && held_keys_.count(key.detail) != 0;
            event.characters = key_characters(key.detail, code, false);
            event.characters_ignoring_modifiers = key_characters(key.detail, code, true);
        }
        if (down) held_keys_.insert(key.detail);
        else held_keys_.erase(key.detail);
        deliver(*record, event);
    }

    void handle_button(const xcb_button_press_event_t& press, bool down) {
        auto* record = record_of(press.event);
        if (!record) return;
        const unsigned button = press.detail;
        if (button >= 4 && button <= 7) {   // the wheel: one step per press
            if (!down) return;
            Event event;
            event.type = event_type::scroll_wheel;
            fill_pointer(event, *record, press.event_x, press.event_y, press.root_x, press.root_y);
            if (button == 4) event.delta_y = 1;
            else if (button == 5) event.delta_y = -1;
            else if (button == 6) event.delta_x = 1;
            else event.delta_x = -1;
            deliver(*record, event);
            return;
        }
        const std::int64_t number = button == 1 ? 0 : button == 3 ? 1 : button == 2 ? 2 : static_cast<std::int64_t>(button) - 5;
        if (down) buttons_ |= 1ULL << number;
        else buttons_ &= ~(1ULL << number);
        Event event;
        if (number == 0) event.type = down ? event_type::left_mouse_down : event_type::left_mouse_up;
        else if (number == 1) event.type = down ? event_type::right_mouse_down : event_type::right_mouse_up;
        else event.type = down ? event_type::other_mouse_down : event_type::other_mouse_up;
        fill_pointer(event, *record, press.event_x, press.event_y, press.root_x, press.root_y);
        event.button_number = number;
        if (down) {
            const double now = event.timestamp;
            const bool again = number == last_click_button_ && now - last_click_time_ <= double_click_interval() &&
                               std::abs(press.root_x - last_click_x_) <= 4 && std::abs(press.root_y - last_click_y_) <= 4;
            click_count_ = again ? click_count_ + 1 : 1;
            last_click_button_ = number;
            last_click_time_ = now;
            last_click_x_ = press.root_x;
            last_click_y_ = press.root_y;
        }
        event.click_count = click_count_;
        event.pressure = down ? 1.0f : 0.0f;
        deliver(*record, event);
    }

    void handle_motion(const xcb_motion_notify_event_t& motion) {
        auto* record = record_of(motion.event);
        if (!record) return;
        Event event;
        if (buttons_ & 1) event.type = event_type::left_mouse_dragged, event.button_number = 0;
        else if (buttons_ & 2) event.type = event_type::right_mouse_dragged, event.button_number = 1;
        else if (buttons_) event.type = event_type::other_mouse_dragged, event.button_number = 2;
        else event.type = event_type::mouse_moved;
        if (event.type == event_type::mouse_moved && !record->accepts_mouse_moved) {
            last_x_ = motion.root_x, last_y_ = motion.root_y, have_last_ = true;
            return;
        }
        fill_pointer(event, *record, motion.event_x, motion.event_y, motion.root_x, motion.root_y);
        if (have_last_) {
            event.delta_x = motion.root_x - last_x_;
            event.delta_y = motion.root_y - last_y_;
        }
        last_x_ = motion.root_x, last_y_ = motion.root_y, have_last_ = true;
        deliver(*record, event);
    }

    void handle_crossing(const xcb_enter_notify_event_t& crossing, bool entered) {
        auto* record = record_of(crossing.event);
        if (!record || record->mouse_inside == entered) return;
        record->mouse_inside = entered;
        Event event;
        event.type = entered ? event_type::mouse_entered : event_type::mouse_exited;
        fill_pointer(event, *record, crossing.event_x, crossing.event_y, crossing.root_x, crossing.root_y);
        deliver(*record, event);
    }

    void handle_configure(const xcb_configure_notify_event_t& configure) {
        auto* record = record_of(configure.window);
        if (!record) return;
        // The position on the root: the window manager reparents the window,
        // so the event's own coordinates are relative to its frame.
        int root_x = configure.x, root_y = configure.y;
        if (auto* reply = xcb_translate_coordinates_reply(
                connection_, xcb_translate_coordinates(connection_, record->xid, screen_->root, 0, 0), nullptr)) {
            root_x = reply->dst_x;
            root_y = reply->dst_y;
            std::free(reply);
        }
        const Rect content = from_x(root_x, root_y, configure.width, configure.height);
        const bool resized = content.size.width != record->content.size.width || content.size.height != record->content.size.height;
        const bool moved = content.origin.x != record->content.origin.x || content.origin.y != record->content.origin.y;
        record->content = content;
        if (resized) {
            Event event = notification(*record, Notification::resized);
            event.rect = content;
            deliver(*record, event);
        }
        if (moved) {
            Event event = notification(*record, Notification::moved);
            event.rect = content;
            deliver(*record, event);
        }
    }

    void handle(xcb_generic_event_t* generic) {
        switch (generic->response_type & 0x7f) {
        case XCB_EXPOSE: {
            const auto& expose = *reinterpret_cast<xcb_expose_event_t*>(generic);
            auto* record = record_of(expose.window);
            if (!record) return;
            Event event = notification(*record, Notification::expose);
            event.rect = {{static_cast<double>(expose.x), record->content.size.height - expose.y - expose.height},
                          {static_cast<double>(expose.width), static_cast<double>(expose.height)}};
            deliver(*record, event);
            return;
        }
        case XCB_CONFIGURE_NOTIFY: handle_configure(*reinterpret_cast<xcb_configure_notify_event_t*>(generic)); return;
        case XCB_KEY_PRESS: handle_key(*reinterpret_cast<xcb_key_press_event_t*>(generic), true); return;
        case XCB_KEY_RELEASE: {
            // A release immediately followed by a press of the same key at the
            // same time is auto-repeat: report only the press (is_repeat).
            const auto& release = *reinterpret_cast<xcb_key_release_event_t*>(generic);
            if (auto* next = xcb_poll_for_queued_event(connection_)) {
                const auto& press = *reinterpret_cast<xcb_key_press_event_t*>(next);
                if ((next->response_type & 0x7f) == XCB_KEY_PRESS && press.detail == release.detail && press.time == release.time) {
                    handle_key(press, true);
                    std::free(next);
                    return;
                }
                linux_gui::pending().push_front(next);
            }
            handle_key(release, false);
            return;
        }
        case XCB_BUTTON_PRESS: handle_button(*reinterpret_cast<xcb_button_press_event_t*>(generic), true); return;
        case XCB_BUTTON_RELEASE: handle_button(*reinterpret_cast<xcb_button_release_event_t*>(generic), false); return;
        case XCB_MOTION_NOTIFY: handle_motion(*reinterpret_cast<xcb_motion_notify_event_t*>(generic)); return;
        case XCB_ENTER_NOTIFY: handle_crossing(*reinterpret_cast<xcb_enter_notify_event_t*>(generic), true); return;
        case XCB_LEAVE_NOTIFY: handle_crossing(*reinterpret_cast<xcb_leave_notify_event_t*>(generic), false); return;
        case XCB_FOCUS_IN: {
            const auto& focus = *reinterpret_cast<xcb_focus_in_event_t*>(generic);
            auto* record = record_of(focus.event);
            if (!record || focus.mode == XCB_NOTIFY_MODE_GRAB || focus.mode == XCB_NOTIFY_MODE_UNGRAB) return;
            if (key_window_ == record->id) return;
            const bool was_inactive = key_window_ == 0;
            key_window_ = record->id;
            if (was_inactive) deliver_application(Notification::application_activated);
            deliver(*record, notification(*record, Notification::became_key));
            return;
        }
        case XCB_FOCUS_OUT: {
            const auto& focus = *reinterpret_cast<xcb_focus_out_event_t*>(generic);
            auto* record = record_of(focus.event);
            if (!record || focus.mode == XCB_NOTIFY_MODE_GRAB || focus.mode == XCB_NOTIFY_MODE_UNGRAB) return;
            if (key_window_ != record->id) return;
            key_window_ = 0;
            deliver(*record, notification(*record, Notification::resigned_key));
            deliver_application(Notification::application_deactivated);
            return;
        }
        case XCB_MAP_NOTIFY: {
            auto* record = record_of(reinterpret_cast<xcb_map_notify_event_t*>(generic)->window);
            if (!record) return;
            if (pending_focus_ == record->id) {
                xcb_set_input_focus(connection_, XCB_INPUT_FOCUS_PARENT, record->xid, XCB_CURRENT_TIME);
                xcb_flush(connection_);
                pending_focus_ = 0;
            }
            Event event = notification(*record, Notification::occlusion_changed);
            event.visible = true;
            deliver(*record, event);
            return;
        }
        case XCB_UNMAP_NOTIFY: {
            auto* record = record_of(reinterpret_cast<xcb_unmap_notify_event_t*>(generic)->window);
            if (!record) return;
            Event event = notification(*record, Notification::occlusion_changed);
            event.visible = false;
            deliver(*record, event);
            return;
        }
        case XCB_CLIENT_MESSAGE: {
            const auto& message = *reinterpret_cast<xcb_client_message_event_t*>(generic);
            auto* record = record_of(message.window);
            if (record && message.type == wm_protocols_ && message.data.data32[0] == wm_delete_window_)
                deliver(*record, notification(*record, Notification::close_requested));
            return;
        }
        case XCB_MAPPING_NOTIFY:
            if (xkb_state_ && keymap_) {   // the keymap changed: reload it
                const auto device = xkb_x11_get_core_keyboard_device_id(connection_);
                if (auto* map = xkb_x11_keymap_new_from_device(xkb_context_, connection_, device, XKB_KEYMAP_COMPILE_NO_FLAGS)) {
                    xkb_state_unref(xkb_state_);
                    xkb_keymap_unref(keymap_);
                    keymap_ = map;
                    xkb_state_ = xkb_x11_state_new_from_device(keymap_, connection_, device);
                }
                deliver_application(Notification::keyboard_layout_changed);
            }
            return;
        case XCB_SELECTION_REQUEST: answer_selection_request(*reinterpret_cast<xcb_selection_request_event_t*>(generic)); return;
        case XCB_SELECTION_CLEAR: {
            const auto& clear = *reinterpret_cast<xcb_selection_clear_event_t*>(generic);
            if (clear.selection != clipboard_ || clear.owner != clipboard_window_) return;
            owns_clipboard_ = false;   // another client took the clipboard
            if (!xfixes_) ++clipboard_changes_;
            return;
        }
        default: break;
        }
        const auto kind = generic->response_type & 0x7f;
        if (randr_event_ && kind == randr_event_ + XCB_RANDR_SCREEN_CHANGE_NOTIFY) {
            refresh_screens();
            deliver_application(Notification::screens_changed);
        } else if (xfixes_ && kind == xfixes_event_ + XCB_XFIXES_SELECTION_NOTIFY) {
            const auto& change = *reinterpret_cast<xcb_xfixes_selection_notify_event_t*>(generic);
            if (change.selection == clipboard_ && change.owner != clipboard_window_) {
                ++clipboard_changes_;
                last_clipboard_owner_ = change.owner;
            }
        }
    }

    xcb_connection_t* connection_{};
    xcb_screen_t* screen_{};
    bool failed_{};
    xcb_gcontext_t gc_{};
    xcb_atom_t wm_protocols_{}, wm_delete_window_{}, wm_change_state_{}, net_wm_name_{}, utf8_string_{}, net_wm_state_{},
        net_wm_state_above_{}, net_wm_state_fullscreen_{}, net_wm_state_maximized_vert_{}, net_wm_state_maximized_horz_{},
        net_wm_state_demands_attention_{}, net_active_window_{}, net_workarea_{}, clipboard_{}, targets_{}, incr_{},
        text_atom_{}, text_plain_utf8_{}, png_{}, transfer_{};
    // Extensions.
    std::uint8_t randr_event_{}, xfixes_event_{};
    bool randr_monitors_{}, xfixes_{};
    const xcb_render_query_pict_formats_reply_t* render_formats_{};
    bool render_checked_{};
    xcb_cursor_context_t* cursor_context_{};
    // Screens: the primary monitor's left and bottom edges in root pixels
    // (AppKit's origin).
    std::vector<MonitorRecord> monitors_;
    int primary_left_{}, primary_bottom_{};
    // Cursors.
    std::map<SystemCursor, xcb_cursor_t> system_cursors_;
    std::map<CursorId, xcb_cursor_t> custom_cursors_;
    CursorId next_cursor_{1000};
    xcb_cursor_t current_cursor_{XCB_NONE};
    bool cursor_visible_{true};
    // Clipboard.
    xcb_window_t clipboard_window_{};
    bool owns_clipboard_{};
    xcb_window_t last_clipboard_owner_{};
    std::vector<std::uint8_t> clipboard_png_;
    xkb_context* xkb_context_{};
    xkb_keymap* keymap_{};
    xkb_state* xkb_state_{};
    std::map<WindowId, WindowRecord> windows_;
    std::map<xcb_window_t, WindowId> by_xid_;
    std::vector<std::pair<WindowId, Rect>> invalidations_;
    std::shared_ptr<WindowEventSink> application_sink_;
    WindowId next_window_{1};
    WindowId key_window_{};
    WindowId pending_focus_{};
    std::uint64_t modifiers_{}, buttons_{};
    std::set<xcb_keycode_t> held_keys_;
    std::int64_t click_count_{}, last_click_button_{-1};
    double last_click_time_{};
    int last_click_x_{}, last_click_y_{};
    int last_x_{}, last_y_{};
    bool have_last_{};
    std::optional<std::string> clipboard_text_;
    std::optional<Image> clipboard_image_;
    std::int64_t clipboard_changes_{};
};

} // namespace

std::unique_ptr<WindowSystem> create_window_server() { return std::make_unique<X11WindowServer>(); }

namespace {

// EDID manufacturer and product codes (bytes 8-11) of a RandR output.
void read_edid(xcb_connection_t* connection, xcb_randr_output_t output, Monitor& monitor) {
    static constexpr char name[] = "EDID";
    auto* atom = xcb_intern_atom_reply(connection, xcb_intern_atom(connection, 1, sizeof(name) - 1, name), nullptr);
    if (!atom) return;
    const auto edid = atom->atom;
    std::free(atom);
    if (edid == XCB_NONE) return;
    auto* reply = xcb_randr_get_output_property_reply(
        connection, xcb_randr_get_output_property(connection, output, edid, XCB_GET_PROPERTY_TYPE_ANY, 0, 32, 0, 0), nullptr);
    if (!reply) return;
    if (reply->format == 8 && xcb_randr_get_output_property_data_length(reply) >= 12) {
        const auto* bytes = xcb_randr_get_output_property_data(reply);
        monitor.edid_vendor = static_cast<std::uint16_t>((bytes[8] << 8) | bytes[9]);
        monitor.edid_product = static_cast<std::uint16_t>(bytes[10] | (bytes[11] << 8));
    }
    std::free(reply);
}

} // namespace

Result<PixelSize> window_client_size(std::uintptr_t window) {
    linux_gui::AnyConnection any;
    if (!any.connection) return Error::unsupported;
    auto* reply = xcb_get_geometry_reply(any.connection, xcb_get_geometry(any.connection, static_cast<xcb_window_t>(window)), nullptr);
    if (!reply) return Error::not_found;
    const PixelSize size{reply->width, reply->height};
    std::free(reply);
    return size;
}

// RandR 1.5 monitors (with their outputs' EDID), else the root window.
Result<std::vector<Monitor>> monitors() {
    linux_gui::AnyConnection any;
    if (!any.connection) return Error::unsupported;
    std::vector<Monitor> result;
    if (const auto* randr = xcb_get_extension_data(any.connection, &xcb_randr_id); randr && randr->present) {
        auto* version = xcb_randr_query_version_reply(any.connection, xcb_randr_query_version(any.connection, 1, 5), nullptr);
        const bool has_monitors = version && (version->major_version > 1 || version->minor_version >= 5);
        std::free(version);
        if (has_monitors)
            if (auto* reply = xcb_randr_get_monitors_reply(any.connection, xcb_randr_get_monitors(any.connection, any.screen->root, 1), nullptr)) {
                for (auto it = xcb_randr_get_monitors_monitors_iterator(reply); it.rem; xcb_randr_monitor_info_next(&it)) {
                    Monitor monitor;
                    monitor.left = it.data->x;
                    monitor.top = it.data->y;
                    monitor.width = it.data->width;
                    monitor.height = it.data->height;
                    monitor.primary = it.data->primary;
                    if (xcb_randr_monitor_info_outputs_length(it.data) > 0)
                        read_edid(any.connection, xcb_randr_monitor_info_outputs(it.data)[0], monitor);
                    result.push_back(monitor);
                }
                std::free(reply);
            }
    }
    if (result.empty()) {
        Monitor monitor;
        monitor.width = any.screen->width_in_pixels;
        monitor.height = any.screen->height_in_pixels;
        result.push_back(monitor);
    }
    // One primary: RandR's, else the one at the desktop origin, else the first.
    if (std::none_of(result.begin(), result.end(), [](const Monitor& monitor) { return monitor.primary; })) {
        const auto origin = std::find_if(result.begin(), result.end(), [](const Monitor& monitor) { return monitor.left == 0 && monitor.top == 0; });
        (origin != result.end() ? *origin : result.front()).primary = true;
    }
    return result;
}

} // namespace alchemy::host::ui
