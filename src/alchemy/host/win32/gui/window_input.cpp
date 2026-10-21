#include "alchemy/host/win32/gui/window_server.hpp"

#include "alchemy/gui/runloop.hpp"
#include "alchemy/host/win32/gui/clipboard_codec.hpp"

#include <imm.h>
#include <windowsx.h>

#include <cmath>
#include <cstdlib>

// Keyboard, mouse, pen, scroll, raw input and IME -> gui::Event (owner: A3).
namespace alchemy::gui::win32 {
namespace {

std::uint64_t buttons_from_key_state(WPARAM state) {
    std::uint64_t buttons = 0;
    if (state & MK_LBUTTON) buttons |= 1U << 0;
    if (state & MK_RBUTTON) buttons |= 1U << 1;
    if (state & MK_MBUTTON) buttons |= 1U << 2;
    if (state & MK_XBUTTON1) buttons |= 1U << 3;
    if (state & MK_XBUTTON2) buttons |= 1U << 4;
    return buttons;
}

// Logical button state of the current message (GetKeyState follows the
// queue, so it matches the message being handled).
std::uint64_t queue_buttons() {
    std::uint64_t buttons = 0;
    if (GetKeyState(VK_LBUTTON) < 0) buttons |= 1U << 0;
    if (GetKeyState(VK_RBUTTON) < 0) buttons |= 1U << 1;
    if (GetKeyState(VK_MBUTTON) < 0) buttons |= 1U << 2;
    if (GetKeyState(VK_XBUTTON1) < 0) buttons |= 1U << 3;
    if (GetKeyState(VK_XBUTTON2) < 0) buttons |= 1U << 4;
    return buttons;
}

std::uint32_t drag_type(std::uint64_t buttons, std::int64_t& button) {
    if (buttons & 1U) {
        button = 0;
        return event_type::left_mouse_dragged;
    }
    if (buttons & 2U) {
        button = 1;
        return event_type::right_mouse_dragged;
    }
    if (buttons) {
        button = 2;
        while (!(buttons & (1ULL << button))) ++button;
        return event_type::other_mouse_dragged;
    }
    button = 0;
    return event_type::mouse_moved;
}

std::uint32_t button_type(std::int64_t button, bool down) {
    if (button == 0) return down ? event_type::left_mouse_down : event_type::left_mouse_up;
    if (button == 1) return down ? event_type::right_mouse_down : event_type::right_mouse_up;
    return down ? event_type::other_mouse_down : event_type::other_mouse_up;
}

std::wstring composition_string(HIMC context, DWORD kind) {
    const auto bytes = ImmGetCompositionStringW(context, kind, nullptr, 0);
    if (bytes <= 0) return {};
    std::wstring text(static_cast<std::size_t>(bytes) / sizeof(wchar_t), L'\0');
    ImmGetCompositionStringW(context, kind, text.data(), static_cast<DWORD>(bytes));
    return text;
}

struct ContextLease {
    HWND hwnd;
    HIMC context;
    explicit ContextLease(HWND window) : hwnd(window), context(ImmGetContext(window)) {}
    ~ContextLease() {
        if (context) ImmReleaseContext(hwnd, context);
    }
    ContextLease(const ContextLease&) = delete;
    ContextLease& operator=(const ContextLease&) = delete;
};

bool is_high_surrogate(char32_t unit) { return unit >= 0xd800 && unit <= 0xdbff; }
bool is_low_surrogate(char32_t unit) { return unit >= 0xdc00 && unit <= 0xdfff; }

} // namespace

// ---- state queries ----------------------------------------------------------------

PhysicalModifiers Win32WindowServer::physical_modifiers() const {
    PhysicalModifiers keys;
    keys.left_shift = GetKeyState(VK_LSHIFT) < 0;
    keys.right_shift = GetKeyState(VK_RSHIFT) < 0;
    keys.left_control = GetKeyState(VK_LCONTROL) < 0 && !fake_control_down_;
    keys.right_control = GetKeyState(VK_RCONTROL) < 0;
    keys.left_alt = GetKeyState(VK_LMENU) < 0;
    keys.right_alt = GetKeyState(VK_RMENU) < 0;
    keys.left_windows = GetKeyState(VK_LWIN) < 0;
    keys.right_windows = GetKeyState(VK_RWIN) < 0;
    keys.caps_lock_on = (GetKeyState(VK_CAPITAL) & 1) != 0;
    return keys;
}

std::uint64_t Win32WindowServer::current_modifier_flags() const {
    return win32::modifier_flags(physical_modifiers(), swap_control_command_);
}

std::uint64_t Win32WindowServer::modifier_flags() const {
    require_main_thread("modifier_flags");
    return current_modifier_flags();
}

std::uint64_t Win32WindowServer::pressed_mouse_buttons() const {
    // Any thread (+[NSEvent pressedMouseButtons] is thread-safe; Qt asks from
    // render threads). Physical state: it is not queue-synchronized.
    const bool swapped = GetSystemMetrics(SM_SWAPBUTTON) != 0;
    std::uint64_t buttons = 0;
    if (GetAsyncKeyState(swapped ? VK_RBUTTON : VK_LBUTTON) < 0) buttons |= 1U << 0;
    if (GetAsyncKeyState(swapped ? VK_LBUTTON : VK_RBUTTON) < 0) buttons |= 1U << 1;
    if (GetAsyncKeyState(VK_MBUTTON) < 0) buttons |= 1U << 2;
    if (GetAsyncKeyState(VK_XBUTTON1) < 0) buttons |= 1U << 3;
    if (GetAsyncKeyState(VK_XBUTTON2) < 0) buttons |= 1U << 4;
    return buttons;
}

Point Win32WindowServer::mouse_location() const {
    // Any thread (+[NSEvent mouseLocation] is thread-safe; QCursor::pos() on a
    // render thread reaches it). layout() is locked.
    const ThreadDpiScope dpi_scope;   // physical pixels whatever the caller's DPI awareness
    POINT cursor{};
    if (!GetCursorPos(&cursor)) fail_win32("GetCursorPos");
    return screen_point(cursor);
}

double Win32WindowServer::double_click_interval() const { return GetDoubleClickTime() / 1000.0; }

double Win32WindowServer::key_repeat_delay() const {
    int delay = 1;   // 0..3 -> 250..1000 ms
    SystemParametersInfoW(SPI_GETKEYBOARDDELAY, 0, &delay, 0);
    return (std::clamp(delay, 0, 3) + 1) * 0.25;
}

double Win32WindowServer::key_repeat_interval() const {
    DWORD speed = 31;   // 0..31 -> about 2.5..30 repetitions per second
    SystemParametersInfoW(SPI_GETKEYBOARDSPEED, 0, &speed, 0);
    const auto rate = 2.5 + std::min<DWORD>(speed, 31) * (27.5 / 31.0);
    return 1.0 / rate;
}

double Win32WindowServer::message_time() const {
    // GetMessageTime is the tick count of the message being handled; express
    // the message's age on the NSEvent clock.
    const auto age = static_cast<DWORD>(GetTickCount() - static_cast<DWORD>(GetMessageTime()));
    const auto now = monotonic_seconds();
    return age < 10000 ? now - age / 1000.0 : now;
}

void Win32WindowServer::fill_pointer(Event& event, WindowRecord& record, POINT client, std::uint64_t buttons) {
    event.location = content_point(record.hwnd, client.x, client.y);
    POINT screen = client;
    ClientToScreen(record.hwnd, &screen);
    event.screen_location = screen_point(screen);
    event.pressed_buttons = buttons;
    event.modifier_flags = current_modifier_flags();
    event.timestamp = message_time();
}

std::int64_t Win32WindowServer::count_click(WindowRecord& record, std::int64_t button, bool down, POINT screen) {
    if (!down) return click_.window == record.id && click_.button == button ? click_.count : 1;
    const auto time = GetMessageTime();
    const bool repeat = click_.window == record.id && click_.button == button &&
                        static_cast<DWORD>(time - click_.time) <= GetDoubleClickTime() &&
                        std::abs(screen.x - click_.position.x) <= GetSystemMetrics(SM_CXDOUBLECLK) / 2 &&
                        std::abs(screen.y - click_.position.y) <= GetSystemMetrics(SM_CYDOUBLECLK) / 2;
    click_ = {record.id, button, time, screen, repeat ? click_.count + 1 : 1};
    return click_.count;
}

// ---- keyboard ---------------------------------------------------------------------

std::string Win32WindowServer::key_characters(UINT virtual_key, UINT scan_code, std::uint16_t key_code, bool up,
                                              bool ignoring_modifiers) {
    BYTE state[256]{};
    if (!GetKeyboardState(state)) return {};
    const bool shift = (state[VK_SHIFT] & 0x80) != 0;
    if (const auto special = event_special_character(key_code, shift)) {
        std::string text;
        append_utf8(text, *special);
        return text;
    }
    if (swap_control_command_ && !(state[VK_RMENU] & 0x80)) {
        // Command (physical Ctrl) leaves characters alone; Control (physical
        // Windows key) produces control characters, as on macOS.
        std::swap(state[VK_LCONTROL], state[VK_LWIN]);
        std::swap(state[VK_RCONTROL], state[VK_RWIN]);
        state[VK_CONTROL] = static_cast<BYTE>((state[VK_LCONTROL] | state[VK_RCONTROL]) & 0x80);
    }
    if (ignoring_modifiers) {
        for (const int key : {VK_CONTROL, VK_LCONTROL, VK_RCONTROL, VK_MENU, VK_LMENU, VK_RMENU, VK_LWIN, VK_RWIN})
            state[key] = 0;
    }
    wchar_t buffer[16]{};
    // Bit 2: do not change the keyboard state (Windows 10 1607+). Only the
    // real key-down translation advances the dead-key state; probes
    // (ignoring modifiers, key up) must not consume it.
    const UINT flags = (ignoring_modifiers || up) ? 0x4 : 0;
    const auto count = ToUnicodeEx(virtual_key, scan_code, state, buffer, 16, flags, GetKeyboardLayout(0));
    if (count < 0) {
        // A dead key: AppKit reports no characters for it, and the spacing
        // accent when modifiers are ignored.
        if (!ignoring_modifiers) return {};
        return wide_to_utf8(std::wstring_view(buffer, 1));
    }
    return wide_to_utf8(std::wstring_view(buffer, static_cast<std::size_t>(count)));
}

bool Win32WindowServer::handle_key(WindowRecord& record, UINT message, WPARAM wparam, LPARAM lparam) {
    const bool up = message == WM_KEYUP || message == WM_SYSKEYUP;
    auto virtual_key = static_cast<UINT>(wparam);
    const auto scan_code = static_cast<UINT>((lparam >> 16) & 0xff);
    const bool extended = (lparam & (1 << 24)) != 0;
    const bool repeat = !up && (lparam & (1 << 30)) != 0;
    if (virtual_key == VK_PROCESSKEY || virtual_key == VK_PACKET) return true;   // IME / WM_CHAR handle these
    // AltGr arrives as a synthesized left Ctrl followed by right Alt with the
    // same time stamp; macOS has only Option there, so drop the Ctrl.
    if (virtual_key == VK_CONTROL && !extended) {
        MSG next{};
        if (PeekMessageW(&next, nullptr, 0, 0, PM_NOREMOVE) && next.wParam == VK_MENU &&
            (next.lParam & (1 << 24)) != 0 && next.time == static_cast<DWORD>(GetMessageTime()) &&
            (up ? (next.message == WM_KEYUP || next.message == WM_SYSKEYUP)
                : (next.message == WM_KEYDOWN || next.message == WM_SYSKEYDOWN))) {
            fake_control_down_ = !up;
            return true;
        }
        if (up) fake_control_down_ = false;
    }
    if (virtual_key == VK_SHIFT) virtual_key = MapVirtualKeyW(scan_code, MAPVK_VSC_TO_VK_EX);
    auto key = mac_key_code(virtual_key, scan_code, extended);
    if (key == no_key_code) return false;
    key = apply_modifier_mapping(key, swap_control_command_);

    Event event;
    event.key_code = key;
    event.timestamp = message_time();
    POINT cursor{};
    GetCursorPos(&cursor);
    ScreenToClient(record.hwnd, &cursor);
    event.location = content_point(record.hwnd, cursor.x, cursor.y);
    if (is_modifier_key(key)) {
        if (repeat || (key == kvk::caps_lock && up)) return true;
        event.type = event_type::flags_changed;
        event.modifier_flags = current_modifier_flags();
        last_flags_ = event.modifier_flags;
        deliver(record, event);
        return true;
    }
    event.type = up ? event_type::key_up : event_type::key_down;
    event.is_repeat = repeat;
    event.modifier_flags = current_modifier_flags() | key_event_flags(key);
    // charactersIgnoringModifiers first: it must not consume a pending dead
    // key that the real translation is about to combine.
    event.characters_ignoring_modifiers = key_characters(virtual_key, scan_code, key, up, true);
    event.characters = key_characters(virtual_key, scan_code, key, up, false);
    deliver(record, event);
    // Alt+F4 keeps closing the window (WM_SYSCOMMAND SC_CLOSE ->
    // close_requested).
    return !(message == WM_SYSKEYDOWN && virtual_key == VK_F4);
}

void Win32WindowServer::resync_modifiers(WindowRecord& record, bool report) {
    if (fake_control_down_ && GetKeyState(VK_RMENU) >= 0) fake_control_down_ = false;
    const auto flags = current_modifier_flags();
    if (flags == last_flags_) return;
    const auto previous = last_flags_;
    last_flags_ = flags;
    if (!report) return;
    // Modifiers changed while another application had the keyboard: report
    // one flagsChanged naming a key whose state differs.
    std::uint16_t key = kvk::caps_lock;
    for (const auto candidate : {kvk::shift, kvk::right_shift, kvk::control, kvk::right_control, kvk::option,
                                 kvk::right_option, kvk::command, kvk::right_command}) {
        const auto bits = modifier_bits(candidate) & ~(modifier::shift | modifier::control | modifier::option |
                                                       modifier::command);
        if ((flags ^ previous) & bits) {
            key = candidate;
            break;
        }
    }
    Event event;
    event.type = event_type::flags_changed;
    event.key_code = key;
    event.modifier_flags = flags;
    deliver(record, event);
}

void Win32WindowServer::handle_char(WindowRecord& record, WPARAM wparam) {
    auto unit = static_cast<char32_t>(wparam);
    if (unit < 0x10000 && is_high_surrogate(unit)) {
        high_surrogate_ = static_cast<wchar_t>(unit);
        return;
    }
    if (unit < 0x10000 && is_low_surrogate(unit)) {
        if (high_surrogate_ == 0) return;
        unit = 0x10000 + ((static_cast<char32_t>(high_surrogate_) - 0xd800) << 10) + (unit - 0xdc00);
    }
    high_surrogate_ = 0;
    if (unit < 0x20 || unit == 0x7f) return;   // control characters belong to key events
    Event event;
    event.kind = EventKind::text;
    append_utf8(event.text, unit);
    event.timestamp = message_time();
    deliver(record, event);
}

// ---- mouse ------------------------------------------------------------------------

void Win32WindowServer::handle_mouse_button(WindowRecord& record, UINT message, WPARAM wparam, LPARAM lparam) {
    std::int64_t button = 0;
    bool down = false;
    switch (message) {
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: down = true; [[fallthrough]];
    case WM_LBUTTONUP: button = 0; break;
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: down = true; [[fallthrough]];
    case WM_RBUTTONUP: button = 1; break;
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: down = true; [[fallthrough]];
    case WM_MBUTTONUP: button = 2; break;
    case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK: down = true; [[fallthrough]];
    default: button = GET_XBUTTON_WPARAM(wparam) == XBUTTON1 ? 3 : 4; break;
    }
    const POINT client{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    const auto buttons = buttons_from_key_state(GET_KEYSTATE_WPARAM(wparam));
    // Drags continue outside the window, as AppKit routes them to the window
    // the drag started in.
    if (down) SetCapture(record.hwnd);
    else if (buttons == 0 && GetCapture() == record.hwnd) ReleaseCapture();
    POINT screen = client;
    ClientToScreen(record.hwnd, &screen);
    Event event;
    event.type = button_type(button, down);
    fill_pointer(event, record, client, buttons);
    event.button_number = button;
    event.click_count = count_click(record, button, down, screen);
    event.pressure = (buttons & 1U) ? 1.0f : 0.0f;
    last_mouse_ = screen;
    have_last_mouse_ = true;
    deliver(record, event);
}

void Win32WindowServer::handle_mouse_move(WindowRecord& record, WPARAM wparam, LPARAM lparam) {
    if (relative_mode_) return;   // raw input reports motion while the cursor is pinned
    const POINT client{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    POINT screen = client;
    ClientToScreen(record.hwnd, &screen);
    if (warp_target_ && warp_target_->x == screen.x && warp_target_->y == screen.y) {
        // The move SetCursorPos generates; CGWarpMouseCursorPosition posts
        // no event.
        warp_target_.reset();
        last_mouse_ = screen;
        have_last_mouse_ = true;
        return;
    }
    if (!record.mouse_inside) {
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, record.hwnd, 0};
        TrackMouseEvent(&track);
        record.mouse_inside = true;
        Event entered;
        entered.type = event_type::mouse_entered;
        fill_pointer(entered, record, client, buttons_from_key_state(wparam));
        deliver(record, entered);
    }
    if (have_last_mouse_ && screen.x == last_mouse_.x && screen.y == last_mouse_.y) return;
    const auto scale = window_scale(record.hwnd);
    Event event;
    const auto buttons = buttons_from_key_state(wparam);
    std::int64_t button = 0;
    event.type = drag_type(buttons, button);
    if (event.type == event_type::mouse_moved && !record.accepts_mouse_moved) {
        last_mouse_ = screen;
        have_last_mouse_ = true;
        return;
    }
    fill_pointer(event, record, client, buttons);
    event.button_number = button;
    if (have_last_mouse_) {
        // NSEvent deltas: points, y positive downward.
        event.delta_x = (screen.x - last_mouse_.x) / scale;
        event.delta_y = (screen.y - last_mouse_.y) / scale;
    }
    event.pressure = (buttons & 1U) ? 1.0f : 0.0f;
    last_mouse_ = screen;
    have_last_mouse_ = true;
    deliver(record, event);
}

void Win32WindowServer::handle_mouse_leave(WindowRecord& record) {
    record.mouse_inside = false;
    POINT cursor{};
    GetCursorPos(&cursor);
    if (WindowFromPoint(cursor) == record.hwnd) {
        // Capture changes cancel tracking without the pointer leaving.
        POINT client = cursor;
        ScreenToClient(record.hwnd, &client);
        RECT area{};
        GetClientRect(record.hwnd, &area);
        if (PtInRect(&area, client)) {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, record.hwnd, 0};
            TrackMouseEvent(&track);
            record.mouse_inside = true;
            return;
        }
    }
    POINT client = cursor;
    ScreenToClient(record.hwnd, &client);
    Event event;
    event.type = event_type::mouse_exited;
    fill_pointer(event, record, client, queue_buttons());
    deliver(record, event);
}

void Win32WindowServer::handle_wheel(WindowRecord& record, UINT message, WPARAM wparam, LPARAM lparam) {
    const POINT screen{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    // Win32 sends the wheel to the focus window; AppKit to the window under
    // the pointer.
    auto target = record_for(WindowFromPoint(screen));
    WindowRecord& window = (target && !target->destroyed) ? *target : record;
    POINT client = screen;
    ScreenToClient(window.hwnd, &client);
    const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
    Event event;
    event.type = event_type::scroll_wheel;
    fill_pointer(event, window, client, buttons_from_key_state(GET_KEYSTATE_WPARAM(wparam)));
    // A precision touchpad's pinch arrives as a vertical wheel with
    // MK_CONTROL although no Ctrl key is down: an NSEvent magnify gesture.
    if (message == WM_MOUSEWHEEL && (GET_KEYSTATE_WPARAM(wparam) & MK_CONTROL) && GetKeyState(VK_CONTROL) >= 0) {
        event.type = event_type::magnify;
        event.magnification = delta * magnification_per_wheel_unit;
        event.scroll_phase = window.magnify_gesture ? phase::changed : phase::began;
        window.magnify_gesture = true;
        SetTimer(window.hwnd, scroll_timer_id, scroll_gesture_timeout_ms, nullptr);
        deliver(window, event);
        return;
    }
    // Precision touchpads report sub-notch deltas; treat those as precise
    // (point) scrolling gestures with began/changed phases and an ended
    // phase after a short pause, as a trackpad does.
    const bool precise = (delta % WHEEL_DELTA) != 0 || window.scroll_gesture;
    double amount = 0;
    if (precise) {
        amount = delta * precise_points_per_wheel_unit;
        event.precise_scrolling = true;
        event.scroll_phase = window.scroll_gesture ? phase::changed : phase::began;
        window.scroll_gesture = true;
        SetTimer(window.hwnd, scroll_timer_id, scroll_gesture_timeout_ms, nullptr);
    } else {
        amount = static_cast<double>(delta) / WHEEL_DELTA;   // lines
    }
    // NSEvent: positive deltaY scrolls up (wheel away from the user),
    // positive deltaX scrolls left; WM_MOUSEHWHEEL is positive to the right.
    if (message == WM_MOUSEHWHEEL) event.delta_x = -amount;
    else event.delta_y = amount;
    deliver(window, event);
}

void Win32WindowServer::end_scroll_gesture(WindowRecord& record) {
    KillTimer(record.hwnd, scroll_timer_id);
    POINT client{};
    GetCursorPos(&client);
    ScreenToClient(record.hwnd, &client);
    for (const auto magnify : {false, true}) {
        auto& active = magnify ? record.magnify_gesture : record.scroll_gesture;
        if (!active) continue;
        active = false;
        Event event;
        event.type = magnify ? event_type::magnify : event_type::scroll_wheel;
        fill_pointer(event, record, client, queue_buttons());
        event.timestamp = monotonic_seconds();
        event.precise_scrolling = !magnify;
        event.scroll_phase = phase::ended;   // NSEvent phase of the gesture
        deliver(record, event);
    }
}

// ---- pen (WM_POINTER) ---------------------------------------------------------------

bool Win32WindowServer::handle_pointer(WindowRecord& record, UINT message, WPARAM wparam, LPARAM) {
    const auto pointer = static_cast<UINT32>(GET_POINTERID_WPARAM(wparam));
    POINTER_INPUT_TYPE type{};
    if (!GetPointerType(pointer, &type) || type != PT_PEN) return false;   // touch and mouse: promoted to mouse
    POINTER_PEN_INFO pen{};
    if (!GetPointerPenInfo(pointer, &pen)) return false;
    const auto& info = pen.pointerInfo;
    POINT client = info.ptPixelLocation;
    ScreenToClient(record.hwnd, &client);
    const POINT screen = info.ptPixelLocation;
    const bool contact = (info.pointerFlags & POINTER_FLAG_INCONTACT) != 0;
    Event event;
    fill_pointer(event, record, client, 0);
    event.timestamp = monotonic_seconds();
    event.pointing_device_type =
        (pen.penFlags & (PEN_FLAG_ERASER | PEN_FLAG_INVERTED)) ? pointing_device::eraser : pointing_device::pen;
    event.device_id = pointer;
    event.pressure = (pen.penMask & PEN_MASK_PRESSURE) ? static_cast<float>(pen.pressure) / 1024.0f
                                                         : (contact ? 1.0f : 0.0f);
    if (pen.penMask & PEN_MASK_TILT_X) event.tilt.x = pen.tiltX / 90.0;
    if (pen.penMask & PEN_MASK_TILT_Y) event.tilt.y = pen.tiltY / 90.0;
    if (pen.penMask & PEN_MASK_ROTATION) event.rotation = static_cast<float>(pen.rotation);
    switch (message) {
    case WM_POINTERENTER:
    case WM_POINTERLEAVE:
        event.type = event_type::tablet_proximity;
        event.entering_proximity = message == WM_POINTERENTER;
        deliver(record, event);
        return true;
    case WM_POINTERDOWN: {
        pen_button_ = (pen.penFlags & PEN_FLAG_BARREL) ? 1 : 0;
        pen_in_contact_ = true;
        event.type = button_type(pen_button_, true);
        event.button_number = pen_button_;
        event.pressed_buttons = 1ULL << pen_button_;
        event.click_count = count_click(record, pen_button_, true, screen);
        break;
    }
    case WM_POINTERUP:
        pen_in_contact_ = false;
        event.type = button_type(pen_button_, false);
        event.button_number = pen_button_;
        event.click_count = count_click(record, pen_button_, false, screen);
        event.pressure = 0;
        break;
    default: {   // WM_POINTERUPDATE
        if (contact && pen_in_contact_) {
            event.type = pen_button_ ? event_type::right_mouse_dragged : event_type::left_mouse_dragged;
            event.button_number = pen_button_;
            event.pressed_buttons = 1ULL << pen_button_;
        } else {
            event.type = event_type::mouse_moved;
            if (!record.accepts_mouse_moved) {
                last_mouse_ = screen;
                have_last_mouse_ = true;
                return true;
            }
        }
        if (have_last_mouse_) {
            const auto scale = window_scale(record.hwnd);
            event.delta_x = (screen.x - last_mouse_.x) / scale;
            event.delta_y = (screen.y - last_mouse_.y) / scale;
        }
        break;
    }
    }
    last_mouse_ = screen;
    have_last_mouse_ = true;
    deliver(record, event);
    return true;
}

// ---- relative mouse (raw input) -------------------------------------------------------

void Win32WindowServer::handle_raw_input(WindowRecord& record, LPARAM lparam) {
    if (!relative_mode_) return;
    RAWINPUT raw{};
    UINT size = sizeof(raw);
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) ==
        static_cast<UINT>(-1))
        return;
    if (raw.header.dwType != RIM_TYPEMOUSE) return;
    const auto& mouse = raw.data.mouse;
    LONG dx = 0, dy = 0;
    if (mouse.usFlags & MOUSE_MOVE_ABSOLUTE) {
        // Tablets and remote sessions report absolute positions (0..65535).
        const bool virtual_desktop = (mouse.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
        const auto width = GetSystemMetrics(virtual_desktop ? SM_CXVIRTUALSCREEN : SM_CXSCREEN);
        const auto height = GetSystemMetrics(virtual_desktop ? SM_CYVIRTUALSCREEN : SM_CYSCREEN);
        const auto x = MulDiv(mouse.lLastX, width, 65535);
        const auto y = MulDiv(mouse.lLastY, height, 65535);
        if (raw_have_absolute_) {
            dx = x - raw_last_x_;
            dy = y - raw_last_y_;
        }
        raw_last_x_ = x;
        raw_last_y_ = y;
        raw_have_absolute_ = true;
    } else {
        dx = mouse.lLastX;
        dy = mouse.lLastY;
    }
    if (dx == 0 && dy == 0) return;
    POINT client = relative_anchor_;
    ScreenToClient(record.hwnd, &client);
    const auto buttons = queue_buttons();
    std::int64_t button = 0;
    Event event;
    event.type = drag_type(buttons, button);
    if (event.type == event_type::mouse_moved && !record.accepts_mouse_moved) return;
    fill_pointer(event, record, client, buttons);
    event.button_number = button;
    const auto scale = window_scale(record.hwnd);
    event.delta_x = dx / scale;
    event.delta_y = dy / scale;
    event.pressure = (buttons & 1U) ? 1.0f : 0.0f;
    deliver(record, event);
}

// ---- IME (IMM32) ----------------------------------------------------------------------

bool Win32WindowServer::handle_ime(WindowRecord& record, UINT message, WPARAM wparam, LPARAM lparam,
                                   LRESULT& result) {
    const auto hwnd = record.hwnd;
    switch (message) {
    case WM_IME_SETCONTEXT:
        // The client draws marked text itself (setMarkedText:); keep the
        // candidate list, hide the IME's own composition window.
        result = DefWindowProcW(hwnd, message, wparam, lparam & ~static_cast<LPARAM>(ISC_SHOWUICOMPOSITIONWINDOW));
        return true;
    case WM_IME_STARTCOMPOSITION:
        position_ime(record);
        result = 0;
        return true;
    case WM_IME_COMPOSITION: {
        ContextLease lease(hwnd);
        if (!lease.context) return false;
        if (lparam & GCS_RESULTSTR) {
            Event event;
            event.kind = EventKind::text;
            event.text = wide_to_utf8(composition_string(lease.context, GCS_RESULTSTR));
            event.marked = false;
            event.timestamp = message_time();
            record.marked_text = false;
            deliver(record, event);
        }
        if (lparam & GCS_COMPSTR) {
            const auto marked = composition_string(lease.context, GCS_COMPSTR);
            Event event;
            event.kind = EventKind::text;
            event.text = wide_to_utf8(marked);
            event.marked = true;
            event.timestamp = message_time();
            auto cursor = static_cast<std::uint64_t>(marked.size());
            if (lparam & GCS_CURSORPOS) {
                const auto position = ImmGetCompositionStringW(lease.context, GCS_CURSORPOS, nullptr, 0);
                if (position >= 0) cursor = std::min<std::uint64_t>(static_cast<std::uint64_t>(position), marked.size());
            }
            event.selection_location = cursor;
            // The clause being converted is the selection (AppKit marked
            // text's selected range); otherwise the caret.
            if (lparam & GCS_COMPATTR) {
                const auto bytes = ImmGetCompositionStringW(lease.context, GCS_COMPATTR, nullptr, 0);
                if (bytes > 0) {
                    std::string attributes(static_cast<std::size_t>(bytes), '\0');
                    ImmGetCompositionStringW(lease.context, GCS_COMPATTR, attributes.data(), static_cast<DWORD>(bytes));
                    const auto target = [](char value) {
                        return value == ATTR_TARGET_CONVERTED || value == ATTR_TARGET_NOTCONVERTED;
                    };
                    const auto begin = std::find_if(attributes.begin(), attributes.end(), target);
                    if (begin != attributes.end()) {
                        const auto end = std::find_if_not(begin, attributes.end(), target);
                        event.selection_location = static_cast<std::uint64_t>(begin - attributes.begin());
                        event.selection_length = static_cast<std::uint64_t>(end - begin);
                    }
                }
            }
            record.marked_text = !marked.empty();
            deliver(record, event);
        }
        position_ime(record);
        result = 0;
        return true;
    }
    case WM_IME_ENDCOMPOSITION:
        if (record.marked_text) {
            record.marked_text = false;
            Event event;
            event.kind = EventKind::text;
            event.marked = true;   // empty marked text: unmarkText
            event.timestamp = message_time();
            deliver(record, event);
        }
        result = 0;
        return true;
    case WM_IME_NOTIFY:
        if (wparam == IMN_OPENCANDIDATE || wparam == IMN_CHANGECANDIDATE) position_ime(record);
        return false;
    case WM_IME_CHAR:
        result = 0;   // already committed from GCS_RESULTSTR
        return true;
    default:
        return false;
    }
}

void Win32WindowServer::position_ime(WindowRecord& record) {
    if (pending_ || record.destroyed || shutting_down_ || !record.sink) return;
    std::optional<Rect> caret;
    {
        const auto sink = record.sink;
        CallbackScope scope(*this, record);
        caret = sink->text_caret_rect(record.id);
    }
    if (!caret || record.destroyed) return;
    ContextLease lease(record.hwnd);
    if (!lease.context) return;
    RECT client{};
    GetClientRect(record.hwnd, &client);
    const auto scale = window_scale(record.hwnd);
    RECT area{static_cast<LONG>(std::lround(caret->origin.x * scale)),
              static_cast<LONG>(std::lround(client.bottom - (caret->origin.y + caret->size.height) * scale)),
              static_cast<LONG>(std::lround((caret->origin.x + caret->size.width) * scale)),
              static_cast<LONG>(std::lround(client.bottom - caret->origin.y * scale))};
    COMPOSITIONFORM composition{CFS_FORCE_POSITION, {area.left, area.top}, {}};
    ImmSetCompositionWindow(lease.context, &composition);
    CANDIDATEFORM candidate{0, CFS_EXCLUDE, {area.left, area.bottom}, area};
    ImmSetCandidateWindow(lease.context, &candidate);
}

void Win32WindowServer::set_text_input_enabled(WindowId window, bool enabled) {
    require_main_thread("set_text_input_enabled");
    const ThreadDpiScope dpi_scope;   // physical pixels whatever the caller's DPI awareness
    const auto target = record(window, "set_text_input_enabled");
    if (target->text_input_enabled == enabled) return;
    if (!enabled && target->marked_text) discard_marked_text(window);
    target->text_input_enabled = enabled;
    if (!ImmAssociateContextEx(target->hwnd, nullptr, enabled ? IACE_DEFAULT : 0))
        fail_win32("ImmAssociateContextEx");
    rethrow_pending();
}

void Win32WindowServer::discard_marked_text(WindowId window) {
    require_main_thread("discard_marked_text");
    const auto target = record(window, "discard_marked_text");
    ContextLease lease(target->hwnd);
    if (lease.context) ImmNotifyIME(lease.context, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
    target->marked_text = false;
}

} // namespace alchemy::gui::win32
