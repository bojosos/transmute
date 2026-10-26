#include "alchemy/host/win32/gui/window_server.hpp"

#include "alchemy/gui/runloop.hpp"
#include "alchemy/gui/win32_nesting.hpp"
#include "alchemy/perf.hpp"
#include "alchemy/host/win32/gui/clipboard_codec.hpp"
#include "alchemy/host/win32/gui/cloak.hpp"   // ALCHEMY_GUI_CLOAK (qt-vfb)

#include <imm.h>
#include <ole2.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

// Lifetime, lazy initialization, the WndProc with its exception fence, and
// window management (owner: A3). See include/alchemy/gui/window_server.hpp.
namespace alchemy::gui::win32 {
namespace {

constexpr wchar_t window_class_name[] = L"AlchemyWindow";

struct CreateParameters {
    Win32WindowServer* server{};
    std::shared_ptr<WindowRecord> record;   // null for the application window
};

bool environment_flag(const char* name) {
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) return false;
    const std::string text(value);
    std::free(value);
    return !text.empty() && text != "0";
}

bool desktop_is_interactive() {
    const auto station = GetProcessWindowStation();
    if (station == nullptr) return false;
    USEROBJECTFLAGS flags{};
    if (!GetUserObjectInformationW(station, UOI_FLAGS, &flags, sizeof(flags), nullptr)) return true;
    return (flags.dwFlags & WSF_VISIBLE) != 0;
}

struct Insets {
    LONG left{}, top{}, right{}, bottom{};
};

Insets frame_insets(DWORD style, DWORD ex_style, UINT dpi) {
    RECT rect{0, 0, 100, 100};
    if (!AdjustWindowRectExForDpi(&rect, style, FALSE, ex_style, dpi)) fail_win32("AdjustWindowRectExForDpi");
    return {-rect.left, -rect.top, rect.right - 100, rect.bottom - 100};
}

Insets window_insets(HWND hwnd) {
    return frame_insets(static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)),
                        static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)), GetDpiForWindow(hwnd));
}

UINT dpi_for_scale(double scale) { return static_cast<UINT>(std::lround(scale * USER_DEFAULT_SCREEN_DPI)); }

// The normal (restored) frame of a window in screen pixels; for minimized
// and zoomed windows this is the frame they restore to.
RECT normal_frame(HWND hwnd) {
    if (!IsIconic(hwnd)) {
        RECT rect{};
        if (!GetWindowRect(hwnd, &rect)) fail_win32("GetWindowRect");
        return rect;
    }
    WINDOWPLACEMENT placement{sizeof(placement)};
    if (!GetWindowPlacement(hwnd, &placement)) fail_win32("GetWindowPlacement");
    RECT rect = placement.rcNormalPosition;
    // rcNormalPosition is in workspace coordinates unless WS_EX_TOOLWINDOW.
    if ((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) == 0) {
        MONITORINFO info{sizeof(info)};
        if (GetMonitorInfoW(MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST), &info))
            OffsetRect(&rect, info.rcWork.left - info.rcMonitor.left, info.rcWork.top - info.rcMonitor.top);
    }
    return rect;
}

} // namespace

void fail(const std::string& what) { throw std::runtime_error("Alchemy window server: " + what); }

void fail_win32(const std::string& what) {
    const auto error = GetLastError();
    throw std::runtime_error("Alchemy window server: " + what + " failed (Win32 error " + std::to_string(error) + ")");
}

bool ensure_dpi_awareness() noexcept {
    static std::once_flag once;
    static bool aware = false;
    std::call_once(once, [] {
        aware = SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE;
        if (!aware)
            aware = AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(),
                                                 DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE;
    });
    return aware;
}

ThreadDpiScope::ThreadDpiScope() noexcept
    : previous_(SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {}

ThreadDpiScope::~ThreadDpiScope() {
    if (previous_ != nullptr) SetThreadDpiAwarenessContext(previous_);
}

Win32Style win32_style(std::uint64_t mask, WindowLevel level, bool has_parent) {
    Win32Style result;
    if (mask & style::titled) {
        result.style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
        if (mask & style::miniaturizable) result.style |= WS_MINIMIZEBOX;
        if (mask & style::resizable) result.style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
    } else {
        result.style = WS_POPUP;
    }
    result.style |= WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    if (mask & style::utility_window) result.ex_style |= WS_EX_TOOLWINDOW;
    // Untitled owned windows (menus, tooltips, panels) never appear in the
    // taskbar; untitled top-level windows would.
    if (!(mask & style::titled) && !has_parent) result.ex_style |= WS_EX_TOOLWINDOW;
    if (level > 0) result.ex_style |= WS_EX_TOPMOST;
    return result;
}

LRESULT CALLBACK window_procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    const alchemy::gui::WindowProcedureScope nesting;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        const auto* parameters = static_cast<const CreateParameters*>(create->lpCreateParams);
        if (parameters != nullptr)
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(parameters->server));
    }
    auto* server = reinterpret_cast<Win32WindowServer*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (server == nullptr) return DefWindowProcW(hwnd, message, wparam, lparam);
    // The exception fence: nothing may unwind through Win32 frames
    // (DispatchMessage, modal loops, SendMessage). The message completes with
    // its default result and dispatch_messages() rethrows.
    try {
        return server->handle_message(hwnd, message, wparam, lparam);
    } catch (...) {
        server->stash_exception();
    }
    if (message == WM_CLOSE) return 0;   // DefWindowProc would destroy the window
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

// ---- lifetime -----------------------------------------------------------------

Win32WindowServer::Win32WindowServer()
    : main_thread_(GetCurrentThreadId()),
      interactive_(desktop_is_interactive()),
      swap_control_command_(environment_flag("ALCHEMY_GUI_SWAP_CONTROL_COMMAND")),
      no_activate_(environment_flag("ALCHEMY_GUI_NO_ACTIVATE")),
      trace_(environment_flag("ALCHEMY_GUI_TRACE_WINDOWS")) {}

void Win32WindowServer::trace(const char* format, ...) const {
    if (!trace_) return;
    char buffer[1024];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    std::fprintf(stderr, "[windowserver] %s\n", buffer);
}

Win32WindowServer::~Win32WindowServer() { shutdown(); }

void Win32WindowServer::shutdown() noexcept {
    if (shutting_down_) return;
    shutting_down_ = true;   // no sink is called from here on
    if (GetCurrentThreadId() != main_thread_) return;   // HWNDs can only be destroyed by their thread
    std::vector<std::shared_ptr<WindowRecord>> records;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [id, record] : windows_) records.push_back(record);
    }
    for (const auto& record : records) {
        try {
            register_drop_target(*record, false);
        } catch (...) {
        }
        record->destroyed = true;
        if (record->hwnd) DestroyWindow(record->hwnd);
    }
    if (application_window_) DestroyWindow(application_window_);
    application_window_ = nullptr;
    if (clipping_) ClipCursor(nullptr);
    clipping_ = false;
    if (relative_mode_) {
        RAWINPUTDEVICE device{0x01, 0x02, RIDEV_REMOVE, nullptr};
        RegisterRawInputDevices(&device, 1, sizeof(device));
        relative_mode_ = false;
    }
    for (auto& [id, cursor] : cursors_)
        for (auto& [scale, handle] : cursor.scaled) DestroyCursor(handle);
    cursors_.clear();
    system_cursors_.clear();
    if (ole_initialized_) OleUninitialize();
    ole_initialized_ = false;
}

void Win32WindowServer::require_main_thread(const char* operation) const {
    if (GetCurrentThreadId() != main_thread_)
        fail(std::string(operation) + " must be called on the main thread");
}

void Win32WindowServer::ensure_initialized() const {
    if (initialized_) return;
    require_main_thread("initialization");
    if (!interactive_) fail("no interactive desktop is available (non-interactive window station)");
    ensure_dpi_awareness();
    instance_ = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = window_procedure;
    window_class.hInstance = instance_;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    // Shown until the first frame is presented, and in areas a resize
    // exposes before the next one (the process-lifetime class owns it).
    static const auto background = CreateSolidBrush(unpresented_background);
    window_class.hbrBackground = background ? background : static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    window_class.lpszClassName = window_class_name;
    window_class_ = RegisterClassExW(&window_class);
    if (window_class_ == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) fail_win32("RegisterClassEx");
    // A cloaked run is not ghosted either: when a window stops pumping
    // messages for about 5 s, the shell puts an uncloaked "(Not Responding)"
    // copy of it, last contents included, on the real desktop.
    if (gui_cloak_requested()) DisableProcessWindowsGhosting();
    // The hidden application window: clipboard owner and receiver of the
    // broadcasts that have no window (display and setting changes, session
    // end).
    CreateParameters parameters{const_cast<Win32WindowServer*>(this), nullptr};
    application_window_ = CreateWindowExW(WS_EX_TOOLWINDOW, window_class_name, L"Alchemy", WS_POPUP, 0, 0, 0, 0,
                                          nullptr, nullptr, instance_, &parameters);
    if (application_window_ == nullptr) fail_win32("CreateWindowEx (application window)");
    // The first ShowWindow of a process may be replaced by the STARTUPINFO
    // show state (a launcher's hidden or minimized window style); consume it
    // on the invisible application window so guest windows show as asked.
    ShowWindow(application_window_, SW_HIDE);
    ShowWindow(application_window_, SW_HIDE);
    initialized_ = true;
}

