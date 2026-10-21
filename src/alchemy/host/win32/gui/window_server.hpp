#pragma once

// Win32 host backend, area "ui": the window server (host::ui::WindowSystem,
// the gui::WindowServer contract). Internal to src/alchemy/host/win32/gui/.
// The member functions are split by area:
//   window_server.cpp    lifetime, lazy initialization, the WndProc and its
//                        exception fence, windows and geometry
//   window_input.cpp     keyboard, mouse, pen, scroll, raw input, IME
//   window_services.cpp  cursors, clipboard, screens, keyboard layouts, file
//                        drops, misc
//   window_present.cpp   GDI blit of an AppKit backing store

#include "alchemy/gui/window_server.hpp"
#include "alchemy/host/ui.hpp"
#include "alchemy/host/win32/gui/keymap.hpp"
#include "alchemy/darwin/windowserver/screen_layout.hpp"

#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

struct IDropTarget;

namespace alchemy::gui::win32 {

// Timer ids on window HWNDs.
inline constexpr UINT_PTR modal_timer_id = 0x414c01;    // size/move and menu modal loops
inline constexpr UINT_PTR scroll_timer_id = 0x414c02;   // ends a precise-scroll gesture
inline constexpr UINT_PTR refocus_timer_id = 0x414c03;  // restores the key window after a modal dialog
inline constexpr UINT modal_timer_period_ms = 15;
inline constexpr UINT scroll_gesture_timeout_ms = 120;
inline constexpr UINT refocus_delay_ms = 30;
// Win32 background of a content area nothing has been presented to yet
// (NSColor windowBackgroundColor, dark appearance), and of the area a resize
// exposes before the next frame.
inline constexpr COLORREF unpresented_background = RGB(0x32, 0x32, 0x32);
// Precise (touchpad) wheel deltas: WHEEL_DELTA units -> points.
inline constexpr double precise_points_per_wheel_unit = 30.0 / WHEEL_DELTA;
// Touchpad pinch (Ctrl+wheel without a physical Ctrl key): WHEEL_DELTA
// units -> NSEvent magnification.
inline constexpr double magnification_per_wheel_unit = 0.1 / WHEEL_DELTA;
// Private messages.
inline constexpr UINT deferred_destroy_message = WM_APP + 0x41;

struct WindowRecord {
    WindowId id{};
    HWND hwnd{};
    std::shared_ptr<WindowEventSink> sink;
    std::uint64_t owner{};
    std::uint64_t style_mask{};
    WindowLevel level{};
    WindowId parent{};
    bool accepts_mouse_moved{};
    bool opaque{true};
    bool text_input_enabled{};
    Size minimum{}, maximum{};   // content limits in points; 0 = none

    bool full_screen{};
    WINDOWPLACEMENT saved_placement{};
    LONG_PTR saved_style{}, saved_ex_style{};

    bool creating{true};         // no notifications until create_window returns
    bool destroyed{};
    bool destroy_pending{};      // destroy_window from inside its own callback
    int callback_depth{};
    bool applying_geometry{};    // inside a programmatic SetWindowPos
    bool in_size_move{};
    bool live_resizing{};
    bool in_menu_loop{};
    bool minimized{};
    bool visible{};
    bool mouse_inside{};
    bool scroll_gesture{};
    bool magnify_gesture{};
    bool marked_text{};
    IDropTarget* drop_target{};  // registered OLE drop target (one reference)
    bool drop_files{};           // DragAcceptFiles fallback

