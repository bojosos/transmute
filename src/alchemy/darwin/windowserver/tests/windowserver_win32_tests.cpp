// Win32 window server (owner: A3): hidden windows driven by synthesized
// messages, the WndProc exception fence, geometry round trips, clipboard,
// cursors, screens and keyboard translation. Windows stay hidden (nothing
// appears on the desktop); the clipboard's previous text is restored.
#include "alchemy/host/win32/gui/keymap.hpp"
#include "alchemy/host/win32/gui/window_server.hpp"

#include <ole2.h>
#include <shlobj.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace gui = alchemy::gui;
namespace win32 = alchemy::gui::win32;
namespace kvk = alchemy::gui::win32::kvk;

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

bool close_to(double a, double b, double tolerance) { return std::abs(a - b) <= tolerance; }

// Not derived from std::exception: the fence must catch anything (the fx
// engine unwinds stopping threads with its own type).
struct GuestUnwind {
    int code;
};

class RecordingSink final : public gui::WindowEventSink {
public:
    std::vector<gui::Event> events;
    std::function<void(const gui::Event&)> on_event;
    std::function<gui::Size(gui::Size)> resize;
    std::optional<gui::Rect> caret;
    std::uint64_t accepted_drag{gui::drag_operation::every};
    int drag_queries{};

    void handle_event(const gui::Event& event) override {
        events.push_back(event);
        if (on_event) on_event(event);
    }
    gui::Size will_resize(gui::WindowId, gui::Size proposed) override { return resize ? resize(proposed) : proposed; }
    std::optional<gui::Rect> text_caret_rect(gui::WindowId) override { return caret; }
    std::uint64_t drag_operation(gui::WindowId) override {
        ++drag_queries;
        return accepted_drag;
    }

    const gui::Event* find_input(std::uint32_t type) const {
        for (const auto& event : events)
            if (event.kind == gui::EventKind::input && event.type == type) return &event;
        return nullptr;
    }
    const gui::Event* find_notification(gui::Notification notification) const {
        for (const auto& event : events)
            if (event.kind == gui::EventKind::notification && event.notification == notification) return &event;
        return nullptr;
    }
    std::size_t count_notification(gui::Notification notification) const {
        std::size_t count = 0;
        for (const auto& event : events)
            if (event.kind == gui::EventKind::notification && event.notification == notification) ++count;
        return count;
    }
};

void pump(gui::WindowServer& server) {
    for (int round = 0; round < 8 && server.dispatch_messages(); ++round) {
    }
}

LPARAM key_lparam(unsigned scan, bool extended, bool up, bool repeat = false) {
    LPARAM value = 1 | (static_cast<LPARAM>(scan & 0xff) << 16);
    if (extended) value |= 1 << 24;
    if (up) value |= (1LL << 30) | (1LL << 31);
    else if (repeat) value |= 1LL << 30;
    return value;
}

void test_screens(win32::Win32WindowServer& server) {
    const auto screens = server.screens();
    check(!screens.empty(), "screens: at least one display");
    if (screens.empty()) return;
    check(screens[0].primary, "screens: [0] is primary");
    check(close_to(screens[0].frame.origin.x, 0, 1e-9) && close_to(screens[0].frame.origin.y, 0, 1e-9),
          "screens: primary frame origin is 0,0");
    for (const auto& screen : screens) {
        check(screen.backing_scale >= 1.0 && screen.backing_scale <= 5.0, "screens: backing scale");
        check(close_to(screen.frame.size.width * screen.backing_scale, screen.pixel_width, 0.5), "screens: point size");
        check(screen.visible_frame.size.height <= screen.frame.size.height + 1e-6, "screens: visible frame");
        check(screen.id != 0 && !screen.name.empty(), "screens: id and name");
    }
}