void Win32WindowServer::stash_exception() noexcept {
    if (!pending_) pending_ = std::current_exception();
    // Leave any Win32 modal loop so DispatchMessage returns promptly.
    bool modal = false;
    for (const auto& [id, record] : windows_) modal = modal || record->in_size_move || record->in_menu_loop;
    if (modal) {
        if (GetCapture() != nullptr) ReleaseCapture();
        EndMenu();
    }
}

void Win32WindowServer::rethrow_pending() {
    if (!pending_) return;
    auto pending = pending_;
    pending_ = nullptr;
    std::rethrow_exception(pending);
}

bool Win32WindowServer::dispatch_messages() {
    if (!initialized_) return false;   // no window was ever created: nothing to pump
    require_main_thread("dispatch_messages");
    rethrow_pending();
    bool dispatched = false;
    MSG message{};
    // Bounded so a message flood cannot starve the run loop's timers.
    for (int count = 0; count < 1024 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++count) {
        dispatched = true;
        if (message.message == WM_QUIT) {
            Event event;
            event.kind = EventKind::notification;
            event.notification = Notification::quit_requested;
            deliver_application(event);
            rethrow_pending();
            continue;
        }
        // Only the IME (VK_PROCESSKEY) and injected Unicode (VK_PACKET) need
        // TranslateMessage; every other key computes its characters with
        // ToUnicodeEx in the WndProc (which also advances dead keys).
        if ((message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN) &&
            (message.wParam == VK_PROCESSKEY || message.wParam == VK_PACKET))
            TranslateMessage(&message);
        DispatchMessageW(&message);
        rethrow_pending();
    }
    return dispatched;
}

void Win32WindowServer::set_application_sink(std::shared_ptr<WindowEventSink> sink) {
    require_main_thread("set_application_sink");
    application_sink_ = std::move(sink);
}

// ---- delivery -----------------------------------------------------------------

void Win32WindowServer::deliver(WindowRecord& record, Event& event) {
    if (pending_ || record.destroyed || record.creating || shutting_down_ || !record.sink) return;
    event.window = record.id;
    event.owner = record.owner;
    if (event.timestamp == 0) event.timestamp = monotonic_seconds();
    const auto sink = record.sink;
    CallbackScope scope(*this, record);
    sink->handle_event(event);
}

Win32WindowServer::CallbackScope::CallbackScope(Win32WindowServer& server, WindowRecord& record) noexcept
    : server_(server), record_(record) {
    ++record_.callback_depth;
}

Win32WindowServer::CallbackScope::~CallbackScope() {
    if (--record_.callback_depth != 0 || !record_.destroy_pending || record_.destroyed) return;
    // destroy_window was called from inside this window's own callback:
    // finish it now that the callback returned.
    try {
        if (auto keep = server_.record_for(record_.hwnd)) server_.finish_destroy(keep);
    } catch (...) {
        server_.stash_exception();
    }
}

void Win32WindowServer::deliver_application(Event& event) {
    if (pending_ || shutting_down_ || !application_sink_) return;
    event.window = 0;
    event.owner = 0;
    if (event.timestamp == 0) event.timestamp = monotonic_seconds();
    const auto sink = application_sink_;
    sink->handle_event(event);
}

void Win32WindowServer::notify(WindowRecord& record, Notification notification) {
    Event event;
    event.kind = EventKind::notification;
    event.notification = notification;
    deliver(record, event);
}

// ---- records ------------------------------------------------------------------

std::shared_ptr<WindowRecord> Win32WindowServer::record(WindowId window, const char* operation) const {
    std::lock_guard lock(mutex_);
    const auto found = windows_.find(window);
    if (found == windows_.end() || found->second->destroyed)
        fail(std::string(operation) + ": unknown or destroyed window " + std::to_string(window));
    return found->second;
}

std::shared_ptr<WindowRecord> Win32WindowServer::record_for(HWND hwnd) const {
    std::lock_guard lock(mutex_);
    const auto found = by_hwnd_.find(hwnd);
    return found == by_hwnd_.end() ? nullptr : found->second;
}

std::shared_ptr<WindowRecord> Win32WindowServer::live_record(WindowId window) const {
    std::lock_guard lock(mutex_);
    const auto found = windows_.find(window);
    if (found == windows_.end() || found->second->destroyed) return nullptr;
    return found->second;
}

void Win32WindowServer::change_key_window(WindowId window) {
    if (key_window_ == window) return;
    trace("key window %llu -> %llu", static_cast<unsigned long long>(key_window_),
          static_cast<unsigned long long>(window));
    const auto previous = key_window_;
    key_window_ = window;
    if (window != 0) last_key_window_ = window;
    if (previous != 0) {
        if (const auto old = live_record(previous)) {
            if (old->marked_text) discard_marked_text(previous);
            notify(*old, Notification::resigned_key);
        }
    }
    if (window != 0 && key_window_ == window) {   // a resigned_key callback may have moved it again
        if (const auto now = live_record(window)) notify(*now, Notification::became_key);
    }
}

// ---- the window procedure -------------------------------------------------------