    // Frame origin (bottom-left corner, screen pixels) last seen by
    // WM_WINDOWPOSCHANGED: child windows move by the difference.
    POINT frame_origin{};
    bool have_frame_origin{};
    // Last reported backing scale and screen (backing_changed only on change).
    double backing_scale{};
    ScreenId screen{};
    bool in_dpi_change{};        // inside WM_DPICHANGED
    bool refocus_pending{};      // re-enabled after a modal dialog: restore key status
    bool placed{};               // first order-in placement done (place_initial_frame)
};

struct CursorRecord {
    HCURSOR system{};            // shared system cursor, never destroyed
    Image image;                 // custom cursors
    Point hot_spot;
    std::map<int, HCURSOR> scaled;   // per scale percentage
};

class Win32WindowServer final : public host::ui::WindowSystem {
public:
    Win32WindowServer();
    ~Win32WindowServer() override;
    Win32WindowServer(const Win32WindowServer&) = delete;
    Win32WindowServer& operator=(const Win32WindowServer&) = delete;

    // ---- WindowServer ------------------------------------------------------
    WindowId create_window(const WindowDesc& description, std::shared_ptr<WindowEventSink> sink) override;
    void destroy_window(WindowId window) override;
    void show(WindowId window, ShowCommand command) override;
    void order_relative(WindowId window, WindowId relative, bool above) override;
    void set_parent(WindowId window, WindowId parent) override;
    void set_title(WindowId window, std::string_view title) override;
    void set_style(WindowId window, std::uint64_t style_mask) override;
    void set_level(WindowId window, WindowLevel level) override;
    void set_content_rect(WindowId window, Rect content_rect) override;
    Rect content_rect(WindowId window) const override;
    Rect frame_rect(WindowId window) const override;
    Rect frame_for_content(Rect content_rect, std::uint64_t style_mask) const override;
    Rect content_for_frame(Rect frame_rect, std::uint64_t style_mask) const override;
    void set_content_limits(WindowId window, Size minimum, Size maximum) override;
    void miniaturize(WindowId window, bool miniaturized) override;
    void zoom(WindowId window) override;
    void set_full_screen(WindowId window, bool full_screen) override;
    void set_accepts_mouse_moved(WindowId window, bool accepts) override;
    void set_accepts_file_drops(WindowId window, bool accepts) override;
    void invalidate(WindowId window, std::optional<Rect> content_rect) override;
    WindowId key_window() const override;
    bool is_visible(WindowId window) const override;
    bool window_exists(WindowId window) const override { return live_record(window) != nullptr; }
    double backing_scale(WindowId window) const override;
    ScreenId window_screen(WindowId window) const override;
    Size content_pixel_size(WindowId window) const override;
    void* native_handle(WindowId window) const override;

    std::uint64_t modifier_flags() const override;
    std::uint64_t pressed_mouse_buttons() const override;
    Point mouse_location() const override;
    double double_click_interval() const override;
    double key_repeat_delay() const override;
    double key_repeat_interval() const override;

    CursorId system_cursor(SystemCursor cursor) override;
    CursorId create_cursor(const Image& image, Point hot_spot) override;
    void destroy_cursor(CursorId cursor) override;
    void set_cursor(CursorId cursor) override;
    void set_cursor_visible(bool visible) override;
    void warp_cursor(Point screen_location) override;
    void associate_mouse_and_cursor(bool associated) override;
    void confine_cursor(WindowId window, std::optional<Rect> content_rect) override;

    std::optional<std::string> clipboard_text() const override;
    void set_clipboard_text(std::string_view text) override;
    std::optional<Image> clipboard_image() const override;
    void set_clipboard_image(const Image& image) override;
    std::int64_t clipboard_change_count() const override;

    std::vector<ScreenInfo> screens() const override;

    KeyboardLayout keyboard_layout() const override;
    std::string translate_key(std::uint16_t key_code, std::uint64_t modifier_flags, DeadKeyState& state) const override;
    void set_text_input_enabled(WindowId window, bool enabled) override;
    void discard_marked_text(WindowId window) override;

    bool dispatch_messages() override;
    void set_application_sink(std::shared_ptr<WindowEventSink> sink) override;

    void beep() override;
    void request_attention(bool critical) override;
    // window_present.cpp: GDI blit of an AppKit backing store.
    void present_bitmap(WindowId window, const BitmapView& pixels, std::optional<Rect> content_rect) override;

