#pragma once

// In-memory WindowServer for AppKit unit tests (owner: A4). Behaves like the
// Win32 window server's contract: sink callbacks run synchronously, from
// dispatch_messages() for queued events and from the calls that change a
// window (show, set_content_rect, ...) as Win32 sends WM_ACTIVATE / WM_SIZE
// synchronously. Records every request so tests can assert on it.

#include "alchemy/gui/window_server.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace alchemy::gui::testing {

class FakeWindowServer final : public WindowServer {
public:
    static constexpr double title_bar = 28;   // frame = content + title bar (titled windows)
    static constexpr double scale = 2;

    struct Window {
        WindowDesc desc;
        std::shared_ptr<WindowEventSink> sink;
        Rect content;
        std::uint64_t style{};
        WindowLevel level{};
        std::string title;
        Size minimum, maximum;
        bool visible{}, accepts_mouse_moved{}, accepts_drops{}, text_input{}, full_screen{}, zoomed{};
        bool capture_excluded{};
        int drags{};                          // begin_window_drag calls
    };

    std::map<WindowId, Window> windows;
    std::map<WindowId, double> scales;        // per-window backing scale (default `scale`)
    std::shared_ptr<WindowEventSink> application_sink;
    std::vector<WindowId> destroyed;
    std::deque<Event> pending;
    WindowId key{};
    WindowId next_id{1};
    std::uint64_t flags{}, buttons{};
    Point mouse;
    std::vector<CursorId> cursors_set;
    std::vector<std::pair<Image, Point>> cursors_created;
    int cursor_hide_count{};
    std::vector<Point> warps;
    std::optional<bool> associated;
    std::optional<std::string> clipboard;
    std::optional<Image> clipboard_picture;
    std::int64_t change_count{};
    int beeps{}, attention{}, discards{};
    // Status items: their images and tool tips.
    std::map<StatusItemId, std::pair<Image, std::string>> status_items;
    StatusItemId next_status_item{1};

    Window& at(WindowId id) {
        const auto found = windows.find(id);
        if (found == windows.end()) throw std::runtime_error("fake window server: unknown window " + std::to_string(id));
        return found->second;
    }
    const Window& at(WindowId id) const { return const_cast<FakeWindowServer*>(this)->at(id); }
    void notify(WindowId id, Notification notification, Rect rect = {}) {
        Event event;
        event.kind = EventKind::notification;
        event.notification = notification;
        event.window = id;
        event.owner = at(id).desc.owner;
        event.rect = rect;
        at(id).sink->handle_event(event);
    }
    void queue(Event event) {
        if (event.window && windows.contains(event.window)) event.owner = at(event.window).desc.owner;
        pending.push_back(std::move(event));
    }
    // The window moved to a monitor with another DPI (WM_DPICHANGED).
    void change_backing_scale(WindowId id, double value) {
        scales[id] = value;
        Event event;
        event.kind = EventKind::notification;
        event.notification = Notification::backing_changed;
        event.window = id;
        event.owner = at(id).desc.owner;
        event.backing_scale = value;
        at(id).sink->handle_event(event);
    }
    // An application-wide event (Event::window == 0), delivered synchronously.
    void notify_application(Notification notification) {
        if (!application_sink) throw std::runtime_error("fake window server: no application sink");
        Event event;
        event.kind = EventKind::notification;
        event.notification = notification;
        application_sink->handle_event(event);
    }
    void set_application_sink(std::shared_ptr<WindowEventSink> sink) override { application_sink = std::move(sink); }
    // A file drag over a window (the OLE drop target): delivers drag_entered /
    // drag_updated / drag_exited / files_dropped synchronously and, after
    // drag_entered / drag_updated, returns the sink's drag_operation() answer.
    std::uint64_t drag(WindowId id, Notification notification, Point location, std::vector<std::string> paths,
                       std::uint64_t operations) {
        Event event;
        event.kind = EventKind::notification;
        event.notification = notification;
        event.window = id;
        event.owner = at(id).desc.owner;
        event.location = location;
        event.screen_location = {at(id).content.origin.x + location.x, at(id).content.origin.y + location.y};
        event.paths = std::move(paths);
        event.drag_operations = operations;
        const auto sink = at(id).sink;
        sink->handle_event(event);
        if (notification == Notification::drag_entered || notification == Notification::drag_updated)
            return sink->drag_operation(id);
        return drag_operation::none;
    }