LRESULT Win32WindowServer::handle_message(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        const auto* parameters = static_cast<const CreateParameters*>(create->lpCreateParams);
        if (parameters != nullptr && parameters->record) {
            parameters->record->hwnd = hwnd;
            std::lock_guard lock(mutex_);
            by_hwnd_[hwnd] = parameters->record;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    auto record = record_for(hwnd);
    if (!record) return handle_application_message(hwnd, message, wparam, lparam);
    return handle_window_message(*record, message, wparam, lparam);
}

LRESULT Win32WindowServer::handle_application_message(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (hwnd == application_window_) {
        switch (message) {
        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED: {
            invalidate_layout();
            Event event;
            event.kind = EventKind::notification;
            event.notification = Notification::screens_changed;
            deliver_application(event);
            break;
        }
        case WM_SETTINGCHANGE:
            if (wparam == SPI_SETWORKAREA) {
                invalidate_layout();
                Event event;
                event.kind = EventKind::notification;
                event.notification = Notification::screens_changed;
                deliver_application(event);
            }
            break;
        case WM_QUERYENDSESSION: {
            Event event;
            event.kind = EventKind::notification;
            event.notification = Notification::quit_requested;
            deliver_application(event);
            return TRUE;
        }
        default: break;
        }
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT Win32WindowServer::handle_window_message(WindowRecord& record, UINT message, WPARAM wparam, LPARAM lparam) {
    const auto hwnd = record.hwnd;
    // ALCHEMY_PERF input-to-present latency: input arrival (and its posting
    // time) until the next presented frame.
    if (alchemy::perf::enabled() && ((message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) ||
                                     (message >= WM_KEYFIRST && message <= WM_KEYLAST)))
        alchemy::perf::input_received(static_cast<std::uint32_t>(GetMessageTime()), static_cast<std::uint32_t>(GetTickCount()));
    switch (message) {
    case WM_CLOSE:
        notify(record, Notification::close_requested);
        return 0;
    case WM_NCDESTROY: {
        // Also reached when Win32 destroys the window on its own (an owner's
        // destruction): the record dies with it.
        record.destroyed = true;
        if (key_window_ == record.id) key_window_ = 0;
        if (focus_window_ == record.id) focus_window_ = 0;
        if (last_key_window_ == record.id) last_key_window_ = 0;
        std::lock_guard lock(mutex_);
        by_hwnd_.erase(hwnd);
        windows_.erase(record.id);
        break;
    }
    case WM_PAINT:
        handle_paint(record);
        return 0;
    case WM_SIZE:
        if (wparam == SIZE_MINIMIZED) {
            if (!record.minimized) {
                record.minimized = true;
                notify(record, Notification::miniaturized);
                Event event;
                event.kind = EventKind::notification;
                event.notification = Notification::occlusion_changed;
                event.visible = false;
                deliver(record, event);
            }
            return 0;
        }
        if (record.minimized) {
            record.minimized = false;
            notify(record, Notification::deminiaturized);
            Event event;
            event.kind = EventKind::notification;
            event.notification = Notification::occlusion_changed;
            event.visible = true;
            deliver(record, event);
        }
        if (!record.creating && !record.destroyed) {
            Event event;
            event.kind = EventKind::notification;
            event.notification = Notification::resized;
            event.rect = content_rect(record.id);
            deliver(record, event);
        }
        return 0;
    case WM_MOVE:
        if (!record.creating && !record.destroyed && !record.minimized && !IsIconic(hwnd)) {
            Event event;
            event.kind = EventKind::notification;
            event.notification = Notification::moved;
            event.rect = content_rect(record.id);
            deliver(record, event);
        }
        return 0;
    case WM_SHOWWINDOW:
        record.visible = wparam != 0;
        if (!record.creating) {
            Event event;
            event.kind = EventKind::notification;
            event.notification = Notification::occlusion_changed;
            event.visible = record.visible;
            deliver(record, event);
        }
        break;
    case WM_SETFOCUS:
        focus_window_ = record.id;
        record.refocus_pending = false;
        KillTimer(hwnd, refocus_timer_id);
        resync_modifiers(record, true);
        apply_clip();
        change_key_window(record.id);
        return 0;
    case WM_KILLFOCUS:
        if (focus_window_ == record.id) focus_window_ = 0;
        apply_clip();   // ClipCursor is global: release it while another window has the keyboard
        // The focus moves to a window that is not ours (a dialog, another
        // application) or to none: no window of ours is key. A window of ours
        // receiving it reports became_key, which resigns this one.
        if (key_window_ == record.id && !record_for(reinterpret_cast<HWND>(wparam))) change_key_window(0);
        return 0;
    case WM_ENABLE:
        // A modal dialog owned by this window (NSAlert runModal, TaskDialog,
        // MessageBox) disables it while it runs. When it ends, Win32 normally
        // reactivates the owner; make sure the window that was key gets the
        // keyboard back once the dialog is gone.
        if (wparam != 0 && last_key_window_ == record.id && focus_window_ != record.id) {
            record.refocus_pending = true;
            SetTimer(hwnd, refocus_timer_id, refocus_delay_ms, nullptr);
        }
        break;
    case WM_ACTIVATEAPP:
        if ((wparam != 0) != application_active_) {
            application_active_ = wparam != 0;
            Event event;
            event.kind = EventKind::notification;
            event.notification = application_active_ ? Notification::application_activated
                                                     : Notification::application_deactivated;
            deliver_application(event);
        }
        break;
    case WM_DPICHANGED:
        if (lparam == 0) break;
        handle_dpi_changed(record, LOWORD(wparam), *reinterpret_cast<const RECT*>(lparam));
        return 0;
    case WM_GETDPISCALEDSIZE:
        if (lparam != 0 && handle_dpi_scaled_size(record, static_cast<UINT>(wparam), *reinterpret_cast<SIZE*>(lparam)))
            return TRUE;
        break;
    case WM_WINDOWPOSCHANGED: {
        // DefWindowProc sends WM_SIZE and WM_MOVE (resized / moved) first.
        const auto result = DefWindowProcW(hwnd, message, wparam, lparam);
        handle_position_changed(record);
        return result;
    }
    case WM_GETMINMAXINFO:
        handle_min_max(record, *reinterpret_cast<MINMAXINFO*>(lparam));
        return 0;
    case WM_SIZING:
        if (handle_sizing(record, wparam, *reinterpret_cast<RECT*>(lparam))) return TRUE;
        break;
    case WM_ENTERSIZEMOVE:
        record.in_size_move = true;
        // Win32 runs its own modal loop until WM_EXITSIZEMOVE; the timer keeps
        // the guest's timers and redraws alive inside it (modal_tick).
        SetTimer(hwnd, modal_timer_id, modal_timer_period_ms, nullptr);
        return 0;
    case WM_EXITSIZEMOVE:
        record.in_size_move = false;
        if (!record.in_menu_loop) KillTimer(hwnd, modal_timer_id);
        if (record.live_resizing) {
            record.live_resizing = false;
            notify(record, Notification::live_resize_ended);
        }
        return 0;
    case WM_ENTERMENULOOP:
        record.in_menu_loop = true;
        SetTimer(hwnd, modal_timer_id, modal_timer_period_ms, nullptr);
        break;
    case WM_EXITMENULOOP:
        record.in_menu_loop = false;
        if (!record.in_size_move) KillTimer(hwnd, modal_timer_id);
        break;
    case WM_TIMER:
        if (wparam == modal_timer_id) {
            notify(record, Notification::modal_tick);
            return 0;
        }
        if (wparam == scroll_timer_id) {
            end_scroll_gesture(record);
            return 0;
        }
        if (wparam == refocus_timer_id) {
            KillTimer(hwnd, refocus_timer_id);
            refocus_after_modal(record);
            return 0;
        }
        break;
    case WM_SYSCOMMAND:
        // Alt alone / Alt+letter opens the window menu on Windows; AppKit has
        // no such binding, the keys reach the application instead.
        if ((wparam & 0xfff0) == SC_KEYMENU && lparam != ' ') return 0;
        break;
    case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT) {
            apply_cursor();
            return TRUE;
        }
        break;
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
        if (handle_key(record, message, wparam, lparam)) return 0;
        break;
    case WM_CHAR:
        handle_char(record, wparam);
        return 0;
    case WM_UNICHAR:
        if (wparam == UNICODE_NOCHAR) return TRUE;
        handle_char(record, wparam);
        return 0;
    case WM_SYSCHAR:
        if (wparam == ' ') break;   // Alt+Space: the window menu
        return 0;
    case WM_DEADCHAR:
    case WM_SYSDEADCHAR:
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDBLCLK:
        handle_mouse_button(record, message, wparam, lparam);
        return 0;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
        handle_mouse_button(record, message, wparam, lparam);
        return TRUE;
    case WM_MOUSEMOVE:
        handle_mouse_move(record, wparam, lparam);
        return 0;
    case WM_MOUSELEAVE:
        handle_mouse_leave(record);
        return 0;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        handle_wheel(record, message, wparam, lparam);
        return 0;
    case WM_POINTERENTER:
    case WM_POINTERLEAVE:
    case WM_POINTERDOWN:
    case WM_POINTERUP:
    case WM_POINTERUPDATE:
        if (handle_pointer(record, message, wparam, lparam)) return 0;
        break;
    case WM_INPUT:
        handle_raw_input(record, lparam);
        break;   // DefWindowProc releases the raw input buffer
    case WM_INPUTLANGCHANGE: {
        Event event;
        event.kind = EventKind::notification;
        event.notification = Notification::keyboard_layout_changed;
        deliver_application(event);
        break;
    }
    case WM_DROPFILES: {
        const auto drop = reinterpret_cast<HDROP>(wparam);
        auto paths = dropped_paths(drop);
        POINT point{};
        DragQueryPoint(drop, &point);   // client pixels
        DragFinish(drop);
        ClientToScreen(hwnd, &point);
        deliver_drop(record.id, std::move(paths), point, drag_operation::copy);
        return 0;
    }
    default: {
        LRESULT result = 0;
        if (message >= WM_IME_STARTCOMPOSITION && message <= WM_IME_KEYLAST) {
            if (handle_ime(record, message, wparam, lparam, result)) return result;
        } else if (message >= WM_IME_SETCONTEXT && message <= WM_IME_KEYUP) {
            if (handle_ime(record, message, wparam, lparam, result)) return result;
        }
        break;
    }
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

void Win32WindowServer::handle_paint(WindowRecord& record) {
    PAINTSTRUCT paint{};
    if (BeginPaint(record.hwnd, &paint) == nullptr) return;
    const auto dirty = paint.rcPaint;
    EndPaint(record.hwnd, &paint);
    if (IsRectEmpty(&dirty)) return;
    RECT client{};
    GetClientRect(record.hwnd, &client);
    const auto scale = window_scale(record.hwnd);
    Event event;
    event.kind = EventKind::notification;
    event.notification = Notification::expose;
    event.rect = {{dirty.left / scale, (client.bottom - dirty.bottom) / scale},
                  {(dirty.right - dirty.left) / scale, (dirty.bottom - dirty.top) / scale}};
    deliver(record, event);
}

bool Win32WindowServer::handle_sizing(WindowRecord& record, WPARAM edge, RECT& rect) {
    if (record.in_size_move && !record.live_resizing) {
        record.live_resizing = true;
        notify(record, Notification::live_resize_started);
    }
    const auto insets = window_insets(record.hwnd);
    const auto scale = window_scale(record.hwnd);
    const auto horizontal = insets.left + insets.right;
    const auto vertical = insets.top + insets.bottom;
    const Size proposed{std::max<LONG>(0, rect.right - rect.left - horizontal) / scale,
                        std::max<LONG>(0, rect.bottom - rect.top - vertical) / scale};
    auto size = proposed;
    if (!pending_ && !record.destroyed && !shutting_down_ && record.sink) {
        const auto sink = record.sink;
        CallbackScope scope(*this, record);
        size = sink->will_resize(record.id, proposed);
    }
    if (record.destroyed) return false;
    if (record.minimum.width > 0) size.width = std::max(size.width, record.minimum.width);
    if (record.minimum.height > 0) size.height = std::max(size.height, record.minimum.height);
    if (record.maximum.width > 0) size.width = std::min(size.width, record.maximum.width);
    if (record.maximum.height > 0) size.height = std::min(size.height, record.maximum.height);
    if (size.width == proposed.width && size.height == proposed.height) return false;
    const auto width = static_cast<LONG>(std::lround(std::max(0.0, size.width) * scale)) + horizontal;
    const auto height = static_cast<LONG>(std::lround(std::max(0.0, size.height) * scale)) + vertical;
    if (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT) rect.left = rect.right - width;
    else rect.right = rect.left + width;
    if (edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT) rect.top = rect.bottom - height;
    else rect.bottom = rect.top + height;
    return true;
}

void Win32WindowServer::handle_min_max(WindowRecord& record, MINMAXINFO& info) {
    if (record.minimum.width <= 0 && record.minimum.height <= 0 && record.maximum.width <= 0 &&
        record.maximum.height <= 0)
        return;
    const auto insets = window_insets(record.hwnd);
    const auto scale = window_scale(record.hwnd);
    const auto pixels = [&](double points, LONG extra) {
        return static_cast<LONG>(std::min(1.0e7, std::lround(points * scale) + static_cast<double>(extra)));
    };
    if (record.minimum.width > 0) info.ptMinTrackSize.x = pixels(record.minimum.width, insets.left + insets.right);
    if (record.minimum.height > 0) info.ptMinTrackSize.y = pixels(record.minimum.height, insets.top + insets.bottom);
    if (record.maximum.width > 0 && record.maximum.width < 1.0e6)
        info.ptMaxTrackSize.x = pixels(record.maximum.width, insets.left + insets.right);
    if (record.maximum.height > 0 && record.maximum.height < 1.0e6)
        info.ptMaxTrackSize.y = pixels(record.maximum.height, insets.top + insets.bottom);
}

void Win32WindowServer::handle_position_changed(WindowRecord& record) {
    const auto hwnd = record.hwnd;
    if (record.destroyed || IsIconic(hwnd)) return;   // minimized windows are parked off screen
    RECT frame{};
    if (!GetWindowRect(hwnd, &frame)) return;
    const bool had_origin = record.have_frame_origin;
    const auto previous = record.frame_origin;
    // The AppKit frame origin: the bottom-left corner.
    record.frame_origin = {frame.left, frame.bottom};
    record.have_frame_origin = true;
    if (had_origin && !record.creating && (frame.left != previous.x || frame.bottom != previous.y)) {
        // AppKit child windows (addChildWindow:) move with their parent's
        // frame origin; Win32 owned windows do not move at all.
        const auto dx = frame.left - previous.x;
        const auto dy = frame.bottom - previous.y;
        std::vector<std::shared_ptr<WindowRecord>> children;
        {
            std::lock_guard lock(mutex_);
            for (const auto& [id, other] : windows_)
                if (other->parent == record.id && !other->destroyed && !other->full_screen) children.push_back(other);
        }
        for (const auto& child : children) {
            if (child->destroyed || IsIconic(child->hwnd) || IsZoomed(child->hwnd)) continue;
            RECT child_frame{};
            if (!GetWindowRect(child->hwnd, &child_frame)) continue;
            SetWindowPos(child->hwnd, nullptr, child_frame.left + dx, child_frame.top + dy, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
        }
    }
    // A move to another monitor of the same scale changes the screen only
    // (WM_DPICHANGED reports scale changes itself).
    if (!record.in_dpi_change)
        update_backing(record, window_scale(hwnd),
                       screen_id_for_monitor(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)));
}

void Win32WindowServer::handle_dpi_changed(WindowRecord& record, UINT dpi, const RECT& suggested) {
    invalidate_layout();   // a monitor's scale may have changed
    const auto hwnd = record.hwnd;
    if (dpi == 0) dpi = GetDpiForWindow(hwnd);
    trace("WM_DPICHANGED %llu dpi %u suggested {%ld,%ld %ldx%ld}", static_cast<unsigned long long>(record.id), dpi,
          suggested.left, suggested.top, suggested.right - suggested.left, suggested.bottom - suggested.top);
    record.in_dpi_change = true;
    struct Reset {
        WindowRecord& record;
        ~Reset() { record.in_dpi_change = false; }
    } reset{record};
    if (record.full_screen) {
        // Full screen keeps covering its monitor.
        MONITORINFO info{sizeof(info)};
        if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info))
            SetWindowPos(hwnd, nullptr, info.rcMonitor.left, info.rcMonitor.top,
                         info.rcMonitor.right - info.rcMonitor.left, info.rcMonitor.bottom - info.rcMonitor.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
    } else if (!record.applying_geometry && !IsIconic(hwnd) && suggested.right > suggested.left &&
               suggested.bottom > suggested.top) {
        // The suggested rect keeps the content size in points (see
        // WM_GETDPISCALEDSIZE). apply_content_rect sizes for the new scale
        // itself.
        SetWindowPos(hwnd, nullptr, suggested.left, suggested.top, suggested.right - suggested.left,
                     suggested.bottom - suggested.top, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (record.destroyed) return;
    update_backing(record, static_cast<double>(dpi) / USER_DEFAULT_SCREEN_DPI,
                   screen_id_for_monitor(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)));
    InvalidateRect(hwnd, nullptr, FALSE);
}

bool Win32WindowServer::handle_dpi_scaled_size(WindowRecord& record, UINT dpi, SIZE& size) {
    // The frame size at the new DPI that keeps the content size in points
    // (Win32 would scale the whole frame, title bar included, linearly).
    const auto hwnd = record.hwnd;
    if (dpi == 0 || record.full_screen || IsIconic(hwnd) || IsZoomed(hwnd)) return false;
    RECT client{};
    if (!GetClientRect(hwnd, &client)) return false;
    const auto old_scale = window_scale(hwnd);
    const auto new_scale = static_cast<double>(dpi) / USER_DEFAULT_SCREEN_DPI;
    RECT frame{0, 0, static_cast<LONG>(std::lround(client.right / old_scale * new_scale)),
               static_cast<LONG>(std::lround(client.bottom / old_scale * new_scale))};
    if (!AdjustWindowRectExForDpi(&frame, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)), FALSE,
                                  static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)), dpi))
        return false;
    size = {frame.right - frame.left, frame.bottom - frame.top};
    return true;
}

