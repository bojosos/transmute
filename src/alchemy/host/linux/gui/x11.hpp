#pragma once

// Linux host backend, GUI areas: the X11 connection the window server
// (window_server.cpp) and the run loop's waits (run_loop.cpp) share. Never
// included outside src/alchemy/host/linux/gui/.
//
// The connection is opened by the first window server request ($DISPLAY:
// WSLg's X server, an X11 desktop, XWayland, or a private Xvfb for cloaked
// runs) and lives for the process.

#include <xcb/xcb.h>

namespace alchemy::host::linux_gui {

// The open connection, or nullptr before the window server opened one.
[[nodiscard]] xcb_connection_t* connection() noexcept;
// Events already read from the socket and queued by XCB (a poll of the
// socket would miss them).
[[nodiscard]] bool events_queued() noexcept;

} // namespace alchemy::host::linux_gui
