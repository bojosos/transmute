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

// The window server's connection, else a short-lived one of the caller's own
// (queries that work before any window exists: monitors, idle time, ...).
// `connection` is nullptr without an X display.
struct AnyConnection {
    xcb_connection_t* connection{};
    xcb_screen_t* screen{};
    bool owned{};
    AnyConnection() {
        int preferred = 0;
        connection = linux_gui::connection();
        if (!connection) {
            connection = xcb_connect(nullptr, &preferred);
            owned = true;
            if (xcb_connection_has_error(connection)) {
                xcb_disconnect(connection);
                connection = nullptr;
                return;
            }
        }
        auto iterator = xcb_setup_roots_iterator(xcb_get_setup(connection));
        for (int i = 0; i < preferred && iterator.rem; ++i) xcb_screen_next(&iterator);
        screen = iterator.data;
    }
    ~AnyConnection() {
        if (owned && connection) xcb_disconnect(connection);
    }
    AnyConnection(const AnyConnection&) = delete;
    AnyConnection& operator=(const AnyConnection&) = delete;
};

} // namespace alchemy::host::linux_gui