void Win32WindowServer::place_initial_frame(WindowRecord& record) {
    // NSWindow constrainFrameRect:toScreen: when a titled window is first
    // ordered in, for Win32: its visible frame is kept inside the monitor's
    // work area so the taskbar does not cover it (AppKit only keeps titled
    // windows below the menu bar; the Dock may overlap them). The title bar
    // wins over the bottom edge; a resizable window too tall or wide is
    // shrunk.
    record.placed = true;
    const auto hwnd = record.hwnd;
    if (record.parent != 0 || !(record.style_mask & style::titled) || record.full_screen || IsIconic(hwnd) ||
        IsZoomed(hwnd))
        return;
    RECT frame{};
    if (!GetWindowRect(hwnd, &frame)) return;
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(MonitorFromRect(&frame, MONITOR_DEFAULTTONEAREST), &info)) return;
    const auto& work = info.rcWork;
    // Win32 frames carry invisible resize borders left, right and bottom.
    const auto border = window_insets(hwnd).left;
    RECT visible{frame.left + border, frame.top, frame.right - border, frame.bottom - border};
    auto width = frame.right - frame.left;
    auto height = frame.bottom - frame.top;
    if (record.style_mask & style::resizable) {
        if (visible.bottom - visible.top > work.bottom - work.top) {
            const auto excess = (visible.bottom - visible.top) - (work.bottom - work.top);
            height -= excess;
            visible.bottom -= excess;
        }
        if (visible.right - visible.left > work.right - work.left) {
            const auto excess = (visible.right - visible.left) - (work.right - work.left);
            width -= excess;
            visible.right -= excess;
        }
    }
    LONG dx = 0, dy = 0;
    if (visible.bottom > work.bottom) dy = work.bottom - visible.bottom;
    if (visible.top + dy < work.top) dy = work.top - visible.top;
    if (visible.right > work.right) dx = work.right - visible.right;
    if (visible.left + dx < work.left) dx = work.left - visible.left;
    if (dx == 0 && dy == 0 && width == frame.right - frame.left && height == frame.bottom - frame.top) return;
    trace("place %llu frame {%ld,%ld %ldx%ld} -> {%ld,%ld %ldx%ld} (work area {%ld,%ld %ldx%ld})",
          static_cast<unsigned long long>(record.id), frame.left, frame.top, frame.right - frame.left,
          frame.bottom - frame.top, frame.left + dx, frame.top + dy, width, height, work.left, work.top,
          work.right - work.left, work.bottom - work.top);
    SetWindowPos(hwnd, nullptr, frame.left + dx, frame.top + dy, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

void Win32WindowServer::update_backing(WindowRecord& record, double scale, ScreenId screen) {
    if (!(scale > 0)) return;
    if (std::abs(scale - record.backing_scale) <= 1e-9 && screen == record.screen) return;
    const bool first = record.backing_scale == 0;
    record.backing_scale = scale;
    record.screen = screen;
    if (first) return;   // the creation state, not a change
    Event event;
    event.kind = EventKind::notification;
    event.notification = Notification::backing_changed;
    event.backing_scale = scale;
    event.screen = screen;
    deliver(record, event);
}

void Win32WindowServer::refocus_after_modal(WindowRecord& record) {
    if (!record.refocus_pending) return;
    record.refocus_pending = false;
    const auto hwnd = record.hwnd;
    if (record.destroyed || focus_window_ == record.id || last_key_window_ != record.id) return;
    // Hidden, minimized, or disabled again by another modal dialog.
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || !IsWindowEnabled(hwnd)) return;
    const auto active = GetActiveWindow();
    if (active != nullptr && active != hwnd) return;   // another window of this thread is in front
    if (active == nullptr) {
        // Win32 activated no window of this thread when the dialog closed
        // (an unowned dialog). Take the keyboard back only while the
        // foreground is still this process's (the user dismissed our dialog
        // and did not switch away), and never under no_activate_.
        if (no_activate_) return;
        const auto foreground = GetForegroundWindow();
        DWORD process = 0;
        if (foreground != nullptr) GetWindowThreadProcessId(foreground, &process);
        if (foreground != nullptr && process != GetCurrentProcessId()) return;
        SetForegroundWindow(hwnd);
    }
    SetFocus(hwnd);
}