    // ---- internal ------------------------------------------------------------
    // The fenced window procedure body (called by the static WndProc).
    LRESULT handle_message(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    // Stashes the in-flight exception for dispatch_messages to rethrow (the
    // first one wins) and unwinds any Win32 modal loop.
    void stash_exception() noexcept;
    // Delivers an event to a window's sink (fills window, owner, timestamp).
    void deliver(WindowRecord& record, Event& event);
    // Files dropped through OLE or WM_DROPFILES; location in screen pixels,
    // operations = the drag_operation:: mask the source allows.
    void deliver_drop(WindowId window, std::vector<std::string> paths, POINT screen_pixel,
                      std::uint64_t operations);
    // An OLE file drag over a window (drag_entered / drag_updated /
    // drag_exited); returns the drag_operation:: mask the window accepts
    // (none for drag_exited or when nothing was delivered).
    std::uint64_t deliver_drag(WindowId window, Notification notification, const std::vector<std::string>& paths,
                               POINT screen_pixel, std::uint64_t operations);
    [[nodiscard]] bool accepting_drops(WindowId window) const;
    // The window's registered OLE drop target (no reference added), or null
    // (not accepting drops, or the WM_DROPFILES fallback). For tests.
    [[nodiscard]] IDropTarget* drop_target(WindowId window) const;
    // ALCHEMY_GUI_NO_ACTIVATE=1: windows never take the foreground or the
    // keyboard focus on their own (unattended and test runs); key status is
    // tracked logically. User clicks still activate normally.
    [[nodiscard]] bool no_activate() const noexcept { return no_activate_; }
    void set_no_activate(bool enabled) noexcept { no_activate_ = enabled; }
    [[nodiscard]] DWORD main_thread() const noexcept { return main_thread_; }
    // False on a non-interactive window station (services, some CI agents):
    // window_server() then fails explicitly.
    [[nodiscard]] bool interactive() const noexcept override { return interactive_; }
    // Destroys every window without calling any sink (process exit).
    // Idempotent; a no-op off the main thread (HWNDs belong to it).
    void shutdown() noexcept override;

private:
    // Counts a sink callback of a window; when the outermost one returns, a
    // destroy_window deferred from inside it completes.
    class CallbackScope {
    public:
        CallbackScope(Win32WindowServer& server, WindowRecord& record) noexcept;
        ~CallbackScope();
        CallbackScope(const CallbackScope&) = delete;
        CallbackScope& operator=(const CallbackScope&) = delete;

    private:
        Win32WindowServer& server_;
        WindowRecord& record_;
    };

    void ensure_initialized() const;
    void require_main_thread(const char* operation) const;
    void rethrow_pending();
    [[nodiscard]] std::shared_ptr<WindowRecord> record(WindowId window, const char* operation) const;
    [[nodiscard]] std::shared_ptr<WindowRecord> record_for(HWND hwnd) const;
    [[nodiscard]] std::shared_ptr<WindowRecord> live_record(WindowId window) const;   // null when gone
    void finish_destroy(const std::shared_ptr<WindowRecord>& record);
    void deliver_application(Event& event);
    void notify(WindowRecord& record, Notification notification);

    // geometry
    [[nodiscard]] ScreenLayout layout() const;
    void invalidate_layout() const;
    [[nodiscard]] double window_scale(HWND hwnd) const;
    [[nodiscard]] PixelRect client_screen_rect(HWND hwnd) const;
    [[nodiscard]] std::size_t window_monitor(const ScreenLayout& layout, HWND hwnd) const;
    void apply_content_rect(WindowRecord& record, Rect content_rect, UINT extra_flags);
    [[nodiscard]] Point content_point(HWND hwnd, LONG client_x, LONG client_y) const;
    [[nodiscard]] Point screen_point(POINT screen_pixel) const;
    [[nodiscard]] Rect content_rect_to_client(HWND hwnd, Rect rect, RECT& pixels) const;
    void apply_style(WindowRecord& record);

    // window procedure parts
    LRESULT handle_application_message(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle_window_message(WindowRecord& record, UINT message, WPARAM wparam, LPARAM lparam);
    bool handle_sizing(WindowRecord& record, WPARAM edge, RECT& rect);
    void handle_min_max(WindowRecord& record, MINMAXINFO& info);
    void handle_paint(WindowRecord& record);
    void handle_position_changed(WindowRecord& record);
    void handle_dpi_changed(WindowRecord& record, UINT dpi, const RECT& suggested);
    bool handle_dpi_scaled_size(WindowRecord& record, UINT dpi, SIZE& size);
    void refocus_after_modal(WindowRecord& record);
    void place_initial_frame(WindowRecord& record);
    // Reports backing_changed when the scale or the screen differs from the
    // last report.
    void update_backing(WindowRecord& record, double scale, ScreenId screen);
    // Key status moves to `window` (0: none), with resigned_key /
    // became_key for the windows involved.
    void change_key_window(WindowId window);

    // input (win32_input.cpp)
    bool handle_key(WindowRecord& record, UINT message, WPARAM wparam, LPARAM lparam);
    void handle_char(WindowRecord& record, WPARAM wparam);
    void handle_mouse_button(WindowRecord& record, UINT message, WPARAM wparam, LPARAM lparam);
    void handle_mouse_move(WindowRecord& record, WPARAM wparam, LPARAM lparam);
    void handle_mouse_leave(WindowRecord& record);
    void handle_wheel(WindowRecord& record, UINT message, WPARAM wparam, LPARAM lparam);
    void end_scroll_gesture(WindowRecord& record);
    bool handle_pointer(WindowRecord& record, UINT message, WPARAM wparam, LPARAM lparam);
    void handle_raw_input(WindowRecord& record, LPARAM lparam);
    bool handle_ime(WindowRecord& record, UINT message, WPARAM wparam, LPARAM lparam, LRESULT& result);
    void position_ime(WindowRecord& record);
    void resync_modifiers(WindowRecord& record, bool report);
    [[nodiscard]] PhysicalModifiers physical_modifiers() const;
    [[nodiscard]] std::uint64_t current_modifier_flags() const;
    void fill_pointer(Event& event, WindowRecord& record, POINT client, std::uint64_t buttons);
    [[nodiscard]] double message_time() const;
    // Click count of a button press / release (Win32 double-click time and
    // rectangle, any number of clicks).
    std::int64_t count_click(WindowRecord& record, std::int64_t button, bool down, POINT screen);
    // NSEvent characters / charactersIgnoringModifiers of a key message.
    std::string key_characters(UINT virtual_key, UINT scan_code, std::uint16_t key_code, bool up,
                               bool ignoring_modifiers);

    // services (win32_services.cpp)
    void apply_cursor();
    [[nodiscard]] HCURSOR cursor_handle(CursorId cursor);
    [[nodiscard]] bool cursor_over_client() const;
    void apply_clip();
    void register_drop_target(WindowRecord& record, bool accepts);

    DWORD main_thread_{};
    bool interactive_{};
    mutable bool initialized_{};
    mutable HINSTANCE instance_{};
    mutable ATOM window_class_{};
    mutable HWND application_window_{};
    bool swap_control_command_{};
    bool shutting_down_{};

    mutable std::mutex mutex_;   // windows_ (native_handle) and the layout cache
    std::map<WindowId, std::shared_ptr<WindowRecord>> windows_;
    std::unordered_map<HWND, std::shared_ptr<WindowRecord>> by_hwnd_;
    WindowId next_window_{1};
    mutable std::optional<ScreenLayout> layout_cache_;
    // screens() result (display configuration queries, DCs, friendly names:
    // about 1 ms per call), dropped with the layout cache on display, DPI and
    // work-area changes.
    mutable std::optional<std::vector<ScreenInfo>> screens_cache_;
    mutable std::uint64_t screens_generation_{};   // bumped by invalidate_layout

    std::shared_ptr<WindowEventSink> application_sink_;
    std::exception_ptr pending_;
    WindowId key_window_{};      // AppKit key window (logical under no_activate_)
    WindowId focus_window_{};    // the window holding the Win32 keyboard focus
    WindowId last_key_window_{}; // the most recent key window (not cleared on resign)
    bool application_active_{};
    bool no_activate_{};
    bool trace_{};               // ALCHEMY_GUI_TRACE_WINDOWS=1: window calls on stderr

public:
    // printf-style diagnostics on stderr while ALCHEMY_GUI_TRACE_WINDOWS=1.
    void trace(const char* format, ...) const;

private:

    // keyboard
    bool fake_control_down_{};
    wchar_t high_surrogate_{};
    std::uint64_t last_flags_{};

    // mouse
    struct Click {
        WindowId window{};
        std::int64_t button{-1};
        LONG time{};
        POINT position{};
        std::int64_t count{};
    } click_;
    POINT last_mouse_{};
    bool have_last_mouse_{};
    std::optional<POINT> warp_target_;
    bool relative_mode_{};
    POINT relative_anchor_{};
    LONG raw_last_x_{}, raw_last_y_{};
    bool raw_have_absolute_{};
    WindowId confine_window_{};
    std::optional<Rect> confine_rect_;
    bool clipping_{};            // a ClipCursor of ours is in effect
    std::int64_t pen_button_{};
    bool pen_in_contact_{};

    // cursors
    std::map<CursorId, CursorRecord> cursors_;
    std::map<SystemCursor, CursorId> system_cursors_;
    CursorId next_cursor_{1};
    CursorId current_cursor_{};
    int cursor_hide_count_{};

    // file drops
    bool ole_initialized_{};
    bool ole_unavailable_{};
};

// The window class's WndProc.
LRESULT CALLBACK window_procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

// Process-wide per-monitor-v2 DPI awareness (once; any thread). Returns
// false when the process was already set to another awareness.
bool ensure_dpi_awareness() noexcept;

// Scoped per-monitor-v2 awareness for the calling thread (screen queries on
// threads of a process whose awareness could not be set).
class ThreadDpiScope {
public:
    ThreadDpiScope() noexcept;
    ~ThreadDpiScope();
    ThreadDpiScope(const ThreadDpiScope&) = delete;
    ThreadDpiScope& operator=(const ThreadDpiScope&) = delete;

private:
    DPI_AWARENESS_CONTEXT previous_{};
};

// Screen enumeration (monitor pixel rects and scales, primary first).
[[nodiscard]] std::vector<MonitorPixels> enumerate_monitors();
[[nodiscard]] ScreenId screen_id_for_monitor(HMONITOR monitor);
// The UTF-8 paths of an HDROP.
[[nodiscard]] std::vector<std::string> dropped_paths(HANDLE drop);
// OLE drag effects -> the drag_operation:: mask the source allows, narrowed
// by the modifier keys (MK_* key state); and an accepted mask -> the effect
// shown (copy, link, move in that order of preference).
[[nodiscard]] std::uint64_t drag_operations_for(DWORD allowed_effects, DWORD key_state);
[[nodiscard]] DWORD drop_effect_for(std::uint64_t operations);

// Win32 window styles for an NSWindowStyleMask.
struct Win32Style {
    DWORD style{}, ex_style{};
};
[[nodiscard]] Win32Style win32_style(std::uint64_t style_mask, WindowLevel level, bool has_parent);

[[noreturn]] void fail(const std::string& what);
[[noreturn]] void fail_win32(const std::string& what);

} // namespace alchemy::gui::win32
