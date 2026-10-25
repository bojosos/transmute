#pragma once

// Host OS layer: the one interface between the macOS layer
// (src/alchemy/darwin/, the loader and the runtime) and the operating system
// Alchemy runs on. docs/ALCHEMY_PROVIDERS.md describes the pattern and
// docs/ALCHEMY_HOST_LINUX.md maps each area to Linux.
//
// Rules:
// - Only a backend (src/alchemy/host/<backend>/) includes OS headers
//   (windows.h, winsock2.h, WinHTTP, COM, WASAPI, ...). Everything else uses
//   these headers. scripts/check_host_boundary.ps1 (ctest
//   alchemy_host_boundary) enforces it.
// - The API speaks host-neutral types: Handle, Error, byte spans, UTF-8
//   paths and strings. Never Darwin types (errno values, struct stat,
//   kern_return_t: the macOS layer translates) and never OS types.
// - One backend is compiled in, chosen by CMake (ALCHEMY_HOST=win32|null).
//   Headers declare free functions and small concrete classes that the
//   backend defines: no virtual calls, so a host call costs what the OS call
//   costs. Inline helpers here must not depend on the backend.
// - The null backend (src/alchemy/host/null/) compiles everything and fails
//   each call with Error::unsupported. Building the macOS layer against it
//   is the proof that no OS header leaks out of the backends.
// - Policy that holds for every backend (ALCHEMY_NETWORK) lives in
//   src/alchemy/host/common/, above the backends.
//
// Areas (one header each): vm, sections, files, net, dns_sd, http, process,
// ipc, sync, time, locale, icu, crypto, system, ui, audio, midi, gpu, plus
// network_policy. An area's owner extends its header; add a function to
// every backend (null included) in the same commit.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

namespace alchemy::host {

// The backend compiled in ("win32", "null").
[[nodiscard]] std::string_view backend() noexcept;

// Host-neutral failure reasons. They name POSIX error kinds (the macOS layer
// maps them to Darwin errno values, kern_return_t or OSStatus), not values.
enum class Error : std::uint16_t {
    none = 0,
    unsupported,           // the backend cannot do this (ENOSYS / ENOTSUP)
    permission,            // EPERM
    not_found,             // ENOENT
    interrupted,           // EINTR
    io,                    // EIO
    bad_handle,            // EBADF
    no_memory,             // ENOMEM
    access_denied,         // EACCES
    fault,                 // EFAULT
    busy,                  // EBUSY
    exists,                // EEXIST
    cross_device,          // EXDEV
    not_directory,         // ENOTDIR
    is_directory,          // EISDIR
    invalid_argument,      // EINVAL
    too_many_open,         // EMFILE / ENFILE
    no_space,              // ENOSPC
    read_only,             // EROFS
    too_many_links,        // EMLINK
    loop,                  // ELOOP (too many symbolic links)
    broken_pipe,           // EPIPE
    out_of_range,          // ERANGE / EOVERFLOW
    would_block,           // EAGAIN
    in_progress,           // EINPROGRESS
    already,               // EALREADY
    not_socket,            // ENOTSOCK
    message_too_long,      // EMSGSIZE
    destination_required,  // EDESTADDRREQ
    protocol_type,         // EPROTOTYPE
    protocol_option,       // ENOPROTOOPT
    protocol_not_supported, // EPROTONOSUPPORT
    socket_type,           // ESOCKTNOSUPPORT
    protocol_family,       // EPFNOSUPPORT
    address_family,        // EAFNOSUPPORT
    address_in_use,        // EADDRINUSE
    address_not_available, // EADDRNOTAVAIL
    network_down,          // ENETDOWN
    network_unreachable,   // ENETUNREACH
    network_reset,         // ENETRESET
    connection_aborted,    // ECONNABORTED
    connection_reset,      // ECONNRESET
    no_buffers,            // ENOBUFS
    already_connected,     // EISCONN
    not_connected,         // ENOTCONN
    shut_down,             // ESHUTDOWN
    timed_out,             // ETIMEDOUT
    connection_refused,    // ECONNREFUSED
    name_too_long,         // ENAMETOOLONG
    host_down,             // EHOSTDOWN
    host_unreachable,      // EHOSTUNREACH
    not_empty,             // ENOTEMPTY
    deadlock,              // EDEADLK
    no_locks,              // ENOLCK
    denied_by_policy,      // ALCHEMY_NETWORK refused it (the caller picks the errno)
    other,                 // anything else (the backend logs the native code)
};
[[nodiscard]] std::string_view name(Error error) noexcept;

// A value or an Error. Small and header-only, like std::expected (C++23).
template <class T>
class Result {
public:
    Result(T value) : value_(std::move(value)) {}
    Result(Error error) : error_(error) {}
    [[nodiscard]] explicit operator bool() const noexcept { return error_ == Error::none; }
    [[nodiscard]] Error error() const noexcept { return error_; }
    [[nodiscard]] T& operator*() & noexcept { return value_; }
    [[nodiscard]] const T& operator*() const& noexcept { return value_; }
    [[nodiscard]] T&& operator*() && noexcept { return std::move(value_); }
    // std::addressof: a T may overload unary & (WRL ComPtr releases its pointer).
    [[nodiscard]] T* operator->() noexcept { return std::addressof(value_); }
    [[nodiscard]] const T* operator->() const noexcept { return std::addressof(value_); }
    [[nodiscard]] T value_or(T fallback) const& { return error_ == Error::none ? value_ : std::move(fallback); }

private:
    T value_{};
    Error error_{Error::none};
};
template <>
class Result<void> {
public:
    Result() = default;
    Result(Error error) : error_(error) {}
    [[nodiscard]] explicit operator bool() const noexcept { return error_ == Error::none; }
    [[nodiscard]] Error error() const noexcept { return error_; }

private:
    Error error_{Error::none};
};
using Status = Result<void>;

// An OS object (a file, socket, mapping, event, process, pipe, ...): the
// backend's native handle or descriptor, opaque to the macOS layer. 0 is
// "none" on every backend (a backend whose native "none" differs, such as
// INVALID_HANDLE_VALUE or -1, translates).
struct Handle {
    std::uintptr_t value{};
    [[nodiscard]] explicit operator bool() const noexcept { return value != 0; }
    friend bool operator==(Handle, Handle) = default;
};

// Closes any Handle the host API returned.
void close(Handle handle) noexcept;
// Another Handle to the same object (DuplicateHandle / dup).
[[nodiscard]] Result<Handle> duplicate(Handle handle);

// Owns a Handle and closes it.
class OwnedHandle {
public:
    OwnedHandle() = default;
    explicit OwnedHandle(Handle handle) noexcept : handle_(handle) {}
    OwnedHandle(OwnedHandle&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
    OwnedHandle& operator=(OwnedHandle&& other) noexcept {
        if (this != &other) reset(std::exchange(other.handle_, {}));
        return *this;
    }
    OwnedHandle(const OwnedHandle&) = delete;
    OwnedHandle& operator=(const OwnedHandle&) = delete;
    ~OwnedHandle() { reset(); }
    [[nodiscard]] Handle get() const noexcept { return handle_; }
    [[nodiscard]] Handle release() noexcept { return std::exchange(handle_, {}); }
    void reset(Handle handle = {}) noexcept {
        if (handle_) close(handle_);
        handle_ = handle;
    }
    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(handle_); }

private:
    Handle handle_{};
};

using Bytes = std::span<std::byte>;
using ConstBytes = std::span<const std::byte>;

} // namespace alchemy::host
