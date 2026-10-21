#include "alchemy/gui/install.hpp"
#include "alchemy/gui/runloop.hpp"
#include "alchemy/gui/window_server.hpp"
#include "alchemy/host/ui.hpp"

#include <atomic>
#include <cstdlib>
#include <stdexcept>

// The window server (owner: r3). See include/alchemy/gui/window_server.hpp;
// the host's implementation is host::ui::WindowSystem (Win32:
// src/alchemy/host/win32/gui/window_server.cpp).
//
// Installation only creates the server object: no window class, window,
// DPI change or thread exists until the guest first asks for a window,
// clipboard or relative mouse, so `alchemy-fx doctor` and headless runs
// (Blender -b) stay untouched.
namespace alchemy::gui {
namespace {

// Deliberately never deleted: sinks belong to AppKit and may reference guest
// objects that no longer exist during static destruction. Its windows are
// destroyed at exit (on the main thread) without calling any sink.
std::atomic<host::ui::WindowSystem*> instance{nullptr};
std::atomic<bool> host_has_none{false};   // the host backend has no window server

void destroy_windows_at_exit() {
    if (auto* server = instance.load()) server->shutdown();
}

} // namespace

void install_windowserver(InstallContext&) {
    auto* server = instance.load();
    if (server == nullptr) {
        auto created = host::ui::create_window_server();
        if (!created) {
            host_has_none = true;
            return;
        }
        host::ui::WindowSystem* expected = nullptr;
        if (instance.compare_exchange_strong(expected, created.get())) {
            server = created.release();
            std::atexit(destroy_windows_at_exit);
        } else {
            server = expected;
        }
    }
    // The run loop's main pump dispatches Win32 messages through the window
    // server (a no-op until the first window exists).
    if (main_run_loop_available())
        main_run_loop().set_message_dispatcher([server] { return server->dispatch_messages(); });
}

WindowServer& window_server() {
    auto* server = instance.load();
    if (server == nullptr)
        throw std::runtime_error(host_has_none ? "Alchemy window server is not available (the host has none)"
                                               : "Alchemy window server is not available (not installed)");
    if (!server->interactive())
        throw std::runtime_error("Alchemy window server is not available (no interactive desktop)");
    return *server;
}

bool window_server_available() noexcept {
    const auto* server = instance.load();
    return server != nullptr && server->interactive();
}

} // namespace alchemy::gui
