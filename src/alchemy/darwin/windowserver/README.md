# Window server

**Covers:** the window server behind AppKit
(`include/alchemy/gui/window_server.hpp`): windows, input, cursors,
clipboard, screens, IME, presentation, cloaking (`ALCHEMY_GUI_CLOAK`).
This directory installs it (`install.cpp`), keeps the screen
geometry (`screen_layout.cpp`) and a copy of what the process presented in
its own windows (`presented_pixels.cpp`, recorded by AppKit's software
drawing and the CALayer composition). Own-content screen capture
(`darwin/screencapturekit/`) reads only that copy, so the host never hands
back pixels and the Windows desktop is never captured.

**Host dependencies:** `host::ui::create_window_server()`, a
`host::ui::WindowSystem`. On win32 that is `src/alchemy/host/win32/gui/`
(`window_*.cpp`, `keymap`, `clipboard_codec`, `cloak`: user32, gdi32,
imm32, ole32, shell32, shcore, dwmapi). The null backend has none, so
`window_server_available()` is false there.

**Unsupported:** see the explicit failures in
`src/alchemy/host/win32/gui/window_*.cpp`.
