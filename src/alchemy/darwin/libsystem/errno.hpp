#pragma once

// The one translation of host errors into Darwin errno values (<sys/errno.h>),
// for every framework of the macOS layer: a host call fails with a
// host::Error (alchemy/host/host.hpp), and the macOS layer turns it into the
// Darwin result at the call site. kern_return_t and OSStatus callers map from
// the errno or from host::Error themselves.
#include "alchemy/host/host.hpp"

#include <cstdint>

namespace alchemy::libsystem {

// Darwin <sys/errno.h> values used below.
namespace darwin {
inline constexpr std::uint32_t eperm = 1, enoent = 2, eintr = 4, eio = 5, ebadf = 9, edeadlk = 11, enomem = 12,
    eacces = 13, efault = 14, ebusy = 16, eexist = 17, exdev = 18, enotdir = 20, eisdir = 21, einval = 22,
    emfile = 24, enospc = 28, erofs = 30, emlink = 31, epipe = 32, erange = 34, eagain = 35, einprogress = 36,
    ealready = 37, enotsock = 38, edestaddrreq = 39, emsgsize = 40, eprototype = 41, enoprotoopt = 42,
    eprotonosupport = 43, esocktnosupport = 44, enotsup = 45, epfnosupport = 46, eafnosupport = 47,
    eaddrinuse = 48, eaddrnotavail = 49, enetdown = 50, enetunreach = 51, enetreset = 52, econnaborted = 53,
    econnreset = 54, enobufs = 55, eisconn = 56, enotconn = 57, eshutdown = 58, etimedout = 60,
    econnrefused = 61, eloop = 62, enametoolong = 63, ehostdown = 64, ehostunreach = 65, enotempty = 66,
    enolck = 77, enosys = 78;
} // namespace darwin

// The Darwin errno for `error`. Two kinds need the caller's context and get a
// default here: Error::unsupported is ENOTSUP (an interface that does not
// exist at all is ENOSYS: check for unsupported first), and
// Error::denied_by_policy is ENETUNREACH (what the network policy's refusals
// report). Error::other, which the backend logs, is EIO.
[[nodiscard]] constexpr std::uint32_t darwin_errno(host::Error error) noexcept {
    using host::Error;
    switch (error) {
    case Error::none: return 0;
    case Error::unsupported: return darwin::enotsup;
    case Error::permission: return darwin::eperm;
    case Error::not_found: return darwin::enoent;
    case Error::interrupted: return darwin::eintr;
    case Error::io: return darwin::eio;
    case Error::bad_handle: return darwin::ebadf;
    case Error::no_memory: return darwin::enomem;
    case Error::access_denied: return darwin::eacces;
    case Error::fault: return darwin::efault;
    case Error::busy: return darwin::ebusy;
    case Error::exists: return darwin::eexist;
    case Error::cross_device: return darwin::exdev;
    case Error::not_directory: return darwin::enotdir;
    case Error::is_directory: return darwin::eisdir;
    case Error::invalid_argument: return darwin::einval;
    case Error::too_many_open: return darwin::emfile;
    case Error::no_space: return darwin::enospc;
    case Error::read_only: return darwin::erofs;
    case Error::too_many_links: return darwin::emlink;
    case Error::loop: return darwin::eloop;
    case Error::broken_pipe: return darwin::epipe;
    case Error::out_of_range: return darwin::erange;
    case Error::would_block: return darwin::eagain;
    case Error::in_progress: return darwin::einprogress;
    case Error::already: return darwin::ealready;
    case Error::not_socket: return darwin::enotsock;
    case Error::message_too_long: return darwin::emsgsize;
    case Error::destination_required: return darwin::edestaddrreq;
    case Error::protocol_type: return darwin::eprototype;
    case Error::protocol_option: return darwin::enoprotoopt;
    case Error::protocol_not_supported: return darwin::eprotonosupport;
    case Error::socket_type: return darwin::esocktnosupport;
    case Error::protocol_family: return darwin::epfnosupport;
    case Error::address_family: return darwin::eafnosupport;
    case Error::address_in_use: return darwin::eaddrinuse;
    case Error::address_not_available: return darwin::eaddrnotavail;
    case Error::network_down: return darwin::enetdown;
    case Error::network_unreachable: return darwin::enetunreach;
    case Error::network_reset: return darwin::enetreset;
    case Error::connection_aborted: return darwin::econnaborted;
    case Error::connection_reset: return darwin::econnreset;
    case Error::no_buffers: return darwin::enobufs;
    case Error::already_connected: return darwin::eisconn;
    case Error::not_connected: return darwin::enotconn;
    case Error::shut_down: return darwin::eshutdown;
    case Error::timed_out: return darwin::etimedout;
    case Error::connection_refused: return darwin::econnrefused;
    case Error::name_too_long: return darwin::enametoolong;
    case Error::host_down: return darwin::ehostdown;
    case Error::host_unreachable: return darwin::ehostunreach;
    case Error::not_empty: return darwin::enotempty;
    case Error::deadlock: return darwin::edeadlk;
    case Error::no_locks: return darwin::enolck;
    case Error::denied_by_policy: return darwin::enetunreach;
    case Error::other: return darwin::eio;
    }
    return darwin::eio;
}

// ---- the Darwin results of the replaced Win32 tables ----
// libSystem's file, descriptor and socket services reported host failures
// through three tables before the host layer existed. These keep their
// Darwin values, kind by kind, so guests see the same errno as before:
//
//   darwin_file_errno    path and handle calls (open, stat, flock, rename,
//                        attributes, pipes, ...): the former windows_error();
//                        kinds it did not list are EIO.
//   darwin_crt_errno     descriptor I/O that went through the C runtime
//                        (read, write, lseek, dup, fsync, chmod, access): the
//                        runtime's own Win32-to-errno table, then host_error();
//                        kinds the runtime does not list are EINVAL.
//   darwin_socket_errno  BSD socket calls: the former network_error(); kinds
//                        outside the socket error range are EIO.
//   darwin_spawn_errno   child process creation (process/launch.cpp); kinds
//                        it did not list are EIO.
[[nodiscard]] constexpr std::uint32_t darwin_file_errno(host::Error error) noexcept {
    using host::Error;
    switch (error) {
    case Error::none: return 0;
    case Error::not_found: return darwin::enoent;
    case Error::access_denied: return darwin::eacces;
    case Error::permission: return darwin::eperm;
    case Error::bad_handle: return darwin::ebadf;
    case Error::exists: return darwin::eexist;
    case Error::busy: return darwin::ebusy;               // a sharing violation
    case Error::no_space: return darwin::enospc;
    case Error::invalid_argument: return darwin::einval;
    case Error::not_empty: return darwin::enotempty;
    case Error::not_directory: return darwin::enotdir;
    case Error::unsupported: return darwin::enosys;       // ERROR_NOT_SUPPORTED was ENOSYS
    case Error::too_many_open: return darwin::emfile;
    case Error::no_memory: return darwin::enomem;
    case Error::read_only: return darwin::erofs;
    case Error::cross_device: return darwin::exdev;
    case Error::name_too_long: return darwin::enametoolong;
    default: return darwin::eio;
    }
}
[[nodiscard]] constexpr std::uint32_t darwin_crt_errno(host::Error error) noexcept {
    using host::Error;
    switch (error) {
    case Error::none: return 0;
    case Error::not_found: return darwin::enoent;
    case Error::name_too_long: return darwin::enoent;     // the runtime: ENOENT
    case Error::too_many_open: return darwin::emfile;
    case Error::access_denied: return darwin::eacces;
    case Error::busy: return darwin::eacces;              // sharing and lock violations
    case Error::read_only: return darwin::eacces;         // write protection
    case Error::bad_handle: return darwin::ebadf;
    case Error::no_memory: return darwin::enomem;
    case Error::exists: return darwin::eexist;
    case Error::broken_pipe: return darwin::epipe;
    case Error::no_space: return darwin::enospc;
    case Error::not_empty: return darwin::enotempty;
    case Error::cross_device: return darwin::eio;         // EXDEV was not passed on
    case Error::io: return darwin::eacces;                // device errors (not ready, CRC, read or write fault)
    default: return darwin::einval;
    }
}
// Child process creation (process/launch.cpp's former windows_errno()):
// kinds it did not list are EIO.
[[nodiscard]] constexpr std::uint32_t darwin_spawn_errno(host::Error error) noexcept {
    using host::Error;
    switch (error) {
    case Error::none: return 0;
    case Error::not_found: return darwin::enoent;
    case Error::access_denied: return darwin::eacces;
    case Error::no_memory: return darwin::enomem;
    case Error::too_many_open: return darwin::emfile;
    case Error::bad_handle: return darwin::ebadf;
    case Error::name_too_long: return darwin::enametoolong;
    case Error::not_directory: return darwin::enotdir;
    default: return darwin::eio;
    }
}
[[nodiscard]] constexpr std::uint32_t darwin_socket_errno(host::Error error) noexcept {
    using host::Error;
    switch (error) {
    case Error::none: case Error::interrupted: case Error::bad_handle: case Error::access_denied: case Error::fault:
    case Error::invalid_argument: case Error::too_many_open: case Error::would_block: case Error::in_progress:
    case Error::already: case Error::not_socket: case Error::destination_required: case Error::message_too_long:
    case Error::protocol_type: case Error::protocol_option: case Error::protocol_not_supported: case Error::socket_type:
    case Error::unsupported: case Error::protocol_family: case Error::address_family: case Error::address_in_use:
    case Error::address_not_available: case Error::network_down: case Error::network_unreachable: case Error::network_reset:
    case Error::connection_aborted: case Error::connection_reset: case Error::no_buffers: case Error::already_connected:
    case Error::not_connected: case Error::shut_down: case Error::timed_out: case Error::connection_refused: case Error::loop:
    case Error::name_too_long: case Error::host_down: case Error::host_unreachable:
        return darwin_errno(error);
    default: return darwin::eio;
    }
}

} // namespace alchemy::libsystem
