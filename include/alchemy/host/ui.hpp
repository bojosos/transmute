#pragma once

// Host area "ui" (owner: r3). The rules are in include/alchemy/host/host.hpp. Windows, input,
// displays, vsync, clipboard, drag and drop, dialogs and shell integration. The window server
// (src/alchemy/darwin/windowserver/) runs on this. GUI-only: the backends are
// src/alchemy/host/<backend>/gui/ui.cpp, compiled into alchemy-fx.

#include "alchemy/gui/window_server.hpp"
#include "alchemy/host/host.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy::host::ui {

// The host's window server: Alchemy's window-server contract
// (include/alchemy/gui/window_server.hpp: windows, input, cursors,
// clipboard, screens and IME, in AppKit's points and key codes), which
// darwin/windowserver/ installs and AppKit uses. Creating it creates no
// window, window class or thread: that waits for the first request.
class WindowSystem : public gui::WindowServer {
public:
    // False without an interactive desktop (a service's window station, some
    // CI agents): gui::window_server() then fails explicitly.
    [[nodiscard]] virtual bool interactive() const noexcept = 0;
    // Destroys every window without calling any sink (process exit).
    // Idempotent; a no-op off the main thread.
    virtual void shutdown() noexcept = 0;
};
// The backend's window server; null when the host has none (the null backend).
[[nodiscard]] std::unique_ptr<WindowSystem> create_window_server();
// The client area of a native window in pixels (gui::WindowServer::native_handle);
// Error::not_found when the window no longer exists.
struct PixelSize { std::uint32_t width{}, height{}; };
[[nodiscard]] Result<PixelSize> window_client_size(std::uintptr_t window);

// One active monitor of the host display configuration, in the host's order.
struct Monitor {
    // Its desktop rectangle in pixels (0 x 0 when the host gives no mode).
    std::int32_t left{}, top{};
    std::uint32_t width{}, height{};
    bool primary{};                    // at the desktop origin
    // EDID manufacturer (bytes 8-9, big-endian PNP code) and product code
    // (bytes 10-11), when the monitor reports an EDID.
    std::optional<std::uint16_t> edid_vendor, edid_product;
};
[[nodiscard]] Result<std::vector<Monitor>> monitors();

// Milliseconds since the user's last keyboard or mouse input (any kind).
[[nodiscard]] Result<std::uint32_t> idle_ms();

// The physical layout of the user's keyboard (Carbon's KBGetLayoutType).
enum class KeyboardKind : std::uint8_t { ansi, iso, jis };
[[nodiscard]] Result<KeyboardKind> keyboard_kind();

// The user's display accessibility settings.
[[nodiscard]] Result<bool> reduce_motion();   // interface animations are turned off
[[nodiscard]] Result<bool> high_contrast();   // a high-contrast theme is on
[[nodiscard]] Result<bool> screen_reader();   // a screen reader is running (VoiceOver)
// The user's application theme is dark (NSAppearanceNameDarkAqua).
[[nodiscard]] Result<bool> dark_appearance();
// The color of the screen pixel under the pointer, 8-bit RGB (NSColorSampler).
[[nodiscard]] Result<std::array<std::uint8_t, 3>> pixel_under_pointer();

// ---- shell (NSWorkspace, NSDocumentController) ----
// Paths are host paths (UTF-8).
// Opens a file by its association, or a URL with its handler (`verb`
// "open"; "edit", "print", ... as the host knows them).
[[nodiscard]] Status shell_open(std::string_view target, std::string_view verb = "open", std::string_view parameters = {});
// A file manager window on the file's folder with the file selected.
[[nodiscard]] Status reveal_file(std::string_view path);
// File manager windows, one per folder, with these files selected.
[[nodiscard]] Status reveal_files(std::span<const std::string> paths);
// The host's recent-documents list (Start menu, jump lists).
void add_recent_document(std::string_view path);
// The desktop picture (one for every screen on win32); "" when there is none.
[[nodiscard]] Result<std::string> desktop_picture();
[[nodiscard]] Status set_desktop_picture(std::string_view path);

// A high-resolution one-shot timer a frame loop waits on (CVDisplayLink pacing).
class FrameTimer {
public:
    [[nodiscard]] static Result<FrameTimer> create();
    FrameTimer();                      // no timer
    FrameTimer(FrameTimer&&) noexcept;
    FrameTimer& operator=(FrameTimer&&) noexcept;
    FrameTimer(const FrameTimer&) = delete;
    FrameTimer& operator=(const FrameTimer&) = delete;
    ~FrameTimer();
    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(handle_); }

    // Blocks until `delay_ns` from now, but no longer than `limit_ms`.
    [[nodiscard]] Status wait(std::uint64_t delay_ns, std::uint32_t limit_ms);

private:
    OwnedHandle handle_;
};