void test_window_lifecycle(win32::Win32WindowServer& server) {
    auto sink = std::make_shared<RecordingSink>();
    gui::WindowDesc description;
    description.content_rect = {{120, 140}, {400, 300}};
    description.title = "Alchemy window server test \xe2\x9c\x93";
    description.owner = 0x1234;
    description.accepts_mouse_moved = true;
    const auto window = server.create_window(description, sink);
    check(window != 0, "create_window returns an id");
    const auto hwnd = static_cast<HWND>(server.native_handle(window));
    check(IsWindow(hwnd) && !IsWindowVisible(hwnd), "window exists hidden");
    check(!server.is_visible(window), "is_visible false while hidden");
    check(sink->events.empty(), "no notifications during creation");
    const auto scale = server.backing_scale(window);
    const auto tolerance = 1.0 / scale + 1e-6;

    auto content = server.content_rect(window);
    check(close_to(content.origin.x, 120, tolerance) && close_to(content.origin.y, 140, tolerance) &&
              close_to(content.size.width, 400, tolerance) && close_to(content.size.height, 300, tolerance),
          "content rect round trip at creation");
    const auto pixels = server.content_pixel_size(window);
    check(close_to(pixels.width, 400 * scale, 1.0) && close_to(pixels.height, 300 * scale, 1.0), "content pixel size");

    // Frame <-> content conversions agree with the real window.
    const auto frame = server.frame_rect(window);
    const auto expected_frame = server.frame_for_content(content, description.style_mask);
    check(close_to(frame.origin.x, expected_frame.origin.x, tolerance) &&
              close_to(frame.origin.y, expected_frame.origin.y, tolerance) &&
              close_to(frame.size.width, expected_frame.size.width, tolerance) &&
              close_to(frame.size.height, expected_frame.size.height, tolerance),
          "frame_for_content matches the window frame");
    const auto back = server.content_for_frame(frame, description.style_mask);
    check(close_to(back.size.width, content.size.width, tolerance) && close_to(back.origin.y, content.origin.y, tolerance),
          "content_for_frame inverts frame_for_content");
    check(frame.size.height > content.size.height, "titled frame includes the title bar");

    // Programmatic resize: resized and moved notifications, synchronously.
    server.set_content_rect(window, {{200, 150}, {500, 360}});
    content = server.content_rect(window);
    check(close_to(content.origin.x, 200, tolerance) && close_to(content.origin.y, 150, tolerance) &&
              close_to(content.size.width, 500, tolerance) && close_to(content.size.height, 360, tolerance),
          "set_content_rect round trip");
    const auto* resized = sink->find_notification(gui::Notification::resized);
    check(resized != nullptr && close_to(resized->rect.size.width, 500, tolerance), "resized notification");
    check(sink->find_notification(gui::Notification::moved) != nullptr, "moved notification");
    check(resized != nullptr && resized->window == window && resized->owner == 0x1234, "event window and owner");

    // Keyboard: posted key messages carry scan codes.
    sink->events.clear();
    PostMessageW(hwnd, WM_KEYDOWN, 'A', key_lparam(0x1e, false, false));
    PostMessageW(hwnd, WM_KEYDOWN, 'A', key_lparam(0x1e, false, false, true));
    PostMessageW(hwnd, WM_KEYUP, 'A', key_lparam(0x1e, false, true));
    PostMessageW(hwnd, WM_KEYDOWN, VK_LEFT, key_lparam(0x4b, true, false));
    PostMessageW(hwnd, WM_KEYDOWN, VK_F5, key_lparam(0x3f, false, false));
    pump(server);
    std::vector<const gui::Event*> keys;
    for (const auto& event : sink->events)
        if (event.kind == gui::EventKind::input) keys.push_back(&event);
    check(keys.size() == 5, "five key events, got " + std::to_string(keys.size()));
    if (keys.size() == 5) {
        check(keys[0]->type == gui::event_type::key_down && keys[0]->key_code == kvk::ansi_a && !keys[0]->is_repeat,
              "A key down");
        check(!keys[0]->characters.empty() && !keys[0]->characters_ignoring_modifiers.empty(), "A has characters");
        check(keys[1]->is_repeat, "auto-repeat flagged");
        check(keys[2]->type == gui::event_type::key_up && keys[2]->key_code == kvk::ansi_a, "A key up");
        check(keys[3]->key_code == kvk::left_arrow && keys[3]->characters == "\xef\x9c\x82" &&
                  (keys[3]->modifier_flags & gui::modifier::function) &&
                  (keys[3]->modifier_flags & gui::modifier::numeric_pad),
              "left arrow: NSLeftArrowFunctionKey and function|numericPad");
        check(keys[4]->key_code == kvk::f5 && keys[4]->characters == "\xef\x9c\x88", "F5: NSF5FunctionKey");
    }

    // Text outside key bindings (injected Unicode), including a surrogate
    // pair.
    sink->events.clear();
    PostMessageW(hwnd, WM_CHAR, L'x', 1);
    PostMessageW(hwnd, WM_CHAR, 0xd83d, 1);
    PostMessageW(hwnd, WM_CHAR, 0xde00, 1);
    pump(server);
    check(sink->events.size() == 2 && sink->events[0].kind == gui::EventKind::text && sink->events[0].text == "x" &&
              sink->events[1].text == "\xf0\x9f\x98\x80",
          "WM_CHAR text events");

    // Mouse: location in content points (bottom-left origin), click counts.
    sink->events.clear();
    RECT client{};
    GetClientRect(hwnd, &client);
    PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(10, 20));
    PostMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(10, 20));
    PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(11, 20));
    PostMessageW(hwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(31, 25));
    PostMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(31, 25));
    PostMessageW(hwnd, WM_RBUTTONDOWN, MK_RBUTTON, MAKELPARAM(50, 60));
    PostMessageW(hwnd, WM_RBUTTONUP, 0, MAKELPARAM(50, 60));
    pump(server);
    const auto* down = sink->find_input(gui::event_type::left_mouse_down);
    check(down != nullptr && close_to(down->location.x, 10 / scale, 1e-9) &&
              close_to(down->location.y, (client.bottom - 20) / scale, 1e-9) && down->click_count == 1 &&
              down->button_number == 0 && (down->pressed_buttons & 1) && down->pressure == 1.0f,
          "left mouse down");
    std::int64_t second_click = 0;
    for (const auto& event : sink->events)
        if (event.type == gui::event_type::left_mouse_down) second_click = event.click_count;
    check(second_click == 2, "double click counted");
    const auto* dragged = sink->find_input(gui::event_type::left_mouse_dragged);
    check(dragged != nullptr && close_to(dragged->delta_x, 20 / scale, 1e-9) && close_to(dragged->delta_y, 5 / scale, 1e-9),
          "left mouse dragged with deltas (y down positive)");
    const auto* right = sink->find_input(gui::event_type::right_mouse_down);
    check(right != nullptr && right->button_number == 1, "right mouse down");
    check(sink->find_input(gui::event_type::mouse_entered) != nullptr, "mouse entered");
    check(GetCapture() == nullptr, "capture released after the last button");

    // Scroll wheel: notches are lines; sub-notch deltas are precise with
    // phases.
    sink->events.clear();
    POINT origin{0, 0};
    ClientToScreen(hwnd, &origin);
    PostMessageW(hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM(origin.x + 5, origin.y + 5));
    PostMessageW(hwnd, WM_MOUSEHWHEEL, MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM(origin.x + 5, origin.y + 5));
    pump(server);
    check(sink->events.size() == 2 && sink->events[0].type == gui::event_type::scroll_wheel &&
              close_to(sink->events[0].delta_y, 1, 1e-9) && !sink->events[0].precise_scrolling &&
              close_to(sink->events[1].delta_x, -1, 1e-9),
          "wheel notches");
    sink->events.clear();
    PostMessageW(hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, 30), MAKELPARAM(origin.x + 5, origin.y + 5));
    PostMessageW(hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, 30), MAKELPARAM(origin.x + 5, origin.y + 5));
    pump(server);
    const auto deadline = GetTickCount64() + 2000;
    while (sink->events.size() < 3 && GetTickCount64() < deadline) {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
        pump(server);
    }
    check(sink->events.size() == 3 && sink->events[0].precise_scrolling &&
              sink->events[0].scroll_phase == gui::phase::began && sink->events[1].scroll_phase == gui::phase::changed &&
              sink->events[2].scroll_phase == gui::phase::ended &&
              close_to(sink->events[0].delta_y, 30 * win32::precise_points_per_wheel_unit, 1e-9),
          "precise scrolling phases");

    // Touchpad pinch: Ctrl+wheel with no Ctrl key down is a magnify
    // gesture.
    sink->events.clear();
    PostMessageW(hwnd, WM_MOUSEWHEEL, MAKEWPARAM(MK_CONTROL, 60), MAKELPARAM(origin.x + 5, origin.y + 5));
    pump(server);
    const auto magnify_deadline = GetTickCount64() + 2000;
    while (sink->events.size() < 2 && GetTickCount64() < magnify_deadline) {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
        pump(server);
    }
    check(sink->events.size() == 2 && sink->events[0].type == gui::event_type::magnify &&
              close_to(sink->events[0].magnification, 60 * win32::magnification_per_wheel_unit, 1e-9) &&
              sink->events[0].scroll_phase == gui::phase::began && sink->events[1].type == gui::event_type::magnify &&
              sink->events[1].scroll_phase == gui::phase::ended,
          "touchpad pinch is a magnify gesture");

    // Live resize: WM_SIZING consults will_resize; the modal loop ticks.
    sink->events.clear();
    sink->resize = [](gui::Size proposed) {
        return gui::Size{std::round(proposed.width / 50) * 50, std::round(proposed.height / 50) * 50};
    };
    SendMessageW(hwnd, WM_ENTERSIZEMOVE, 0, 0);
    RECT sizing{};
    GetWindowRect(hwnd, &sizing);
    sizing.right += static_cast<LONG>(std::lround(37 * scale));
    sizing.bottom += static_cast<LONG>(std::lround(12 * scale));
    SendMessageW(hwnd, WM_SIZING, WMSZ_BOTTOMRIGHT, reinterpret_cast<LPARAM>(&sizing));
    RECT window_rect{};
    GetWindowRect(hwnd, &window_rect);
    const auto horizontal = (window_rect.right - window_rect.left) - client.right;
    const auto content_width = (sizing.right - sizing.left - horizontal) / scale;
    check(close_to(std::fmod(content_width, 50.0), 0, 1.0) || close_to(std::fmod(content_width, 50.0), 50, 1.0),
          "will_resize applied to WM_SIZING (" + std::to_string(content_width) + ")");
    check(sink->find_notification(gui::Notification::live_resize_started) != nullptr, "live resize started");
    const auto tick_deadline = GetTickCount64() + 2000;
    while (sink->count_notification(gui::Notification::modal_tick) < 2 && GetTickCount64() < tick_deadline) {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
        pump(server);
    }
    check(sink->count_notification(gui::Notification::modal_tick) >= 2, "modal ticks during size/move");
    SendMessageW(hwnd, WM_EXITSIZEMOVE, 0, 0);
    check(sink->find_notification(gui::Notification::live_resize_ended) != nullptr, "live resize ended");
    sink->resize = nullptr;

    // Focus.
    sink->events.clear();
    SendMessageW(hwnd, WM_SETFOCUS, 0, 0);
    check(server.key_window() == window && sink->find_notification(gui::Notification::became_key) != nullptr,
          "became key");
    SendMessageW(hwnd, WM_KILLFOCUS, 0, 0);
    check(server.key_window() == 0 && sink->find_notification(gui::Notification::resigned_key) != nullptr,
          "resigned key");

    // Full screen on a hidden window: style switches, the window stays
    // hidden, and the content rect comes back.
    sink->events.clear();
    const auto before = server.content_rect(window);
    server.set_full_screen(window, true);
    check(sink->find_notification(gui::Notification::entered_full_screen) != nullptr, "entered full screen");
    check((GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CAPTION) == 0, "full screen has no caption");
    server.set_full_screen(window, false);
    check(sink->find_notification(gui::Notification::exited_full_screen) != nullptr, "exited full screen");
    const auto after = server.content_rect(window);
    check(close_to(after.size.width, before.size.width, tolerance) && close_to(after.origin.x, before.origin.x, tolerance),
          "full screen restores the content rect");
    check(!IsWindowVisible(hwnd), "full screen toggles keep the window hidden");

    // Style changes keep the content rect.
    server.set_style(window, gui::style::titled | gui::style::closable);
    const auto restyled = server.content_rect(window);
    check(close_to(restyled.size.width, before.size.width, tolerance) &&
              (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_THICKFRAME) == 0,
          "set_style keeps the content rect");

    server.set_title(window, "renamed");
    server.set_level(window, 3);
    check((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0, "floating level is topmost");
    server.set_level(window, 0);
    server.set_text_input_enabled(window, true);
    server.set_text_input_enabled(window, false);
    server.invalidate(window, gui::Rect{{0, 0}, {10, 10}});
    server.set_accepts_file_drops(window, true);
    server.set_accepts_file_drops(window, false);

    // Close request: the window stays until destroyed.
    sink->events.clear();
    PostMessageW(hwnd, WM_CLOSE, 0, 0);
    pump(server);
    check(sink->find_notification(gui::Notification::close_requested) != nullptr && IsWindow(hwnd),
          "close requested, window kept");

    server.destroy_window(window);
    check(!IsWindow(hwnd), "destroy_window destroys the HWND");
    bool threw = false;
    try {
        static_cast<void>(server.content_rect(window));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    check(threw, "calls on a destroyed window fail");
}

void test_exception_fence(win32::Win32WindowServer& server) {
    auto sink = std::make_shared<RecordingSink>();
    gui::WindowDesc description;
    description.content_rect = {{100, 100}, {320, 240}};
    const auto window = server.create_window(description, sink);
    const auto hwnd = static_cast<HWND>(server.native_handle(window));

    // A sink exception inside DispatchMessage surfaces from
    // dispatch_messages, and later messages are still delivered.
    sink->on_event = [](const gui::Event& event) {
        if (event.type == gui::event_type::key_down) throw GuestUnwind{42};
    };
    PostMessageW(hwnd, WM_KEYDOWN, 'B', key_lparam(0x30, false, false));
    PostMessageW(hwnd, WM_CHAR, L'q', 1);
    int caught = 0;
    for (int round = 0; round < 8; ++round) {
        try {
            if (!server.dispatch_messages()) break;
        } catch (const GuestUnwind& unwind) {
            caught = unwind.code;
        }
    }
    check(caught == 42, "dispatch_messages rethrows the sink exception");
    bool text_after = false;
    for (const auto& event : sink->events) text_after = text_after || (event.kind == gui::EventKind::text);
    check(text_after, "messages after the exception are delivered");

    // A sink exception during a synchronous Win32 call (SetWindowPos ->
    // WM_SIZE) surfaces from that call.
    sink->on_event = [](const gui::Event& event) {
        if (event.notification == gui::Notification::resized) throw std::logic_error("resize failed");
    };
    bool resize_threw = false;
    try {
        server.set_content_rect(window, {{100, 100}, {330, 250}});
    } catch (const std::logic_error&) {
        resize_threw = true;
    }
    check(resize_threw, "set_content_rect rethrows the sink exception");
    sink->on_event = nullptr;
    server.set_content_rect(window, {{100, 100}, {340, 260}});   // usable afterwards

    // destroy_window from inside the window's own callback is deferred until
    // the callback returns.
    sink->on_event = [&](const gui::Event& event) {
        if (event.notification == gui::Notification::close_requested) server.destroy_window(window);
    };
    PostMessageW(hwnd, WM_CLOSE, 0, 0);
    pump(server);
    check(!IsWindow(hwnd), "destroy from inside a callback completes after it");
    bool threw = false;
    try {
        static_cast<void>(server.native_handle(window));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    check(threw, "deferred destroy removed the window");
}

void test_clipboard(win32::Win32WindowServer& server) {
    std::optional<std::string> previous;
    try {
        previous = server.clipboard_text();
    } catch (const std::exception& error) {
        std::cout << "clipboard unavailable, skipped: " << error.what() << '\n';
        return;
    }
    const auto count = server.clipboard_change_count();
    const std::string text = "Alchemy clipboard \xe2\x9c\x93\nsecond line \xf0\x9f\x98\x80";
    server.set_clipboard_text(text);
    check(server.clipboard_change_count() != count, "change count advances");
    check(server.clipboard_text() == text, "clipboard text round trip");
    gui::Image image;
    image.width = 4;
    image.height = 3;
    for (int index = 0; index < 12; ++index)
        for (const int channel : {index * 20, 255 - index * 10, index * 3, 64 + index * 15})
            image.rgba.push_back(static_cast<std::uint8_t>(channel));
    server.set_clipboard_image(image);
    const auto back = server.clipboard_image();
    check(back && back->width == 4 && back->height == 3 && back->rgba == image.rgba, "clipboard image round trip");
    check(!server.clipboard_text(), "setting an image replaces the text");
    if (previous) server.set_clipboard_text(*previous);
}

void test_cursors_and_keyboard(win32::Win32WindowServer& server) {
    const auto arrow = server.system_cursor(gui::SystemCursor::arrow);
    check(arrow != 0 && server.system_cursor(gui::SystemCursor::arrow) == arrow, "system cursors are cached");
    gui::Image image;
    image.width = 16;
    image.height = 16;
    image.rgba.assign(16 * 16 * 4, 0);
    for (int index = 0; index < 16; ++index) {
        auto* pixel = image.rgba.data() + (index * 16 + index) * 4;
        pixel[0] = 255;
        pixel[3] = 255;
    }
    const auto custom = server.create_cursor(image, {8, 8});
    server.set_cursor(custom);
    server.set_cursor_visible(false);
    server.set_cursor_visible(true);
    server.set_cursor(arrow);
    server.destroy_cursor(custom);
    bool threw = false;
    try {
        server.set_cursor(custom);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    check(threw, "destroyed cursor is rejected");

    const auto layout = server.keyboard_layout();
    check(!layout.identifier.empty() && !layout.localized_name.empty(), "keyboard layout: " + layout.identifier);
    gui::DeadKeyState state;
    check(!server.translate_key(kvk::ansi_a, 0, state).empty() && state.value == 0, "translate A");
    check(server.translate_key(kvk::return_key, 0, state) == "\r", "translate Return");
    check(server.translate_key(kvk::left_arrow, 0, state) == "\x1c", "translate left arrow");
    check(server.translate_key(kvk::f1, 0, state) == "\x10", "translate F1");
    check(server.double_click_interval() > 0 && server.key_repeat_interval() > 0 && server.key_repeat_delay() > 0,
          "input timing settings");
}

// ---- drag and drop ------------------------------------------------------------------

// A CF_HDROP data object, what Explorer offers for a file drag.
class FileDataObject final : public IDataObject {
public:
    explicit FileDataObject(std::vector<std::wstring> paths) : paths_(std::move(paths)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override {
        if (object == nullptr) return E_POINTER;
        if (IsEqualIID(id, IID_IUnknown) || IsEqualIID(id, IID_IDataObject)) {
            *object = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { return --references_; }   // lives on the stack
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override {
        if (QueryGetData(format) != S_OK) return DV_E_FORMATETC;
        std::size_t characters = 1;
        for (const auto& path : paths_) characters += path.size() + 1;
        const auto memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + characters * sizeof(wchar_t));
        if (memory == nullptr) return E_OUTOFMEMORY;
        auto* drop = static_cast<DROPFILES*>(GlobalLock(memory));
        drop->pFiles = sizeof(DROPFILES);
        drop->fWide = TRUE;
        auto* text = reinterpret_cast<wchar_t*>(reinterpret_cast<char*>(drop) + sizeof(DROPFILES));
        for (const auto& path : paths_) {
            std::memcpy(text, path.c_str(), (path.size() + 1) * sizeof(wchar_t));
            text += path.size() + 1;
        }
        GlobalUnlock(memory);
        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = memory;
        medium->pUnkForRelease = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override {
        return format != nullptr && format->cfFormat == CF_HDROP && (format->tymed & TYMED_HGLOBAL) ? S_OK
                                                                                                  : DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override {
        if (out != nullptr) out->ptd = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD, IEnumFORMATETC**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

private:
    std::vector<std::wstring> paths_;
    ULONG references_{1};
};

void test_drag_operation_tables() {
    namespace op = gui::drag_operation;
    const DWORD all = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
    check(win32::drag_operations_for(all, 0) == (op::copy | op::link | op::move | op::generic), "drag: all effects");
    check(win32::drag_operations_for(DROPEFFECT_COPY, MK_LBUTTON) == op::copy, "drag: copy-only source");
    check(win32::drag_operations_for(all, MK_CONTROL) == op::copy, "drag: Ctrl copies");
    check(win32::drag_operations_for(all, MK_SHIFT) == (op::move | op::generic), "drag: Shift moves");
    check(win32::drag_operations_for(all, MK_CONTROL | MK_SHIFT) == op::link, "drag: Ctrl+Shift links");
    check(win32::drag_operations_for(DROPEFFECT_MOVE, MK_CONTROL) == op::none, "drag: Ctrl on a move-only source");
    check(win32::drop_effect_for(op::copy | op::move) == DROPEFFECT_COPY, "drop effect prefers copy");
    check(win32::drop_effect_for(op::link | op::generic) == DROPEFFECT_LINK, "drop effect: link before move");
    check(win32::drop_effect_for(op::generic) == DROPEFFECT_MOVE, "drop effect: generic moves");
    check(win32::drop_effect_for(op::none) == DROPEFFECT_NONE && win32::drop_effect_for(op::private_) == DROPEFFECT_NONE,
          "drop effect: nothing shown");
}

void test_drag_and_drop(win32::Win32WindowServer& server) {
    namespace op = gui::drag_operation;
    auto sink = std::make_shared<RecordingSink>();
    gui::WindowDesc description;
    description.content_rect = {{150, 150}, {300, 200}};
    const auto window = server.create_window(description, sink);
    const auto hwnd = static_cast<HWND>(server.native_handle(window));
    server.set_accepts_file_drops(window, true);
    auto* target = server.drop_target(window);
    if (target == nullptr) {
        std::cout << "OLE drag and drop unavailable (WM_DROPFILES fallback): drag tests skipped\n";
        server.destroy_window(window);
        return;
    }
    FileDataObject data({L"C:\\alchemy\\drag one.blend", L"C:\\alchemy\\\u00e9t\u00e9.png"});
    const std::vector<std::string> expected_paths{"C:\\alchemy\\drag one.blend", "C:\\alchemy\\\xc3\xa9t\xc3\xa9.png"};
    const auto all_operations = op::copy | op::link | op::move | op::generic;
    const DWORD all = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
    RECT client{};
    GetClientRect(hwnd, &client);
    POINT pixel{client.right / 2, client.bottom / 4};
    ClientToScreen(hwnd, &pixel);
    const POINTL point{pixel.x, pixel.y};
    const auto scale = server.backing_scale(window);

    DWORD effect = all;
    check(target->DragEnter(&data, MK_LBUTTON, point, &effect) == S_OK && effect == DROPEFFECT_COPY,
          "drag entered: copy shown");
    const auto* entered = sink->find_notification(gui::Notification::drag_entered);
    check(entered != nullptr && entered->window == window && entered->paths == expected_paths &&
              entered->drag_operations == all_operations &&
              close_to(entered->location.x, (client.right / 2) / scale, 1e-6) &&
              close_to(entered->location.y, (client.bottom - client.bottom / 4) / scale, 1e-6),
          "drag_entered payload (paths, operations, content location)");
    check(sink->drag_queries == 1, "drag_operation asked after drag_entered");

    // The window narrows to move, then refuses: the effect follows.
    sink->accepted_drag = op::move;
    effect = all;
    const POINTL moved{point.x + 4, point.y + 3};
    check(target->DragOver(MK_LBUTTON, moved, &effect) == S_OK && effect == DROPEFFECT_MOVE, "drag updated: move shown");
    const auto* updated = sink->find_notification(gui::Notification::drag_updated);
    check(updated != nullptr && updated->paths == expected_paths &&
              close_to(updated->location.x, (client.right / 2 + 4) / scale, 1e-6),
          "drag_updated payload");
    sink->accepted_drag = op::none;
    effect = all;
    target->DragOver(MK_LBUTTON, moved, &effect);
    check(effect == DROPEFFECT_NONE, "refused drag shows no effect");
    target->DragLeave();
    check(sink->count_notification(gui::Notification::drag_exited) == 1 &&
              sink->find_notification(gui::Notification::files_dropped) == nullptr,
          "drag exited without a drop");

    // A drop ends the drag with files_dropped and no drag_exited.
    sink->events.clear();
    sink->accepted_drag = op::every;
    effect = all;
    target->DragEnter(&data, MK_LBUTTON, point, &effect);
    effect = all;
    target->DragOver(MK_LBUTTON, point, &effect);
    effect = all;
    check(target->Drop(&data, 0, point, &effect) == S_OK && effect == DROPEFFECT_COPY, "drop: copy");
    const auto* dropped = sink->find_notification(gui::Notification::files_dropped);
    check(dropped != nullptr && dropped->paths == expected_paths && dropped->drag_operations == all_operations,
          "files_dropped payload");
    check(sink->count_notification(gui::Notification::drag_exited) == 0, "no drag_exited after a drop");

    // Ctrl narrows the source operations to copy.
    sink->events.clear();
    effect = all;
    target->DragEnter(&data, MK_LBUTTON | MK_CONTROL, point, &effect);
    const auto* copying = sink->find_notification(gui::Notification::drag_entered);
    check(copying != nullptr && copying->drag_operations == op::copy && effect == DROPEFFECT_COPY, "Ctrl drag copies");
    target->DragLeave();

    // A drop the window refused ends with drag_exited, not files_dropped.
    sink->events.clear();
    sink->accepted_drag = op::none;
    effect = all;
    target->DragEnter(&data, MK_LBUTTON, point, &effect);
    effect = all;
    target->Drop(&data, 0, point, &effect);
    check(effect == DROPEFFECT_NONE && sink->find_notification(gui::Notification::files_dropped) == nullptr &&
              sink->find_notification(gui::Notification::drag_exited) != nullptr,
          "refused drop");

    // A sink exception inside a drag callback surfaces from dispatch_messages
    // and the drag shows no effect.
    sink->events.clear();
    sink->accepted_drag = op::every;
    sink->on_event = [](const gui::Event& event) {
        if (event.notification == gui::Notification::drag_entered) throw GuestUnwind{7};
    };
    effect = all;
    target->DragEnter(&data, MK_LBUTTON, point, &effect);
    check(effect == DROPEFFECT_NONE, "drag callback exception: no effect");
    int caught = 0;
    PostMessageW(hwnd, WM_NULL, 0, 0);
    try {
        server.dispatch_messages();
    } catch (const GuestUnwind& unwind) {
        caught = unwind.code;
    }
    check(caught == 7, "drag callback exception rethrown by dispatch_messages");
    sink->on_event = nullptr;
    target->DragLeave();

    server.set_accepts_file_drops(window, false);
    check(server.drop_target(window) == nullptr, "drop target revoked");
    server.destroy_window(window);
}

// ---- child windows ----------------------------------------------------------------

void test_child_windows_follow_parent(win32::Win32WindowServer& server) {
    auto parent_sink = std::make_shared<RecordingSink>();
    auto child_sink = std::make_shared<RecordingSink>();
    gui::WindowDesc description;
    description.content_rect = {{200, 200}, {400, 300}};
    const auto parent = server.create_window(description, parent_sink);
    gui::WindowDesc child_description;
    child_description.content_rect = {{260, 240}, {120, 80}};
    child_description.style_mask = gui::style::titled | gui::style::closable;
    child_description.parent = parent;
    const auto child = server.create_window(child_description, child_sink);
    const auto tolerance = 1.0 / server.backing_scale(parent) + 1e-6;

    const auto before = server.content_rect(child);
    server.set_content_rect(parent, {{250, 170}, {400, 300}});
    auto after = server.content_rect(child);
    check(close_to(after.origin.x, before.origin.x + 50, tolerance) &&
              close_to(after.origin.y, before.origin.y - 30, tolerance) &&
              close_to(after.size.width, before.size.width, tolerance) &&
              close_to(after.size.height, before.size.height, tolerance),
          "child window moves with its parent");
    check(child_sink->find_notification(gui::Notification::moved) != nullptr, "child reports moved");

    // A resize that keeps the parent's origin (bottom-left) leaves the child.
    server.set_content_rect(parent, {{250, 170}, {400, 360}});
    const auto kept = server.content_rect(child);
    check(close_to(kept.origin.x, after.origin.x, tolerance) && close_to(kept.origin.y, after.origin.y, tolerance),
          "child stays when the parent grows upward");

    // A destroyed parent releases its child, which stays and no longer moves.
    server.destroy_window(parent);
    after = server.content_rect(child);
    check(close_to(after.origin.x, kept.origin.x, tolerance), "child survives its parent");
    server.destroy_window(child);
}

// set_parent (addChildWindow:): a top-level window becomes an owned child
// that moves with its parent, and top level again with 0.
void test_set_parent(win32::Win32WindowServer& server) {
    auto parent_sink = std::make_shared<RecordingSink>();
    auto child_sink = std::make_shared<RecordingSink>();
    gui::WindowDesc description;
    description.content_rect = {{200, 200}, {400, 300}};
    const auto parent = server.create_window(description, parent_sink);
    description.content_rect = {{260, 240}, {120, 80}};
    const auto child = server.create_window(description, child_sink);
    const auto tolerance = 1.0 / server.backing_scale(parent) + 1e-6;
    const auto parent_hwnd = static_cast<HWND>(server.native_handle(parent));
    const auto child_hwnd = static_cast<HWND>(server.native_handle(child));
    check(GetWindow(child_hwnd, GW_OWNER) == nullptr, "set_parent: top level before");
    server.set_parent(child, parent);
    check(GetWindow(child_hwnd, GW_OWNER) == parent_hwnd, "set_parent: the parent owns the child");
    auto before = server.content_rect(child);
    server.set_content_rect(parent, {{240, 180}, {400, 300}});
    auto after = server.content_rect(child);
    check(close_to(after.origin.x, before.origin.x + 40, tolerance) && close_to(after.origin.y, before.origin.y - 20, tolerance),
          "set_parent: the child moves with its parent");
    bool refused = false;
    try { server.set_parent(parent, child); } catch (const std::runtime_error&) { refused = true; }
    check(refused, "set_parent: a parent cannot become its child's child");
    server.set_parent(child, 0);
    check(GetWindow(child_hwnd, GW_OWNER) == nullptr, "set_parent 0: top level again");
    before = server.content_rect(child);
    server.set_content_rect(parent, {{200, 200}, {400, 300}});
    after = server.content_rect(child);
    check(close_to(after.origin.x, before.origin.x, tolerance) && close_to(after.origin.y, before.origin.y, tolerance),
          "set_parent 0: the child no longer follows");
    server.destroy_window(child);
    server.destroy_window(parent);
}

// ---- DPI ------------------------------------------------------------------------------

void test_dpi_changes(win32::Win32WindowServer& server) {
    auto sink = std::make_shared<RecordingSink>();
    gui::WindowDesc description;
    description.content_rect = {{180, 160}, {360, 240}};
    const auto window = server.create_window(description, sink);
    const auto hwnd = static_cast<HWND>(server.native_handle(window));
    const auto dpi = GetDpiForWindow(hwnd);
    const UINT doubled = dpi * 2;
    RECT client{}, frame{};
    GetClientRect(hwnd, &client);
    GetWindowRect(hwnd, &frame);

    // The DPI-scaled size keeps the content size in points (the title bar is
    // measured at the new DPI, not scaled).
    SIZE size{};
    check(SendMessageW(hwnd, WM_GETDPISCALEDSIZE, doubled, reinterpret_cast<LPARAM>(&size)) == TRUE,
          "WM_GETDPISCALEDSIZE handled");
    RECT expected{0, 0, client.right * 2, client.bottom * 2};
    AdjustWindowRectExForDpi(&expected, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)), FALSE,
                             static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)), doubled);
    check(size.cx == expected.right - expected.left && size.cy == expected.bottom - expected.top,
          "DPI-scaled size keeps the content size");

    // WM_DPICHANGED applies the suggested rect and reports the new scale once.
    sink->events.clear();
    RECT suggested{frame.left, frame.top, frame.left + size.cx, frame.top + size.cy};
    SendMessageW(hwnd, WM_DPICHANGED, MAKEWPARAM(doubled, doubled), reinterpret_cast<LPARAM>(&suggested));
    RECT now{};
    GetWindowRect(hwnd, &now);
    check(EqualRect(&now, &suggested) != FALSE, "suggested rect applied");
    const auto* backing = sink->find_notification(gui::Notification::backing_changed);
    check(backing != nullptr && close_to(backing->backing_scale, doubled / 96.0, 1e-9) &&
              backing->screen == server.window_screen(window),
          "backing_changed carries the new scale and screen");
    check(sink->find_notification(gui::Notification::resized) != nullptr, "the size change is reported");
    SendMessageW(hwnd, WM_DPICHANGED, MAKEWPARAM(doubled, doubled), reinterpret_cast<LPARAM>(&suggested));
    check(sink->count_notification(gui::Notification::backing_changed) == 1, "an unchanged scale is not reported again");

    // Back to the real scale.
    sink->events.clear();
    SendMessageW(hwnd, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&frame));
    GetWindowRect(hwnd, &now);
    backing = sink->find_notification(gui::Notification::backing_changed);
    check(EqualRect(&now, &frame) != FALSE && backing != nullptr && close_to(backing->backing_scale, dpi / 96.0, 1e-9),
          "scale change back");

    // Full screen keeps covering its monitor whatever is suggested.
    server.set_full_screen(window, true);
    RECT odd{frame.left, frame.top, frame.left + 100, frame.top + 100};
    SendMessageW(hwnd, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&odd));
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info);
    GetWindowRect(hwnd, &now);
    check(EqualRect(&now, &info.rcMonitor) != FALSE, "full screen ignores the suggested rect");
    server.set_full_screen(window, false);
    server.destroy_window(window);
}

// Callers on threads with another DPI awareness (SetThreadDpiAwarenessContext)
// still get per-monitor-aware windows and physical-pixel geometry.
void test_caller_dpi_awareness(win32::Win32WindowServer& server) {
    for (const auto context : {DPI_AWARENESS_CONTEXT_UNAWARE, DPI_AWARENESS_CONTEXT_SYSTEM_AWARE,
                               DPI_AWARENESS_CONTEXT_UNAWARE_GDISCALED}) {
        const auto previous = SetThreadDpiAwarenessContext(context);
        if (previous == nullptr) continue;   // not supported by this Windows version
        const auto name = std::to_string(static_cast<int>(GetAwarenessFromDpiAwarenessContext(context)));
        auto sink = std::make_shared<RecordingSink>();
        gui::WindowDesc description;
        description.content_rect = {{140, 130}, {320, 200}};
        const auto window = server.create_window(description, sink);
        const auto hwnd = static_cast<HWND>(server.native_handle(window));
        check(GetAwarenessFromDpiAwarenessContext(GetWindowDpiAwarenessContext(hwnd)) == DPI_AWARENESS_PER_MONITOR_AWARE,
              "window is per-monitor aware (caller awareness " + name + ")");
        double monitor_scale = 0;
        for (const auto& screen : server.screens())
            if (screen.id == server.window_screen(window)) monitor_scale = screen.backing_scale;
        const auto scale = server.backing_scale(window);
        check(close_to(scale, monitor_scale, 1e-9), "backing scale is the monitor's (caller awareness " + name + ")");
        const auto tolerance = 1.0 / scale + 1e-6;
        server.set_content_rect(window, {{150, 140}, {330, 210}});
        const auto content = server.content_rect(window);
        check(close_to(content.origin.x, 150, tolerance) && close_to(content.origin.y, 140, tolerance) &&
                  close_to(content.size.width, 330, tolerance) && close_to(content.size.height, 210, tolerance),
              "content rect round trip (caller awareness " + name + ")");
        const auto pixels = server.content_pixel_size(window);
        check(close_to(pixels.width, 330 * scale, 1.0), "pixel size (caller awareness " + name + ")");
        server.destroy_window(window);
        SetThreadDpiAwarenessContext(previous);
    }
}

// ---- focus and activation (private desktop) --------------------------------------------

// Runs `body` on a thread attached to a private desktop: its windows can be
// shown and activated without appearing on, or taking the focus from, the
// user's desktop.
void on_private_desktop(const char* what, const std::function<void()>& body) {
    std::thread worker([&] {
        const auto name = L"AlchemyWindowServerTest" + std::to_wstring(GetCurrentProcessId());
        const auto desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        if (desktop == nullptr || !SetThreadDesktop(desktop)) {
            std::cout << "private desktop unavailable (Win32 error " << GetLastError() << "): " << what
                      << " skipped\n";
            if (desktop != nullptr) CloseDesktop(desktop);
            return;
        }
        try {
            body();
        } catch (const std::exception& error) {
            check(false, std::string(what) + ": unexpected exception: " + error.what());
        } catch (...) {
            check(false, std::string(what) + ": unexpected non-standard exception");
        }
        CloseDesktop(desktop);
    });
    worker.join();
}

void pump_until(gui::WindowServer& server, const std::function<bool()>& done, ULONGLONG milliseconds = 2000) {
    const auto deadline = GetTickCount64() + milliseconds;
    pump(server);
    while (!done() && GetTickCount64() < deadline) {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        pump(server);
    }
}

struct DialogProbe {
    win32::Win32WindowServer* server{};
    HWND owner{};
    bool ran{};
    bool owner_disabled{};
    gui::WindowId key_inside{~0ULL};
};
DialogProbe* dialog_probe = nullptr;

void CALLBACK close_dialog(HWND, UINT, UINT_PTR id, DWORD) {
    KillTimer(nullptr, id);
    auto& probe = *dialog_probe;
    probe.ran = true;
    probe.owner_disabled = IsWindowEnabled(probe.owner) == FALSE;
    probe.key_inside = probe.server->key_window();
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND candidate, LPARAM) -> BOOL {
            wchar_t name[16]{};
            GetClassNameW(candidate, name, 16);
            if (std::wcscmp(name, L"#32770") == 0) EndDialog(candidate, IDOK);
            return TRUE;
        },
        0);
}

