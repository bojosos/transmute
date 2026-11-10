#pragma once

// Linux host backend: helpers its area files share (never included outside
// src/alchemy/host/linux/).
//
// A Handle holds a file descriptor plus one, so descriptor 0 (standard input)
// is not the empty Handle. Every object the backend hands out is a
// descriptor: files, pipes, sockets, memfd sections, eventfd events, pidfd
// processes, inotify watches.

#include "alchemy/host/host.hpp"

#include <cerrno>
#include <string>
#include <string_view>

namespace alchemy::host::linux_backend {

[[nodiscard]] inline Handle handle_of(int fd) noexcept {
    return fd < 0 ? Handle{} : Handle{static_cast<std::uintptr_t>(fd) + 1};
}
[[nodiscard]] inline int fd_of(Handle handle) noexcept { return static_cast<int>(handle.value) - 1; }

// errno as a host Error.
[[nodiscard]] Error error(int code) noexcept;
[[nodiscard]] inline Error last_error() noexcept { return error(errno); }

// Drops what the backend remembers about a descriptor host::close is about
// to close (sync.cpp: the kind of an event, mutex or semaphore).
void forget(int fd) noexcept;
// The process.cpp part of forget: reaps a child whose pidfd is closed.
void forget_process(int fd) noexcept;
// The ipc.cpp part: a pending listener closed before it accepted.
void forget_listener(int fd) noexcept;

// A NUL-terminated copy of a path for the system calls.
[[nodiscard]] inline std::string zstring(std::string_view text) { return std::string(text); }

} // namespace alchemy::host::linux_backend