    WindowId create_window(const WindowDesc& description, std::shared_ptr<WindowEventSink> sink) override {
        const auto id = next_id++;
        Window window;
        window.desc = description;
        window.sink = std::move(sink);
        window.content = description.content_rect;
        window.style = description.style_mask;
        window.visible = description.visible;
        windows.emplace(id, std::move(window));
        return id;
    }
    void destroy_window(WindowId id) override {
        static_cast<void>(at(id));
        windows.erase(id);
        z_order.erase(std::remove(z_order.begin(), z_order.end(), id), z_order.end());
        destroyed.push_back(id);
        if (key == id) key = 0;
    }
    // Shown windows front to back (the Win32 z-order).
    std::vector<WindowId> z_order;
    // Like WM_SHOWWINDOW: occlusion_changed while the visibility changes,
    // synchronously, from inside the call that changes it.
    void set_visible(WindowId id, bool visible) {
        auto& window = at(id);
        if (!visible) z_order.erase(std::remove(z_order.begin(), z_order.end(), id), z_order.end());
        else if (std::find(z_order.begin(), z_order.end(), id) == z_order.end()) z_order.insert(z_order.begin(), id);
        if (window.visible == visible) return;
        window.visible = visible;
        Event event;
        event.kind = EventKind::notification;
        event.notification = Notification::occlusion_changed;
        event.window = id;
        event.owner = window.desc.owner;
        event.visible = visible;
        window.sink->handle_event(event);
    }
    void place(WindowId id, std::size_t index) {
        z_order.erase(std::remove(z_order.begin(), z_order.end(), id), z_order.end());
        z_order.insert(z_order.begin() + static_cast<std::ptrdiff_t>(std::min(index, z_order.size())), id);
    }
    void show(WindowId id, ShowCommand command) override {
        static_cast<void>(at(id));
        switch (command) {
        case ShowCommand::make_key_and_order_front:
            set_visible(id, true);
            place(id, 0);
            if (key != id) {
                const auto old = key;
                key = id;
                if (old && windows.contains(old)) notify(old, Notification::resigned_key);
                notify(id, Notification::became_key);
            }
            break;
        case ShowCommand::order_front: set_visible(id, true); place(id, 0); break;
        case ShowCommand::order_back:
            set_visible(id, true);
            place(id, z_order.size());
            break;
        case ShowCommand::order_out:
            set_visible(id, false);
            if (key == id) { key = 0; notify(id, Notification::resigned_key); }
            break;
        }
    }
    std::map<WindowId, WindowId> parents;   // set_parent: child -> parent
    void set_parent(WindowId id, WindowId parent) override {
        static_cast<void>(at(id));
        if (parent) { static_cast<void>(at(parent)); parents[id] = parent; }
        else parents.erase(id);
    }
    void order_relative(WindowId id, WindowId relative, bool above) override {
        static_cast<void>(at(relative));
        set_visible(id, true);
        z_order.erase(std::remove(z_order.begin(), z_order.end(), id), z_order.end());
        const auto found = std::find(z_order.begin(), z_order.end(), relative);
        place(id, static_cast<std::size_t>(found - z_order.begin()) + (above ? 0 : 1));
    }
    void set_title(WindowId id, std::string_view title) override { at(id).title = title; }
    void set_style(WindowId id, std::uint64_t style_mask) override { at(id).style = style_mask; }
    void set_level(WindowId id, WindowLevel level) override { at(id).level = level; }
    void set_content_rect(WindowId id, Rect rect) override {
        auto& window = at(id);
        const auto old = window.content;
        window.content = rect;
        if (old.size.width != rect.size.width || old.size.height != rect.size.height) notify(id, Notification::resized, rect);
        else if (old.origin.x != rect.origin.x || old.origin.y != rect.origin.y) notify(id, Notification::moved, rect);
    }
    Rect content_rect(WindowId id) const override { return at(id).content; }
    Rect frame_rect(WindowId id) const override { return frame_for_content(at(id).content, at(id).style); }
    Rect frame_for_content(Rect rect, std::uint64_t style_mask) const override {
        if (style_mask & style::titled) rect.size.height += title_bar;
        return rect;
    }
    Rect content_for_frame(Rect rect, std::uint64_t style_mask) const override {
        if (style_mask & style::titled) rect.size.height -= title_bar;
        return rect;
    }
    void set_content_limits(WindowId id, Size minimum, Size maximum) override { at(id).minimum = minimum; at(id).maximum = maximum; }
    void miniaturize(WindowId id, bool miniaturized) override {
        notify(id, miniaturized ? Notification::miniaturized : Notification::deminiaturized);
    }
    void zoom(WindowId id) override { at(id).zoomed = !at(id).zoomed; }
    void begin_window_drag(WindowId id) override { ++at(id).drags; }
    void set_capture_excluded(WindowId id, bool excluded) override { at(id).capture_excluded = excluded; }
    StatusItemId create_status_item(const Image& image, std::string_view tool_tip) override {
        const auto id = next_status_item++;
        status_items[id] = {image, std::string(tool_tip)};
        return id;
    }
    void update_status_item(StatusItemId id, const Image& image, std::string_view tool_tip) override {
        status_items.at(id) = {image, std::string(tool_tip)};
    }
    void remove_status_item(StatusItemId id) override { status_items.erase(id); }
    // A click on a status item (button 0 left, 1 right), delivered synchronously.
    void click_status_item(StatusItemId id, std::int64_t button, Point screen_location) {
        if (!application_sink) throw std::runtime_error("fake window server: no application sink");
        Event event;
        event.kind = EventKind::notification;
        event.notification = Notification::status_item_clicked;
        event.status_item = id;
        event.button_number = button;
        event.screen_location = screen_location;
        application_sink->handle_event(event);
    }
    void set_full_screen(WindowId id, bool full_screen) override {
        at(id).full_screen = full_screen;
        notify(id, full_screen ? Notification::entered_full_screen : Notification::exited_full_screen);
    }
    void set_accepts_mouse_moved(WindowId id, bool accepts) override { at(id).accepts_mouse_moved = accepts; }
    void set_accepts_file_drops(WindowId id, bool accepts) override { at(id).accepts_drops = accepts; }
    void invalidate(WindowId id, std::optional<Rect> rect) override {
        Event event;
        event.kind = EventKind::notification;
        event.notification = Notification::expose;
        event.window = id;
        event.rect = rect.value_or(Rect{{0, 0}, at(id).content.size});
        queue(event);
    }
    WindowId key_window() const override { return key; }
    bool is_visible(WindowId id) const override { return at(id).visible; }
    bool window_exists(WindowId id) const override { return windows.contains(id); }
    double backing_scale(WindowId id) const override {
        const auto found = scales.find(id);
        return found == scales.end() ? scale : found->second;
    }
    ScreenId window_screen(WindowId) const override { return 1; }
    Size content_pixel_size(WindowId id) const override {
        const auto size = at(id).content.size;
        return {size.width * backing_scale(id), size.height * backing_scale(id)};
    }
    // Tests may give a window a real (hidden) HWND, e.g. for modal dialogs.
    std::map<WindowId, void*> native;
    void* native_handle(WindowId id) const override {
        const auto found = native.find(id);
        return found == native.end() ? nullptr : found->second;
    }