std::size_t count_input(const RecordingSink& sink, std::uint32_t type) {
    std::size_t count = 0;
    for (const auto& event : sink.events)
        if (event.kind == gui::EventKind::input && event.type == type) ++count;
    return count;
}

// Blender's "limited platform support" NSAlert: a task-modal dialog owned by
// the key window. The window resigns key while it runs and is key again,
// receiving keys, afterwards.
void test_modal_dialog_focus() {
    win32::Win32WindowServer server;
    server.set_no_activate(false);
    auto sink = std::make_shared<RecordingSink>();
    gui::WindowDesc description;
    description.content_rect = {{200, 200}, {360, 240}};
    const auto window = server.create_window(description, sink);
    const auto hwnd = static_cast<HWND>(server.native_handle(window));
    server.show(window, gui::ShowCommand::make_key_and_order_front);
    pump(server);
    check(server.key_window() == window && GetFocus() == hwnd, "make_key_and_order_front takes the keyboard");

    sink->events.clear();
    DialogProbe probe{&server, hwnd};
    dialog_probe = &probe;
    SetTimer(nullptr, 0, 50, close_dialog);
    MessageBoxW(hwnd, L"Limited platform support", L"", MB_OK | MB_ICONWARNING | MB_TASKMODAL);
    pump_until(server, [&] { return server.key_window() == window; });
    check(probe.ran && probe.owner_disabled && probe.key_inside == 0, "the dialog holds the keyboard while it runs");
    check(sink->find_notification(gui::Notification::resigned_key) != nullptr && server.key_window() == window &&
              GetFocus() == hwnd && !sink->events.empty() &&
              sink->count_notification(gui::Notification::became_key) == 1,
          "key again after the dialog (resigned " +
              std::to_string(sink->find_notification(gui::Notification::resigned_key) != nullptr) + ", key " +
              std::to_string(server.key_window() == window) + ", focus " + std::to_string(GetFocus() == hwnd) +
              ", foreground " + std::to_string(GetForegroundWindow() == hwnd) + ", became_key " +
              std::to_string(sink->count_notification(gui::Notification::became_key)) + ")");
    PostMessageW(hwnd, WM_KEYDOWN, 'A', key_lparam(0x1e, false, false));
    PostMessageW(hwnd, WM_KEYUP, 'A', key_lparam(0x1e, false, true));
    pump(server);
    check(count_input(*sink, gui::event_type::key_down) == 1 && count_input(*sink, gui::event_type::key_up) == 1,
          "keys reach the window after the dialog");

    // A dialog that leaves no active window when it ends (unowned): the
    // window takes the keyboard back once it is enabled again.
    sink->events.clear();
    const auto dialog = CreateWindowExW(0, L"STATIC", L"dialog", WS_POPUP | WS_VISIBLE, 0, 0, 40, 40, nullptr, nullptr,
                                        nullptr, nullptr);
    SetActiveWindow(dialog);
    EnableWindow(hwnd, FALSE);
    pump(server);
    check(server.key_window() == 0 && sink->find_notification(gui::Notification::resigned_key) != nullptr,
          "a foreign dialog takes the key status");
    DestroyWindow(dialog);
    EnableWindow(hwnd, TRUE);
    pump_until(server, [&] { return server.key_window() == window; });
    check(server.key_window() == window && GetFocus() == hwnd, "key restored after an unowned dialog");
    server.destroy_window(window);
    dialog_probe = nullptr;
}

