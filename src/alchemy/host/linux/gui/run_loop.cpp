// Linux host backend, area "ui": the GUI run loop's waits. Every waitable
// handle is a descriptor poll(2) reports readable while it is signalled
// (src/alchemy/host/linux/sync.cpp); window input is the X connection's
// socket plus the events XCB has already read and queued.
//
// Dialog-loop hooks have nothing to hook yet: the Linux backend runs no host
// modal loops (no native dialogs, and the window manager moves and resizes
// windows without blocking the client).
#include "alchemy/host/linux/gui/x11.hpp"
#include "alchemy/host/linux/linux.hpp"
#include "alchemy/host/sync.hpp"
#include "alchemy/host/time.hpp"
#include "alchemy/host/ui.hpp"

#include <poll.h>

#include <algorithm>
#include <cerrno>

namespace alchemy::host::ui {

namespace {
constexpr std::size_t kWaitLimit = 63;   // plus the X socket
}

std::size_t wait_limit() noexcept { return kWaitLimit; }

Result<std::size_t> wait_for_input(std::span<const Handle> handles, std::uint32_t timeout_ms, bool messages) {
    if (handles.size() > kWaitLimit || (handles.empty() && !messages)) return Error::invalid_argument;   // nothing to wait for
    auto* connection = messages ? linux_gui::connection() : nullptr;
    const bool forever = timeout_ms == 0xffffffffu;
    const auto deadline = time::monotonic_ns() + static_cast<std::uint64_t>(timeout_ms) * 1000000;
    pollfd fds[kWaitLimit + 1];
    for (;;) {
        if (connection && linux_gui::events_queued()) return handles.size();
        std::size_t count = 0;
        for (const auto handle : handles) fds[count++] = {linux_backend::fd_of(handle), POLLIN, 0};
        if (connection) fds[count++] = {xcb_get_file_descriptor(connection), POLLIN, 0};
        int wait_ms = -1;
        if (!forever) {
            const auto now = time::monotonic_ns();
            wait_ms = now >= deadline ? 0 : static_cast<int>(std::min<std::uint64_t>((deadline - now + 999999) / 1000000, 0x7fffffff));
        }
        const int ready = ::poll(fds, count, wait_ms);
        if (ready < 0) {
            if (errno == EINTR) continue;
            return Error::io;
        }
        if (ready == 0) {
            if (!forever && time::monotonic_ns() >= deadline) return Error::timed_out;
            continue;
        }
        for (std::size_t i = 0; i < handles.size(); ++i) {
            if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            // Take the signal the way sync::wait_any does (an auto-reset
            // event, a mutex or a semaphore is consumed); a lost race polls again.
            const auto taken = sync::wait_any(handles.subspan(i, 1), 0);
            if (taken) return i;
            if (taken.error() != Error::timed_out) return taken.error();
        }
        if (connection && (fds[handles.size()].revents & (POLLIN | POLLHUP | POLLERR))) return handles.size();
    }
}

bool is_gui_thread() noexcept { return linux_gui::connection() != nullptr; }
bool in_modal_loop() noexcept { return false; }
Status install_dialog_loop_hook(std::function<void()>, std::uint32_t) { return {}; }
void enable_dialog_loop_ticks(bool) noexcept {}
void remove_dialog_loop_hook() noexcept {}

} // namespace alchemy::host::ui