// ---- geometry -----------------------------------------------------------------

ScreenLayout Win32WindowServer::layout() const {
    {
        std::lock_guard lock(mutex_);
        if (layout_cache_) return *layout_cache_;
    }
    ThreadDpiScope scope;
    ScreenLayout fresh(enumerate_monitors());
    if (fresh.empty()) fail("no display is attached");
    // Cached only while the application window exists to observe display
    // changes.
    if (initialized_) {
        std::lock_guard lock(mutex_);
        layout_cache_ = fresh;
    }
    return fresh;
}

void Win32WindowServer::invalidate_layout() const {
    std::lock_guard lock(mutex_);
    layout_cache_.reset();
    screens_cache_.reset();
    ++screens_generation_;
}

double Win32WindowServer::window_scale(HWND hwnd) const {
    const auto dpi = GetDpiForWindow(hwnd);
    return dpi ? static_cast<double>(dpi) / USER_DEFAULT_SCREEN_DPI : 1.0;
}

PixelRect Win32WindowServer::client_screen_rect(HWND hwnd) const {
    RECT client{};
    if (IsIconic(hwnd)) {
        // Minimized windows keep the frame they restore to.
        auto frame = normal_frame(hwnd);
        const auto insets = window_insets(hwnd);
        return {frame.left + insets.left, frame.top + insets.top, frame.right - insets.right,
                frame.bottom - insets.bottom};
    }
    if (!GetClientRect(hwnd, &client)) fail_win32("GetClientRect");
    POINT origin{0, 0};
    ClientToScreen(hwnd, &origin);
    return {origin.x, origin.y, origin.x + client.right, origin.y + client.bottom};
}

std::size_t Win32WindowServer::window_monitor(const ScreenLayout& screens, HWND hwnd) const {
    const auto index = screens.monitor_for_handle(
        reinterpret_cast<std::uintptr_t>(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)));
    if (index < screens.monitors().size()) return index;
    const auto rect = client_screen_rect(hwnd);
    return screens.monitor_for_pixel({(rect.left + rect.right) / 2.0, (rect.top + rect.bottom) / 2.0});
}

Point Win32WindowServer::content_point(HWND hwnd, LONG client_x, LONG client_y) const {
    RECT client{};
    GetClientRect(hwnd, &client);
    const auto scale = window_scale(hwnd);
    return {client_x / scale, (client.bottom - client_y) / scale};
}

Point Win32WindowServer::screen_point(POINT pixel) const {
    return layout().to_points(PixelPoint{static_cast<double>(pixel.x), static_cast<double>(pixel.y)});
}

void Win32WindowServer::apply_content_rect(WindowRecord& record, Rect content, UINT extra_flags) {
    if (!(content.size.width >= 0) || !(content.size.height >= 0) || !std::isfinite(content.origin.x) ||
        !std::isfinite(content.origin.y))
        fail("content rect is invalid");
    const auto screens = layout();
    const auto monitor = screens.monitor_for_rect(content);
    const auto hwnd = record.hwnd;
    const auto style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    const auto ex_style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    auto frame_for = [&](double scale) {
        const auto pixels = screens.to_pixels(content, monitor, scale);
        RECT frame{pixels.left, pixels.top, pixels.right, pixels.bottom};
        if (!AdjustWindowRectExForDpi(&frame, style, FALSE, ex_style, dpi_for_scale(scale)))
            fail_win32("AdjustWindowRectExForDpi");
        return frame;
    };
    if (IsZoomed(hwnd)) {   // setFrame: on a zoomed window un-zooms it
        if (no_activate_) zoom(record.id);
        else ShowWindow(hwnd, SW_RESTORE);
    }
    const auto target_scale = screens.monitors()[monitor].pixels.scale;
    auto frame = frame_for(target_scale);
    record.applying_geometry = true;
    struct Reset {
        WindowRecord& record;
        ~Reset() { record.applying_geometry = false; }
    } reset{record};
    if (IsIconic(hwnd)) {
        WINDOWPLACEMENT placement{sizeof(placement)};
        GetWindowPlacement(hwnd, &placement);
        if ((ex_style & WS_EX_TOOLWINDOW) == 0) {
            MONITORINFO info{sizeof(info)};
            if (GetMonitorInfoW(MonitorFromRect(&frame, MONITOR_DEFAULTTONEAREST), &info))
                OffsetRect(&frame, info.rcMonitor.left - info.rcWork.left, info.rcMonitor.top - info.rcWork.top);
        }
        placement.rcNormalPosition = frame;
        placement.showCmd = SW_SHOWMINNOACTIVE;
        placement.flags = 0;
        if (!SetWindowPlacement(hwnd, &placement)) fail_win32("SetWindowPlacement");
        return;
    }
    if (!SetWindowPos(hwnd, nullptr, frame.left, frame.top, frame.right - frame.left, frame.bottom - frame.top,
                      SWP_NOZORDER | SWP_NOACTIVATE | extra_flags))
        fail_win32("SetWindowPos");
    // Win32 decides the window's DPI from where it landed; when that is not
    // the scale the rect was computed for, size it again for the real DPI.
    const auto actual = window_scale(hwnd);
    if (std::abs(actual - target_scale) > 1e-6) {
        frame = frame_for(actual);
        if (!SetWindowPos(hwnd, nullptr, frame.left, frame.top, frame.right - frame.left, frame.bottom - frame.top,
                          SWP_NOZORDER | SWP_NOACTIVATE))
            fail_win32("SetWindowPos");
    }
}

