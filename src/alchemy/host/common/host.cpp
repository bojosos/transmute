#include "alchemy/host/host.hpp"

namespace alchemy::host {

std::string_view name(Error error) noexcept {
    switch (error) {
    case Error::none: return "none";
    case Error::unsupported: return "unsupported";
    case Error::permission: return "permission";
    case Error::not_found: return "not found";
    case Error::interrupted: return "interrupted";
    case Error::io: return "I/O error";
    case Error::bad_handle: return "bad handle";
    case Error::no_memory: return "no memory";
    case Error::access_denied: return "access denied";
    case Error::fault: return "fault";
    case Error::busy: return "busy";
    case Error::exists: return "exists";
    case Error::cross_device: return "cross-device";
    case Error::not_directory: return "not a directory";
    case Error::is_directory: return "is a directory";
    case Error::invalid_argument: return "invalid argument";
    case Error::too_many_open: return "too many open";
    case Error::no_space: return "no space";
    case Error::read_only: return "read-only";
    case Error::too_many_links: return "too many links";
    case Error::loop: return "too many symbolic links";
    case Error::broken_pipe: return "broken pipe";
    case Error::out_of_range: return "out of range";
    case Error::would_block: return "would block";
    case Error::in_progress: return "in progress";
    case Error::already: return "already in progress";
    case Error::not_socket: return "not a socket";
    case Error::message_too_long: return "message too long";
    case Error::destination_required: return "destination address required";
    case Error::protocol_type: return "wrong protocol type";
    case Error::protocol_option: return "protocol option not available";
    case Error::protocol_not_supported: return "protocol not supported";
    case Error::socket_type: return "socket type not supported";
    case Error::protocol_family: return "protocol family not supported";
    case Error::address_family: return "address family not supported";
    case Error::address_in_use: return "address in use";
    case Error::address_not_available: return "address not available";
    case Error::network_down: return "network down";
    case Error::network_unreachable: return "network unreachable";
    case Error::network_reset: return "network reset";
    case Error::connection_aborted: return "connection aborted";
    case Error::connection_reset: return "connection reset";
    case Error::no_buffers: return "no buffers";
    case Error::already_connected: return "already connected";
    case Error::not_connected: return "not connected";
    case Error::shut_down: return "shut down";
    case Error::timed_out: return "timed out";
    case Error::connection_refused: return "connection refused";
    case Error::name_too_long: return "name too long";
    case Error::host_down: return "host down";
    case Error::host_unreachable: return "host unreachable";
    case Error::not_empty: return "not empty";
    case Error::deadlock: return "deadlock";
    case Error::no_locks: return "no locks";
    case Error::denied_by_policy: return "denied by policy";
    case Error::other: return "other";
    }
    return "unknown";
}

} // namespace alchemy::host