// ---- the GUI run loop's waits (darwin/runloop) ----
// The most handles wait_for_input takes.
[[nodiscard]] std::size_t wait_limit() noexcept;
// Waits up to `timeout_ms` for one of `handles` to be signaled or, with
// `messages`, for input in this thread's window message queue (input that
// arrived before the wait counts too). Returns the signaled handle's index,
// handles.size() for input, Error::timed_out, or Error::deadlock for an
// abandoned mutex.
[[nodiscard]] Result<std::size_t> wait_for_input(std::span<const Handle> handles, std::uint32_t timeout_ms, bool messages);
// This thread already uses the host's windowing (never converts it).
[[nodiscard]] bool is_gui_thread() noexcept;
// This GUI thread is inside a host modal loop: a window move or resize, or
// menu tracking.
[[nodiscard]] bool in_modal_loop() noexcept;
// Host dialog loops (message boxes, task dialogs, other modal dialogs) own
// the message queue while they run. With the hook installed on this thread
// and enabled, `tick` runs about every `period_ms` from inside such a loop.
// The hook belongs to the installing thread; remove it there.
[[nodiscard]] Status install_dialog_loop_hook(std::function<void()> tick, std::uint32_t period_ms);
void enable_dialog_loop_ticks(bool enabled) noexcept;
void remove_dialog_loop_hook() noexcept;

// ---- dialogs (NSAlert, NSSavePanel / NSOpenPanel) ----
// Owners are native windows (gui::WindowServer::native_handle); 0 is none.

// A modal message box with its own buttons (NSAlert -runModal): the host's
// task dialog.
enum class AlertIcon : std::uint8_t { information, warning, error };
struct AlertDialog {
    std::string caption, instruction, content;   // "" leaves a part out
    std::vector<std::string> buttons;   // AppKit's order: the first is the default and is laid out rightmost
    std::optional<std::size_t> cancel_button;    // the button Escape answers (none: Escape does nothing)
    AlertIcon icon{};
    double width{};                    // points (0: the host's default width)
    std::uintptr_t owner{};
    bool no_foreground{};              // do not take the foreground from other applications
    std::string verification;          // a checkbox under the buttons ("" none)
    bool verification_checked{};
    // Called once the dialog window exists, with its native handle.
    std::function<void(std::uintptr_t dialog)> created;
};
struct AlertAnswer {
    std::size_t button{};              // index into AlertDialog::buttons
    bool verification_checked{};
};
// Runs the dialog to its end. An exception from `created` propagates after
// the dialog closes.
[[nodiscard]] Result<AlertAnswer> show_alert(const AlertDialog& alert);

// The host's file open / save dialog. Paths are host paths (UTF-8).
struct FileDialog {
    bool save{};                       // a save dialog (it asks before overwriting)
    bool folders{};                    // open: choose folders instead of files
    bool multiple{}, show_hidden{};
    std::string title, ok_label, name_label, name;
    std::string directory;             // the folder it starts in ("" the host's choice)
    std::vector<std::string> extensions;   // offered file types ("" none); the first is a save's default
    bool allow_other_types{};          // also offer "All files"
    std::uintptr_t owner{};
};
// The chosen paths; nullopt when the user cancelled.
[[nodiscard]] Result<std::optional<std::vector<std::string>>> run_file_dialog(const FileDialog& dialog);

// An application-modal session: disables the top-level windows of `windows`
// other than `keep`'s (enabled ones only, each once) and returns them, for
// enable_windows afterwards.
[[nodiscard]] std::vector<std::uintptr_t> disable_windows(std::span<const std::uintptr_t> windows, std::uintptr_t keep);
void enable_windows(std::span<const std::uintptr_t> windows);

// ---- spell checking (NSSpellChecker) ----
// Languages are the host's tags ("en-US"). Text is UTF-16 code units, as
// NSString ranges count them.
[[nodiscard]] Result<std::vector<std::string>> spelling_languages();
struct Misspelling {
    std::size_t start{}, length{};
    bool replace{};                    // the host replaces it automatically (with `replacement`)
    std::u16string replacement;
};
// The misspelled words of `text` the host would underline or replace
// (repeated words and other grammar are not reported). An unknown language
// finds nothing.
[[nodiscard]] Result<std::vector<Misspelling>> check_spelling(std::string_view language, std::u16string_view text);
[[nodiscard]] Result<std::vector<std::u16string>> spelling_suggestions(std::string_view language, std::u16string_view word);

// A hang diagnostic (ALCHEMY_WAIT_DUMP, runloop/wait_trace.cpp): the host
// stack of another thread of this process, sampled by suspending it briefly
// and unwinding with the host's unwind tables until a frame has none
// (translated guest code). At most `max_frames`; empty when the thread could
// not be stopped or read. Error::not_found: no such thread; Error::shut_down:
// it has exited.
struct StackFrame {
    std::uint64_t address{};
    std::string module;                // the host module's file name, "" outside any module
    std::uint64_t offset{};            // from the module's base
};
[[nodiscard]] Result<std::vector<StackFrame>> sample_thread_stack(std::uint64_t thread_id, std::size_t max_frames);

} // namespace alchemy::host::ui