void Win32WindowServer::apply_style(WindowRecord& record) {
    const auto hwnd = record.hwnd;
    const auto styles = win32_style(record.style_mask, record.level, record.parent != 0);
    const auto current = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    const auto kept = current & (WS_VISIBLE | WS_MINIMIZE | WS_MAXIMIZE | WS_DISABLED);
    SetWindowLongPtrW(hwnd, GWL_STYLE, static_cast<LONG_PTR>(styles.style | kept));
    const auto ex_current = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE,
                      static_cast<LONG_PTR>((styles.ex_style & ~WS_EX_TOPMOST) | (ex_current & WS_EX_TOPMOST)));
    if (const auto menu = GetSystemMenu(hwnd, FALSE))
        EnableMenuItem(menu, SC_CLOSE,
                       MF_BYCOMMAND | ((record.style_mask & style::closable) ? MF_ENABLED : MF_GRAYED));
}

// ---- windows ------------------------------------------------------------------

WindowId Win32WindowServer::create_window(const WindowDesc& description, std::shared_ptr<WindowEventSink> sink) {
    require_main_thread("create_window");
    const ThreadDpiScope dpi_scope;   // physical pixels whatever the caller's DPI awareness
    ensure_initialized();
    if (!sink) fail("create_window needs an event sink");
    auto record = std::make_shared<WindowRecord>();
    record->sink = std::move(sink);
    record->owner = description.owner;
    record->style_mask = description.style_mask & ~style::full_screen;
    record->level = description.level;
    record->parent = description.parent;
    record->accepts_mouse_moved = description.accepts_mouse_moved;
    record->opaque = description.opaque;
    HWND owner_window = nullptr;
    if (description.parent != 0) owner_window = this->record(description.parent, "create_window parent")->hwnd;
    const auto styles = win32_style(record->style_mask, record->level, owner_window != nullptr);

    const auto screens = layout();
    const auto monitor = screens.monitor_for_rect(description.content_rect);
    const auto scale = screens.monitors()[monitor].pixels.scale;
    const auto pixels = screens.to_pixels(description.content_rect, monitor, scale);
    RECT frame{pixels.left, pixels.top, pixels.right, pixels.bottom};
    if (!AdjustWindowRectExForDpi(&frame, styles.style, FALSE, styles.ex_style, dpi_for_scale(scale)))
        fail_win32("AdjustWindowRectExForDpi");

    {
        std::lock_guard lock(mutex_);
        record->id = next_window_++;
        windows_[record->id] = record;
    }
    CreateParameters parameters{this, record};
    const auto title = utf8_to_wide(description.title);
    const auto hwnd = CreateWindowExW(styles.ex_style, window_class_name, title.c_str(), styles.style, frame.left,
                                      frame.top, frame.right - frame.left, frame.bottom - frame.top, owner_window,
                                      nullptr, instance_, &parameters);
    alchemy::perf::phase("first NSWindow HWND created");
    if (hwnd == nullptr) {
        {
            std::lock_guard lock(mutex_);
            windows_.erase(record->id);
        }
        fail_win32("CreateWindowEx");
    }
    try {
        apply_gui_cloak(hwnd);   // ALCHEMY_GUI_CLOAK=1 (win32_cloak.cpp)
        // AppKit text input is off until an NSTextInputContext activates.
        ImmAssociateContextEx(hwnd, nullptr, 0);
        apply_style(*record);
        apply_content_rect(*record, description.content_rect, SWP_FRAMECHANGED);
        record->backing_scale = window_scale(hwnd);
        record->screen = screen_id_for_monitor(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST));
    } catch (...) {
        record->destroyed = true;
        DestroyWindow(hwnd);
        throw;
    }
    record->creating = false;
    const auto id = record->id;
    if (trace_) GetWindowRect(hwnd, &frame);
    trace("create_window %llu content {%.1f,%.1f %.1fx%.1f} style 0x%llx level %lld parent %llu visible %d"
          " -> frame px {%ld,%ld %ldx%ld} scale %.2f",
          static_cast<unsigned long long>(id),
          description.content_rect.origin.x, description.content_rect.origin.y, description.content_rect.size.width,
          description.content_rect.size.height, static_cast<unsigned long long>(description.style_mask),
          static_cast<long long>(description.level), static_cast<unsigned long long>(description.parent),
          description.visible ? 1 : 0, frame.left, frame.top, frame.right - frame.left, frame.bottom - frame.top,
          record->backing_scale);
    if (description.style_mask & style::full_screen) set_full_screen(id, true);
    if (description.visible) show(id, ShowCommand::order_front);
    rethrow_pending();
    return id;
}

void Win32WindowServer::destroy_window(WindowId window) {
    require_main_thread("destroy_window");
    const auto target = record(window, "destroy_window");
    if (target->callback_depth > 0) {
        // Called from inside this window's own callback: finish when it
        // returns (deliver's depth guard).
        target->destroy_pending = true;
        ShowWindow(target->hwnd, SW_HIDE);
        return;
    }
    finish_destroy(target);
    rethrow_pending();
}

void Win32WindowServer::finish_destroy(const std::shared_ptr<WindowRecord>& target) {
    if (target->destroyed) return;
    target->destroyed = true;
    const auto hwnd = target->hwnd;
    // AppKit child windows outlive their parent; Win32 destroys owned
    // windows with their owner, so release them first.
    std::vector<std::shared_ptr<WindowRecord>> children;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [id, other] : windows_)
            if (other->parent == target->id && !other->destroyed) children.push_back(other);
    }
    for (const auto& child : children) {
        child->parent = 0;
        SetWindowLongPtrW(child->hwnd, GWLP_HWNDPARENT, 0);
    }
    if (confine_window_ == target->id) {
        confine_window_ = 0;
        confine_rect_.reset();
        apply_clip();
    }
    if (target->drop_target || target->drop_files) {
        try {
            register_drop_target(*target, false);
        } catch (...) {
        }
    }
    if (key_window_ == target->id) key_window_ = 0;
    if (focus_window_ == target->id) focus_window_ = 0;
    if (last_key_window_ == target->id) last_key_window_ = 0;
    if (hwnd) DestroyWindow(hwnd);
    {
        std::lock_guard lock(mutex_);
        windows_.erase(target->id);
        by_hwnd_.erase(hwnd);
    }
    target->sink.reset();   // after its last callback
}