// ALCHEMY_GUI_NO_ACTIVATE=1: nothing activates a window; key status is
// logical.
void test_no_activate() {
    _putenv_s("ALCHEMY_GUI_NO_ACTIVATE", "1");
    win32::Win32WindowServer server;
    _putenv_s("ALCHEMY_GUI_NO_ACTIVATE", "");
    check(server.no_activate(), "ALCHEMY_GUI_NO_ACTIVATE=1 is honoured");
    const auto inactive = [] { return GetActiveWindow() == nullptr && GetFocus() == nullptr; };
    auto sink = std::make_shared<RecordingSink>();
    gui::WindowDesc description;
    description.content_rect = {{220, 180}, {360, 240}};
    const auto window = server.create_window(description, sink);
    const auto hwnd = static_cast<HWND>(server.native_handle(window));
    server.show(window, gui::ShowCommand::make_key_and_order_front);
    pump(server);
    check(IsWindowVisible(hwnd) && inactive(), "no_activate: shown without activation");
    check(server.key_window() == window && sink->count_notification(gui::Notification::became_key) == 1,
          "no_activate: key status is logical");

    const auto before = server.content_rect(window);
    server.zoom(window);
    pump(server);
    const auto zoomed = server.content_rect(window);
    check(IsZoomed(hwnd) && inactive() && zoomed.size.width > before.size.width, "no_activate: zoom without activation");
    server.zoom(window);
    pump(server);
    const auto restored = server.content_rect(window);
    const auto tolerance = 1.0 / server.backing_scale(window) + 1e-6;
    check(!IsZoomed(hwnd) && inactive() && close_to(restored.size.width, before.size.width, tolerance) &&
              close_to(restored.origin.y, before.origin.y, tolerance),
          "no_activate: unzoom restores without activation (zoomed " + std::to_string(IsZoomed(hwnd)) + ", active " +
              std::to_string(GetActiveWindow() != nullptr) + ", width " + std::to_string(restored.size.width) + " vs " +
              std::to_string(before.size.width) + ", y " + std::to_string(restored.origin.y) + " vs " +
              std::to_string(before.origin.y) + ")");
    server.miniaturize(window, true);
    check(IsIconic(hwnd) && inactive(), "no_activate: miniaturize without activation");
    server.miniaturize(window, false);
    check(!IsIconic(hwnd) && IsWindowVisible(hwnd) && inactive(), "no_activate: deminiaturize without activation");
    server.set_full_screen(window, true);
    server.set_full_screen(window, false);
    check(inactive(), "no_activate: full screen toggles without activation");

    auto second_sink = std::make_shared<RecordingSink>();
    const auto second = server.create_window(description, second_sink);
    server.show(second, gui::ShowCommand::make_key_and_order_front);
    check(server.key_window() == second && sink->find_notification(gui::Notification::resigned_key) != nullptr &&
              second_sink->find_notification(gui::Notification::became_key) != nullptr && inactive(),
          "no_activate: key status moves between windows");
    server.show(second, gui::ShowCommand::order_out);
    check(server.key_window() == 0 && second_sink->find_notification(gui::Notification::resigned_key) != nullptr,
          "no_activate: ordering out resigns key");
    server.destroy_window(second);
    server.destroy_window(window);
}

