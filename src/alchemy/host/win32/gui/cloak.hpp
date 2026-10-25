#pragma once

// ALCHEMY_GUI_CLOAK=1: every window-server window is DWM-cloaked right after
// creation (DwmSetWindowAttribute(DWMWA_CLOAK)). A cloaked window keeps its
// whole USER life (it is "visible", gets WM_PAINT, sizes, DPI, GDI and
// swapchain presentation) but the desktop compositor never shows it and it
// never takes part in hit testing, so unattended runs (harnesses, agents
// sharing a desktop with input tests) cannot cover or intercept anything on
// the real desktop. Capture pixels through the present/backing-store capture
// paths, never from the screen. Owner of this file: qt-vfb; hooked from
// Win32WindowServer::create_window. A cloaked run also turns off window
// ghosting (Win32WindowServer's initialisation): a hung guest's ghost window
// is the shell's, not cloaked, and shows the window's last contents.

#include <windows.h>

namespace alchemy::gui::win32 {

// True when ALCHEMY_GUI_CLOAK is set to something other than "" or "0".
// Read once per process.
[[nodiscard]] bool gui_cloak_requested() noexcept;

// Cloaks `hwnd` when gui_cloak_requested(); fails with std::runtime_error
// when DWM refuses (a run that asked for cloaking never shows a window), and
// verifies the result with DWMWA_CLOAKED.
void apply_gui_cloak(HWND hwnd);

// DWMWA_CLOAKED of `hwnd` (nonzero: cloaked by the app, shell or inherited).
[[nodiscard]] DWORD gui_cloak_state(HWND hwnd) noexcept;

} // namespace alchemy::gui::win32