void Win32WindowServer::show(WindowId window, ShowCommand command) {
    require_main_thread("show");
    const ThreadDpiScope dpi_scope;   // physical pixels whatever the caller's DPI awareness
    const auto target = record(window, "show");
    trace("show %llu command %u", static_cast<unsigned long long>(window), static_cast<unsigned>(command));
    const auto hwnd = target->hwnd;
    const auto top = target->level > 0 ? HWND_TOPMOST : HWND_TOP;
    if (command != ShowCommand::order_out && !target->placed && !IsWindowVisible(hwnd)) place_initial_frame(*target);
    switch (command) {
    case ShowCommand::order_front:
        if (!IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_SHOWNA);
        SetWindowPos(hwnd, top, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        break;
    case ShowCommand::make_key_and_order_front:
        if (no_activate_) {
            // Shown and key without taking the foreground: the Win32 focus
            // moves only when this application already holds it.
            if (IsIconic(hwnd)) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            else if (!IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_SHOWNA);
            SetWindowPos(hwnd, top, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            if (focus_window_ != 0 && focus_window_ != window && IsWindowEnabled(hwnd)) SetFocus(hwnd);
            else change_key_window(window);
            break;
        }
        ShowWindow(hwnd, IsIconic(hwnd) ? SW_RESTORE : SW_SHOW);
        SetWindowPos(hwnd, top, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
        // Without the foreground (another application is in front), Win32
        // leaves the focus where it is; the window is still key to AppKit
        // and takes the keyboard when the user activates it.
        if (focus_window_ != window) change_key_window(window);
        break;
    case ShowCommand::order_back:
        if (!IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_SHOWNA);
        SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        break;
    case ShowCommand::order_out:
        ShowWindow(hwnd, SW_HIDE);
        // A focused window loses the focus through WM_KILLFOCUS; a logical
        // key window (no_activate_) resigns here.
        if (key_window_ == window && focus_window_ != window) change_key_window(0);
        break;
    default:
        fail("show: unknown command " + std::to_string(static_cast<std::uint32_t>(command)));
    }
    rethrow_pending();
}

void Win32WindowServer::order_relative(WindowId window, WindowId relative, bool above) {
    require_main_thread("order_relative");
    const ThreadDpiScope dpi_scope;
    const auto target = record(window, "order_relative");
    const auto other = record(relative, "order_relative");
    trace("order_relative %llu %s %llu", static_cast<unsigned long long>(window), above ? "above" : "below",
          static_cast<unsigned long long>(relative));
    const auto hwnd = target->hwnd;
    if (!target->placed && !IsWindowVisible(hwnd)) place_initial_frame(*target);
    if (!IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_SHOWNA);
    if (window != relative) {
        // SetWindowPos places a window below its insert-after window: above
        // `relative` is below the window right above it (the top without one).
        HWND after = other->hwnd;
        if (above) {
            after = GetWindow(other->hwnd, GW_HWNDPREV);
            if (after == nullptr) after = target->level > 0 ? HWND_TOPMOST : HWND_TOP;
        }
        if (after != hwnd) SetWindowPos(hwnd, after, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    rethrow_pending();
}

void Win32WindowServer::set_parent(WindowId window, WindowId parent) {
    require_main_thread("set_parent");
    const auto target = record(window, "set_parent");
    trace("set_parent %llu %llu", static_cast<unsigned long long>(window), static_cast<unsigned long long>(parent));
    HWND owner = nullptr;
    if (parent != 0) {
        // No window may end up its own ancestor (Win32 owners form a tree).
        for (auto up = record(parent, "set_parent"); up;) {
            if (up->id == window) fail("set_parent: the window would become its own ancestor");
            up = up->parent != 0 ? record(up->parent, "set_parent") : nullptr;
        }
        owner = record(parent, "set_parent")->hwnd;
    }
    // The owner keeps the window above it and hides it when it is
    // minimized; `parent` makes it move with it (WM_MOVE of the parent).
    target->parent = parent;
    SetWindowLongPtrW(target->hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner));
    rethrow_pending();
}

void Win32WindowServer::set_title(WindowId window, std::string_view title) {
    require_main_thread("set_title");
    const auto target = record(window, "set_title");
    trace("set_title %llu '%.*s'", static_cast<unsigned long long>(window), static_cast<int>(title.size()),
          title.data());
    if (!SetWindowTextW(target->hwnd, utf8_to_wide(title).c_str())) fail_win32("SetWindowText");
    rethrow_pending();
}

void Win32WindowServer::set_style(WindowId window, std::uint64_t style_mask) {
    require_main_thread("set_style");
    const ThreadDpiScope dpi_scope;   // physical pixels whatever the caller's DPI awareness
    const auto target = record(window, "set_style");
    trace("set_style %llu 0x%llx", static_cast<unsigned long long>(window),
          static_cast<unsigned long long>(style_mask));
    const bool full_screen = (style_mask & style::full_screen) != 0;
    const auto mask = style_mask & ~style::full_screen;
    if (mask != target->style_mask) {
        if (target->full_screen) {
            target->style_mask = mask;   // applied when full screen ends
        } else {
            const auto content = content_rect(window);
            target->style_mask = mask;
            apply_style(*target);
            apply_content_rect(*target, content, SWP_FRAMECHANGED);
        }
    }
    if (full_screen != target->full_screen) set_full_screen(window, full_screen);
    rethrow_pending();
}

void Win32WindowServer::set_level(WindowId window, WindowLevel level) {
    require_main_thread("set_level");
    const auto target = record(window, "set_level");
    target->level = level;
    SetWindowPos(target->hwnd, level > 0 ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    rethrow_pending();
}

void Win32WindowServer::set_content_rect(WindowId window, Rect content) {
    require_main_thread("set_content_rect");
    const ThreadDpiScope dpi_scope;   // physical pixels whatever the caller's DPI awareness
    const auto target = record(window, "set_content_rect");
    trace("set_content_rect %llu {%.1f,%.1f %.1fx%.1f}", static_cast<unsigned long long>(window), content.origin.x,
          content.origin.y, content.size.width, content.size.height);
    if (target->full_screen) fail("set_content_rect: the window is in full screen");
    apply_content_rect(*target, content, 0);
    rethrow_pending();
}

Rect Win32WindowServer::content_rect(WindowId window) const {
    require_main_thread("content_rect");
    const ThreadDpiScope dpi_scope;   // physical pixels whatever the caller's DPI awareness
    const auto target = record(window, "content_rect");
    const auto screens = layout();
    return screens.to_points(client_screen_rect(target->hwnd), window_monitor(screens, target->hwnd),
                             window_scale(target->hwnd));
}

Rect Win32WindowServer::frame_rect(WindowId window) const {
    require_main_thread("frame_rect");
    const ThreadDpiScope dpi_scope;   // physical pixels whatever the caller's DPI awareness
    const auto target = record(window, "frame_rect");
    const auto frame = normal_frame(target->hwnd);
    const auto screens = layout();
    return screens.to_points(PixelRect{frame.left, frame.top, frame.right, frame.bottom},
                             window_monitor(screens, target->hwnd), window_scale(target->hwnd));
}

Rect Win32WindowServer::frame_for_content(Rect content, std::uint64_t style_mask) const {
    if (style_mask & style::full_screen) return content;
    const auto screens = layout();
    const auto scale = screens.monitors()[screens.monitor_for_rect(content)].pixels.scale;
    const auto styles = win32_style(style_mask, 0, false);
    const auto insets = frame_insets(styles.style, styles.ex_style, dpi_for_scale(scale));
    return {{content.origin.x - insets.left / scale, content.origin.y - insets.bottom / scale},
            {content.size.width + (insets.left + insets.right) / scale,
             content.size.height + (insets.top + insets.bottom) / scale}};
}

Rect Win32WindowServer::content_for_frame(Rect frame, std::uint64_t style_mask) const {
    if (style_mask & style::full_screen) return frame;
    const auto screens = layout();
    const auto scale = screens.monitors()[screens.monitor_for_rect(frame)].pixels.scale;
    const auto styles = win32_style(style_mask, 0, false);
    const auto insets = frame_insets(styles.style, styles.ex_style, dpi_for_scale(scale));
    return {{frame.origin.x + insets.left / scale, frame.origin.y + insets.bottom / scale},
            {std::max(0.0, frame.size.width - (insets.left + insets.right) / scale),
             std::max(0.0, frame.size.height - (insets.top + insets.bottom) / scale)}};
}

void Win32WindowServer::set_content_limits(WindowId window, Size minimum, Size maximum) {
    require_main_thread("set_content_limits");
    const auto target = record(window, "set_content_limits");
    trace("set_content_limits %llu min %.1fx%.1f max %.1fx%.1f", static_cast<unsigned long long>(window), minimum.width,
          minimum.height, maximum.width, maximum.height);
    target->minimum = minimum;
    target->maximum = maximum;
}

void Win32WindowServer::miniaturize(WindowId window, bool miniaturized) {
    require_main_thread("miniaturize");
    const ThreadDpiScope dpi_scope;   // physical pixels whatever the caller's DPI awareness
    const auto target = record(window, "miniaturize");
    trace("miniaturize %llu %d", static_cast<unsigned long long>(window), miniaturized ? 1 : 0);
    if (miniaturized != (IsIconic(target->hwnd) != FALSE)) {
        if (no_activate_) ShowWindow(target->hwnd, miniaturized ? SW_SHOWMINNOACTIVE : SW_SHOWNOACTIVATE);
        else ShowWindow(target->hwnd, miniaturized ? SW_MINIMIZE : SW_RESTORE);
    }
    rethrow_pending();
}

// -[NSWindow performWindowDragWithEvent:]: Win32's modal move loop of a
// title-bar drag (SC_MOVE with HTCAPTION), from the button that is down;
// WM_ENTERSIZEMOVE keeps the guest's timers alive inside it (modal_tick).
void Win32WindowServer::begin_window_drag(WindowId window) {
    require_main_thread("begin_window_drag");
    const auto target = record(window, "begin_window_drag");
    trace("begin_window_drag %llu", static_cast<unsigned long long>(window));
    if (target->full_screen) fail("begin_window_drag: the window is in full screen");
    ReleaseCapture();
    SendMessageW(target->hwnd, WM_SYSCOMMAND, SC_MOVE | HTCAPTION, 0);
}

// -[NSWindow setSharingType:NSWindowSharingNone]: the window is left out of
// screen capture (Windows 10 2004 and later).
void Win32WindowServer::set_capture_excluded(WindowId window, bool excluded) {
    require_main_thread("set_capture_excluded");
    const auto target = record(window, "set_capture_excluded");
#ifndef WDA_EXCLUDEFROMCAPTURE
    constexpr DWORD exclude_from_capture = 0x00000011;
#else
    constexpr DWORD exclude_from_capture = WDA_EXCLUDEFROMCAPTURE;
#endif
    if (!SetWindowDisplayAffinity(target->hwnd, excluded ? exclude_from_capture : WDA_NONE)) fail_win32("SetWindowDisplayAffinity");
}

void Win32WindowServer::zoom(WindowId window) {
    require_main_thread("zoom");
    const ThreadDpiScope dpi_scope;   // physical pixels whatever the caller's DPI awareness
    const auto target = record(window, "zoom");
    trace("zoom %llu (zoomed %d)", static_cast<unsigned long long>(window), IsZoomed(target->hwnd) ? 1 : 0);
    if (target->full_screen) fail("zoom: the window is in full screen");
    const auto hwnd = target->hwnd;
    if (!no_activate_) {
        ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
    } else if (IsZoomed(hwnd)) {
        if (IsWindowVisible(hwnd)) {
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);   // restores without activating
        } else {
            WINDOWPLACEMENT placement{sizeof(placement)};
            if (!GetWindowPlacement(hwnd, &placement)) fail_win32("GetWindowPlacement");
            RECT frame = placement.rcNormalPosition;   // workspace coordinates
            if ((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) == 0) {
                MONITORINFO info{sizeof(info)};
                if (GetMonitorInfoW(MonitorFromRect(&frame, MONITOR_DEFAULTTONEAREST), &info))
                    OffsetRect(&frame, info.rcWork.left - info.rcMonitor.left, info.rcWork.top - info.rcMonitor.top);
            }
            SetWindowLongPtrW(hwnd, GWL_STYLE, GetWindowLongPtrW(hwnd, GWL_STYLE) & ~WS_MAXIMIZE);
            SetWindowPos(hwnd, nullptr, frame.left, frame.top, frame.right - frame.left, frame.bottom - frame.top,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
    } else {
        // SW_MAXIMIZE always activates. Maximize by style and geometry
        // instead: Win32 keeps the normal placement and restores to it.
        if (IsIconic(hwnd)) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        // Record the restore rect explicitly: Win32 only saves it itself on
        // its own minimize / maximize paths.
        WINDOWPLACEMENT placement{sizeof(placement)};
        if (!GetWindowPlacement(hwnd, &placement)) fail_win32("GetWindowPlacement");
        placement.flags = 0;
        placement.showCmd = IsWindowVisible(hwnd) ? SW_SHOWNOACTIVATE : SW_HIDE;
        if (!SetWindowPlacement(hwnd, &placement)) fail_win32("SetWindowPlacement");
        MONITORINFO info{sizeof(info)};
        if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info)) fail_win32("GetMonitorInfo");
        const auto insets = window_insets(hwnd);
        const auto border = insets.left;   // a maximized frame's borders hang off the work area
        RECT frame = info.rcWork;
        frame.left -= border;
        frame.right += insets.right;
        frame.top -= border;
        frame.bottom += insets.bottom;
        SetWindowLongPtrW(hwnd, GWL_STYLE, GetWindowLongPtrW(hwnd, GWL_STYLE) | WS_MAXIMIZE);
        SetWindowPos(hwnd, nullptr, frame.left, frame.top, frame.right - frame.left, frame.bottom - frame.top,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    rethrow_pending();
}

void Win32WindowServer::set_full_screen(WindowId window, bool full_screen) {
    require_main_thread("set_full_screen");
    const ThreadDpiScope dpi_scope;   // physical pixels whatever the caller's DPI awareness
    const auto target = record(window, "set_full_screen");
    trace("set_full_screen %llu %d", static_cast<unsigned long long>(window), full_screen ? 1 : 0);
    if (full_screen == target->full_screen) return;
    const auto hwnd = target->hwnd;
    if (full_screen) {
        target->saved_placement = {sizeof(WINDOWPLACEMENT)};
        if (!GetWindowPlacement(hwnd, &target->saved_placement)) fail_win32("GetWindowPlacement");
        target->saved_style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        target->saved_ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        MONITORINFO info{sizeof(info)};
        if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info)) fail_win32("GetMonitorInfo");
        const auto kept = target->saved_style & (WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS);
        SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP | kept);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE,
                          target->saved_ex_style & ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE |
                                                     WS_EX_STATICEDGE));
        target->full_screen = true;
        target->applying_geometry = true;
        SetWindowPos(hwnd, HWND_TOP, info.rcMonitor.left, info.rcMonitor.top,
                     info.rcMonitor.right - info.rcMonitor.left, info.rcMonitor.bottom - info.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOACTIVATE);
        target->applying_geometry = false;
        notify(*target, Notification::entered_full_screen);
    } else {
        target->full_screen = false;
        SetWindowLongPtrW(hwnd, GWL_STYLE,
                          (target->saved_style & ~WS_VISIBLE) | (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_VISIBLE));
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, target->saved_ex_style);
        apply_style(*target);
        auto placement = target->saved_placement;
        bool rezoom = false;
        if (!IsWindowVisible(hwnd)) {
            placement.showCmd = SW_HIDE;
        } else if (no_activate_) {
            // SW_SHOWNORMAL / SW_SHOWMAXIMIZED placements activate.
            rezoom = placement.showCmd == SW_SHOWMAXIMIZED;
            if (placement.showCmd != SW_SHOWMINNOACTIVE && placement.showCmd != SW_SHOWMINIMIZED)
                placement.showCmd = SW_SHOWNOACTIVATE;
        }
        target->applying_geometry = true;
        SetWindowPlacement(hwnd, &placement);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
        target->applying_geometry = false;
        if (rezoom && !IsZoomed(hwnd)) zoom(window);
        notify(*target, Notification::exited_full_screen);
    }
    rethrow_pending();
}