// The first order-in keeps a titled window's visible frame inside the work
// area (Blender places its main window at the bottom of the full screen
// frame, under the taskbar).
void test_initial_placement() {
    win32::Win32WindowServer server;
    server.set_no_activate(true);
    const auto screens = server.screens();
    if (screens.empty()) return;
    const auto area = screens[0].visible_frame;
    const auto inside_work_area = [](HWND hwnd) {
        RECT frame{};
        GetWindowRect(hwnd, &frame);
        MONITORINFO info{sizeof(info)};
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info);
        RECT client{};
        GetClientRect(hwnd, &client);
        POINT bottom{0, client.bottom};
        ClientToScreen(hwnd, &bottom);
        return frame.top >= info.rcWork.top && bottom.y <= info.rcWork.bottom;
    };

    auto sink = std::make_shared<RecordingSink>();
    gui::WindowDesc description;
    description.content_rect = {{area.origin.x + 40, area.origin.y - 120}, {400, 300}};
    const auto low = server.create_window(description, sink);
    const auto low_hwnd = static_cast<HWND>(server.native_handle(low));
    const auto before = server.content_rect(low);
    server.show(low, gui::ShowCommand::order_front);
    const auto after = server.content_rect(low);
    const auto tolerance = 1.0 / server.backing_scale(low) + 1e-6;
    check(inside_work_area(low_hwnd) && after.origin.y >= area.origin.y - tolerance &&
              close_to(after.origin.x, before.origin.x, tolerance) && close_to(after.size.height, 300, tolerance),
          "first order-in moves a window out from under the taskbar");
    check(sink->find_notification(gui::Notification::moved) != nullptr, "placement reports moved");
    server.show(low, gui::ShowCommand::order_out);
    server.set_content_rect(low, before);
    server.show(low, gui::ShowCommand::order_front);
    const auto again = server.content_rect(low);
    check(close_to(again.origin.y, before.origin.y, tolerance), "only the first order-in is constrained");
    server.destroy_window(low);

    description.content_rect = {{area.origin.x + 60, area.origin.y + 60}, {300, 200}};
    const auto inside = server.create_window(description, sink);
    server.show(inside, gui::ShowCommand::order_front);
    const auto kept = server.content_rect(inside);
    check(close_to(kept.origin.x, area.origin.x + 60, tolerance) && close_to(kept.origin.y, area.origin.y + 60, tolerance),
          "a window inside the work area stays");
    server.destroy_window(inside);

    description.content_rect = {{area.origin.x, area.origin.y - 40}, {area.size.width / 2, area.size.height + 200}};
    const auto tall = server.create_window(description, sink);
    server.show(tall, gui::ShowCommand::order_front);
    check(inside_work_area(static_cast<HWND>(server.native_handle(tall))), "a too tall resizable window is shrunk");
    server.destroy_window(tall);

    description.style_mask = gui::style::borderless;
    description.content_rect = {{area.origin.x + 10, area.origin.y - 50}, {120, 80}};
    const auto popup = server.create_window(description, sink);
    const auto popup_before = server.content_rect(popup);
    server.show(popup, gui::ShowCommand::order_front);
    check(close_to(server.content_rect(popup).origin.y, popup_before.origin.y, tolerance),
          "borderless windows are placed as asked");
    server.destroy_window(popup);
}

} // namespace

int main() {
    try {
        win32::Win32WindowServer server;
        if (!server.interactive()) {
            std::cout << "Alchemy GUI window server Win32 tests skipped: no interactive desktop\n";
            return 0;
        }
        test_screens(server);
        test_window_lifecycle(server);
        test_exception_fence(server);
        test_clipboard(server);
        test_cursors_and_keyboard(server);
        test_drag_operation_tables();
        test_drag_and_drop(server);
        test_child_windows_follow_parent(server);
        test_set_parent(server);
        test_dpi_changes(server);
        test_caller_dpi_awareness(server);
        on_private_desktop("modal dialog focus tests", test_modal_dialog_focus);
        on_private_desktop("no-activate tests", test_no_activate);
        on_private_desktop("initial placement tests", test_initial_placement);
    } catch (const std::exception& error) {
        std::cerr << "unexpected exception: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "unexpected non-standard exception\n";
        return 1;
    }
    if (failures != 0) {
        std::cerr << failures << " window server Win32 check(s) failed\n";
        return 1;
    }
    std::cout << "Alchemy GUI window server Win32 tests passed\n";
    return 0;
}
