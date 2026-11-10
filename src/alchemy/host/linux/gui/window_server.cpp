// Linux host backend, area "ui": the window server over X11 (XCB), the
// counterpart of the Win32 one (include/alchemy/gui/window_server.hpp).
//
// - One X connection ($DISPLAY), opened by the first request. Windows are
//   top-level X windows the window manager decorates; AppKit's bottom-left
//   screen points are X root pixels flipped against the root height (one
//   screen, backing scale 1 for now).
// - Events are read in dispatch_messages and delivered to the window's sink
//   on the main thread; an exception from a sink is rethrown after the
//   batch. Expose, configure (move and resize), focus, map/unmap and the
//   WM_DELETE_WINDOW request become notifications; keys go through
//   xkbcommon (characters with and without modifiers, macOS key codes from
//   the evdev code); buttons 4-7 are wheel steps.
// - present_bitmap puts the BGRA pixels with PutImage (the X server's 24/32-bit
//   TrueColor layout is BGRX on little-endian hosts), in strips that fit the
//   maximum request size.
// - Not yet: XRandR screens and HiDPI, real cursors, the X clipboard (the
//   clipboard is process-local), drag and drop, IME, status items.
#include "alchemy/host/linux/gui/x11.hpp"
#include "alchemy/host/ui.hpp"
#include "alchemy/gui/runloop.hpp"

#include <xcb/xcb_icccm.h>
#include <xkbcommon/xkbcommon-x11.h>
#include <xkbcommon/xkbcommon.h>

#include <algorithm>
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
        const std::uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
        const std::uint32_t values[] = {
            screen_->white_pixel,
            // Borderless windows at a level above normal (menus, pop-ups,
            // tool tips) are placed by AppKit, not the window manager.
            (description.style_mask == style::borderless && description.level > 0) ? 1u : 0u,
            XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_KEY_PRESS |
                XCB_EVENT_MASK_KEY_RELEASE | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
                XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW |
                XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_PROPERTY_CHANGE};
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
    ScreenId window_screen(WindowId window) const override { find(window, "window_screen"); return 1; }
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
        const Point point{static_cast<double>(reply->root_x), screen_height() - reply->root_y};
        std::free(reply);
        return point;
    }
    double double_click_interval() const override { return 0.5; }
    double key_repeat_delay() const override { return 0.5; }
    double key_repeat_interval() const override { return 1.0 / 30; }

    // ---- cursors (the default arrow until cursor themes are wired) ----
    CursorId system_cursor(SystemCursor cursor) override { return static_cast<CursorId>(cursor) + 1; }
    CursorId create_cursor(const Image&, Point) override { return 1; }
    void destroy_cursor(CursorId) override {}
    void set_cursor(CursorId) override {}
    void set_cursor_visible(bool) override {}
    void warp_cursor(Point screen_location) override {
        require_open("warp_cursor");
        xcb_warp_pointer(connection_, XCB_NONE, screen_->root, 0, 0, 0, 0, static_cast<std::int16_t>(screen_location.x),
                         static_cast<std::int16_t>(screen_height() - screen_location.y));
        xcb_flush(connection_);
    }
    void associate_mouse_and_cursor(bool) override {}
    void confine_cursor(WindowId, std::optional<Rect>) override {}

    // ---- clipboard (process-local for now) ----
    std::optional<std::string> clipboard_text() const override { return clipboard_text_; }
    void set_clipboard_text(std::string_view text) override {
        clipboard_text_ = std::string(text);
        clipboard_image_.reset();
        ++clipboard_changes_;
    }
    std::optional<Image> clipboard_image() const override { return clipboard_image_; }
    void set_clipboard_image(const Image& image) override {
        clipboard_image_ = image;
        clipboard_text_.reset();
        ++clipboard_changes_;
    }
    std::int64_t clipboard_change_count() const override { return clipboard_changes_; }

    // ---- screens and keyboard ----
    std::vector<ScreenInfo> screens() const override {
        if (!const_cast<X11WindowServer*>(this)->open()) return {};
        ScreenInfo info;
        info.id = 1;
        info.frame = {{0, 0}, {static_cast<double>(screen_->width_in_pixels), static_cast<double>(screen_->height_in_pixels)}};
        info.visible_frame = info.frame;
        info.pixel_width = screen_->width_in_pixels;
        info.pixel_height = screen_->height_in_pixels;
        info.millimeters_width = screen_->width_in_millimeters;
        info.millimeters_height = screen_->height_in_millimeters;
        info.refresh_rate = 60;
        info.name = "X11 screen";
        info.primary = true;
        return {info};
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

    double screen_height() const { return screen_ ? screen_->height_in_pixels : 0; }

    XRect to_x(const Rect& rect) const {
        return {static_cast<int>(std::lround(rect.origin.x)),
                static_cast<int>(std::lround(screen_height() - rect.origin.y - rect.size.height)),
                static_cast<int>(std::lround(rect.size.width)), static_cast<int>(std::lround(rect.size.height))};
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
        event.screen_location = {static_cast<double>(root_x), screen_height() - root_y};
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
        const Rect content{{static_cast<double>(root_x), screen_height() - root_y - configure.height},
                           {static_cast<double>(configure.width), static_cast<double>(configure.height)}};
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
        default: return;
        }
    }

    xcb_connection_t* connection_{};
    xcb_screen_t* screen_{};
    bool failed_{};
    xcb_gcontext_t gc_{};
    xcb_atom_t wm_protocols_{}, wm_delete_window_{}, wm_change_state_{}, net_wm_name_{}, utf8_string_{}, net_wm_state_{},
        net_wm_state_above_{}, net_wm_state_fullscreen_{}, net_wm_state_maximized_vert_{}, net_wm_state_maximized_horz_{},
        net_wm_state_demands_attention_{}, net_active_window_{};
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

Result<PixelSize> window_client_size(std::uintptr_t window) {
    auto* connection = linux_gui::connection();
    if (!connection) return Error::not_found;
    auto* reply = xcb_get_geometry_reply(connection, xcb_get_geometry(connection, static_cast<xcb_window_t>(window)), nullptr);
    if (!reply) return Error::not_found;
    const PixelSize size{reply->width, reply->height};
    std::free(reply);
    return size;
}

Result<std::vector<Monitor>> monitors() {
    auto* connection = linux_gui::connection();
    if (!connection) return Error::unsupported;
    const auto* screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
    Monitor monitor;
    monitor.width = screen->width_in_pixels;
    monitor.height = screen->height_in_pixels;
    monitor.primary = true;
    return std::vector<Monitor>{monitor};
}

} // namespace alchemy::host::ui
