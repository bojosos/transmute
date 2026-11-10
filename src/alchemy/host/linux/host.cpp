// Linux host backend: handles and the errno mapping.
#include "alchemy/host/linux/linux.hpp"

#include <fcntl.h>
#include <unistd.h>

namespace alchemy::host {

std::string_view backend() noexcept { return "linux"; }

void close(Handle handle) noexcept {
    if (!handle) return;
    linux_backend::forget(linux_backend::fd_of(handle));
    ::close(linux_backend::fd_of(handle));
}

Result<Handle> duplicate(Handle handle) {
    const int fd = fcntl(linux_backend::fd_of(handle), F_DUPFD_CLOEXEC, 0);
    if (fd < 0) return linux_backend::last_error();
    return linux_backend::handle_of(fd);
}

namespace linux_backend {

Error error(int code) noexcept {
    switch (code) {
    case 0: return Error::none;
    case ENOSYS:
    case ENOTSUP: return Error::unsupported;
    case EPERM: return Error::permission;
    case ENOENT: return Error::not_found;
    case EINTR: return Error::interrupted;
    case EIO: return Error::io;
    case EBADF: return Error::bad_handle;
    case ENOMEM: return Error::no_memory;
    case EACCES: return Error::access_denied;
    case EFAULT: return Error::fault;
    case EBUSY:
    case ETXTBSY: return Error::busy;
    case EEXIST: return Error::exists;
    case EXDEV: return Error::cross_device;
    case ENOTDIR: return Error::not_directory;
    case EISDIR: return Error::is_directory;
    case EINVAL: return Error::invalid_argument;
    case EMFILE:
    case ENFILE: return Error::too_many_open;
    case ENOSPC:
    case EDQUOT: return Error::no_space;
    case EROFS: return Error::read_only;
    case EMLINK: return Error::too_many_links;
    case ELOOP: return Error::loop;
    case EPIPE: return Error::broken_pipe;
    case ERANGE:
    case EOVERFLOW: return Error::out_of_range;
    case EAGAIN: return Error::would_block;
    case EINPROGRESS: return Error::in_progress;
    case EALREADY: return Error::already;
    case ENOTSOCK: return Error::not_socket;
    case EMSGSIZE: return Error::message_too_long;
    case EDESTADDRREQ: return Error::destination_required;
    case EPROTOTYPE: return Error::protocol_type;
    case ENOPROTOOPT: return Error::protocol_option;
    case EPROTONOSUPPORT: return Error::protocol_not_supported;
    case ESOCKTNOSUPPORT: return Error::socket_type;
    case EPFNOSUPPORT: return Error::protocol_family;
    case EAFNOSUPPORT: return Error::address_family;
    case EADDRINUSE: return Error::address_in_use;
    case EADDRNOTAVAIL: return Error::address_not_available;
    case ENETDOWN: return Error::network_down;
    case ENETUNREACH: return Error::network_unreachable;
    case ENETRESET: return Error::network_reset;
    case ECONNABORTED: return Error::connection_aborted;
    case ECONNRESET: return Error::connection_reset;
    case ENOBUFS: return Error::no_buffers;
    case EISCONN: return Error::already_connected;
    case ENOTCONN: return Error::not_connected;
    case ESHUTDOWN: return Error::shut_down;
    case ETIMEDOUT: return Error::timed_out;
    case ECONNREFUSED: return Error::connection_refused;
    case ENAMETOOLONG: return Error::name_too_long;
    case EHOSTDOWN: return Error::host_down;
    case EHOSTUNREACH: return Error::host_unreachable;
    case ENOTEMPTY: return Error::not_empty;
    case EDEADLK: return Error::deadlock;
    case ENOLCK: return Error::no_locks;
    default: return Error::other;
    }
}

} // namespace linux_backend

} // namespace alchemy::host