    std::uint64_t modifier_flags() const override { return flags; }
    std::uint64_t pressed_mouse_buttons() const override { return buttons; }
    Point mouse_location() const override { return mouse; }
    double double_click_interval() const override { return 0.5; }
    double key_repeat_delay() const override { return 0.4; }
    double key_repeat_interval() const override { return 0.05; }

    CursorId system_cursor(SystemCursor cursor) override { return 100 + static_cast<CursorId>(cursor); }
    CursorId create_cursor(const Image& image, Point hot_spot) override {
        cursors_created.emplace_back(image, hot_spot);
        return 500 + cursors_created.size();
    }
    void destroy_cursor(CursorId) override {}
    void set_cursor(CursorId cursor) override { cursors_set.push_back(cursor); }
    void set_cursor_visible(bool visible) override { cursor_hide_count += visible ? -1 : 1; }
    void warp_cursor(Point location) override { warps.push_back(location); mouse = location; }
    void associate_mouse_and_cursor(bool value) override { associated = value; }
    void confine_cursor(WindowId, std::optional<Rect>) override {}

    std::optional<std::string> clipboard_text() const override { return clipboard; }
    void set_clipboard_text(std::string_view text) override { clipboard = std::string(text); ++change_count; }
    std::optional<Image> clipboard_image() const override { return clipboard_picture; }
    void set_clipboard_image(const Image& image) override { clipboard_picture = image; ++change_count; }
    std::int64_t clipboard_change_count() const override { return change_count; }

    std::vector<ScreenInfo> screens() const override {
        ScreenInfo info;
        info.id = 1;
        info.frame = {{0, 0}, {1440, 900}};
        info.visible_frame = {{0, 40}, {1440, 860}};
        info.backing_scale = scale;
        info.refresh_rate = 120;
        info.pixel_width = 2880;
        info.pixel_height = 1800;
        info.millimeters_width = 300;
        info.millimeters_height = 190;
        info.name = "Fake Display";
        info.primary = true;
        return {info};
    }