void Win32WindowServer::set_accepts_mouse_moved(WindowId window, bool accepts) {
    require_main_thread("set_accepts_mouse_moved");
    record(window, "set_accepts_mouse_moved")->accepts_mouse_moved = accepts;
}

void Win32WindowServer::set_accepts_file_drops(WindowId window, bool accepts) {
    require_main_thread("set_accepts_file_drops");
    const auto target = record(window, "set_accepts_file_drops");
    register_drop_target(*target, accepts);
}

void Win32WindowServer::invalidate(WindowId window, std::optional<Rect> content) {
    require_main_thread("invalidate");
    const ThreadDpiScope dpi_scope;   // physical pixels whatever the caller's DPI awareness
    const auto target = record(window, "invalidate");
    if (!content) {
        InvalidateRect(target->hwnd, nullptr, FALSE);
        return;
    }
    RECT client{};
    GetClientRect(target->hwnd, &client);
    const auto scale = window_scale(target->hwnd);
    RECT dirty{static_cast<LONG>(std::floor(content->origin.x * scale)),
               static_cast<LONG>(std::floor(client.bottom - (content->origin.y + content->size.height) * scale)),
               static_cast<LONG>(std::ceil((content->origin.x + content->size.width) * scale)),
               static_cast<LONG>(std::ceil(client.bottom - content->origin.y * scale))};
    InvalidateRect(target->hwnd, &dirty, FALSE);
}

WindowId Win32WindowServer::key_window() const {
    require_main_thread("key_window");
    return key_window_;
}

bool Win32WindowServer::is_visible(WindowId window) const {
    require_main_thread("is_visible");
    const auto target = record(window, "is_visible");
    return IsWindowVisible(target->hwnd) && !IsIconic(target->hwnd);
}

double Win32WindowServer::backing_scale(WindowId window) const {
    require_main_thread("backing_scale");
    return window_scale(record(window, "backing_scale")->hwnd);
}

ScreenId Win32WindowServer::window_screen(WindowId window) const {
    require_main_thread("window_screen");
    return screen_id_for_monitor(MonitorFromWindow(record(window, "window_screen")->hwnd, MONITOR_DEFAULTTONEAREST));
}

Size Win32WindowServer::content_pixel_size(WindowId window) const {
    require_main_thread("content_pixel_size");
    const ThreadDpiScope dpi_scope;   // physical pixels whatever the caller's DPI awareness
    const auto target = record(window, "content_pixel_size");
    const auto rect = client_screen_rect(target->hwnd);
    return {static_cast<double>(rect.width()), static_cast<double>(rect.height())};
}

void* Win32WindowServer::native_handle(WindowId window) const {
    // Any thread.
    return record(window, "native_handle")->hwnd;
}

} // namespace alchemy::gui::win32

namespace alchemy::host::ui {

std::unique_ptr<WindowSystem> create_window_server() { return std::make_unique<gui::win32::Win32WindowServer>(); }

} // namespace alchemy::host::ui