    KeyboardLayout keyboard_layout() const override { return {"com.apple.keylayout.US", "U.S.", "en"}; }
    // kVK_ANSI_A / S produce letters; Option+E (kVK_ANSI_E) is the acute
    // dead key, completing on the next letter.
    std::string translate_key(std::uint16_t key_code, std::uint64_t modifiers, DeadKeyState& state) const override {
        const bool shift = (modifiers & modifier::shift) != 0;
        if (key_code == 0x0e && (modifiers & modifier::option)) { state.value = 1; return {}; }
        std::string base;
        if (key_code == 0x00) base = shift ? "A" : "a";
        else if (key_code == 0x01) base = shift ? "S" : "s";
        else if (key_code == 0x24) base = "\r";
        if (state.value == 1 && base == "a") { state.value = 0; return "\xc3\xa1"; }   // U+00E1
        state.value = 0;
        return base;
    }
    void set_text_input_enabled(WindowId id, bool enabled) override { at(id).text_input = enabled; }
    void discard_marked_text(WindowId) override { ++discards; }

    bool dispatch_messages() override {
        bool any{};
        while (!pending.empty()) {
            const auto event = pending.front();
            pending.pop_front();
            if (event.window == 0 && application_sink) { any = true; application_sink->handle_event(event); continue; }
            if (!windows.contains(event.window)) continue;
            any = true;
            at(event.window).sink->handle_event(event);
        }
        return any;
    }
    void beep() override { ++beeps; }
    void request_attention(bool) override { ++attention; }

    // Software presentation: each window's "screen" (the content area in
    // physical pixels, BGRA premultiplied, top-down) receives the presented
    // rectangles; tests read it back with pixel().
    struct Screen {
        std::uint32_t width{}, height{};
        std::vector<std::uint8_t> bgra;
        int presents{};
        std::optional<Rect> last_rect;
    };
    std::map<WindowId, Screen> screens_by_window;
    void present_bitmap(WindowId id, const BitmapView& pixels, std::optional<Rect> rect) override {
        static_cast<void>(at(id));
        auto& screen = screens_by_window[id];
        if (screen.width != pixels.width || screen.height != pixels.height) {
            screen.width = pixels.width; screen.height = pixels.height;
            screen.bgra.assign(static_cast<std::size_t>(pixels.width) * pixels.height * 4, 0);
        }
        const auto s = backing_scale(id);
        const double height_points = pixels.height / s;
        std::int64_t x0 = 0, y0 = 0, x1 = pixels.width, y1 = pixels.height;
        if (rect) {
            x0 = static_cast<std::int64_t>(rect->origin.x * s);
            x1 = static_cast<std::int64_t>((rect->origin.x + rect->size.width) * s + 0.999);
            y0 = static_cast<std::int64_t>((height_points - rect->origin.y - rect->size.height) * s);
            y1 = static_cast<std::int64_t>((height_points - rect->origin.y) * s + 0.999);
        }
        x0 = std::max<std::int64_t>(0, x0); y0 = std::max<std::int64_t>(0, y0);
        x1 = std::min<std::int64_t>(static_cast<std::int64_t>(pixels.width), x1);
        y1 = std::min<std::int64_t>(static_cast<std::int64_t>(pixels.height), y1);
        for (auto y = y0; y < y1; ++y)
            for (auto x = x0; x < x1; ++x)
                for (int c = 0; c < 4; ++c)
                    screen.bgra[(static_cast<std::size_t>(y) * pixels.width + x) * 4 + c] =
                        pixels.bgra[static_cast<std::size_t>(y) * pixels.stride + x * 4 + c];
        ++screen.presents;
        screen.last_rect = rect;
    }
    // BGRA of the presented pixel at content point (x, y) (y up, points).
    std::array<std::uint8_t, 4> pixel(WindowId id, double x, double y) const {
        const auto found = screens_by_window.find(id);
        if (found == screens_by_window.end()) throw std::runtime_error("fake window server: nothing presented");
        const auto& screen = found->second;
        const auto s = backing_scale(id);
        const auto px = static_cast<std::int64_t>(x * s);
        const auto py = static_cast<std::int64_t>(screen.height - y * s);
        if (px < 0 || py < 0 || px >= static_cast<std::int64_t>(screen.width) || py >= static_cast<std::int64_t>(screen.height)) throw std::runtime_error("fake window server: pixel outside");
        std::array<std::uint8_t, 4> value{};
        for (int c = 0; c < 4; ++c) value[c] = screen.bgra[(static_cast<std::size_t>(py) * screen.width + px) * 4 + c];
        return value;
    }
};

} // namespace alchemy::gui::testing
