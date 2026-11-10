// Linux host backend, area "ipc": UNIX-domain stream sockets in the abstract
// namespace ("\0alchemy.<uid>.<name>": no file, gone with the last socket).
//
// The Windows contract is one pipe instance per client, where accept() turns
// the listener into the connection. Here each name has one listening socket
// per process, shared by its pending listeners; a pending listener is an
// eventfd (so cancel() can wake its accept), and accept() puts the accepted
// connection in its place with dup3, keeping the Handle's value. The
// listening socket closes with the last pending listener.
#include "alchemy/host/ipc.hpp"
#include "alchemy/host/linux/linux.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

namespace alchemy::host::ipc {

namespace {

using linux_backend::fd_of;
using linux_backend::handle_of;
using linux_backend::last_error;

struct Server {
    int socket{-1};
    int pending{};   // listeners not yet accepted or closed
};

std::mutex g_lock;
std::map<std::string, Server>& servers() {
    static auto* map = new std::map<std::string, Server>;
    return *map;
}
std::map<int, std::string>& pending_listeners() {   // eventfd -> name
    static auto* map = new std::map<int, std::string>;
    return *map;
}

socklen_t address_of(std::string_view name, sockaddr_un& address) {
    std::memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    const auto text = "alchemy." + std::to_string(getuid()) + "." + std::string(name);
    const auto length = std::min(text.size(), sizeof(address.sun_path) - 1);
    std::memcpy(address.sun_path + 1, text.data(), length);   // sun_path[0] = 0: the abstract namespace
    return static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + length);
}

// One pending listener fewer for `name` (g_lock held); the last closes the socket.
void release_listener(const std::string& name) {
    const auto it = servers().find(name);
    if (it == servers().end()) return;
    if (--it->second.pending <= 0) {
        ::close(it->second.socket);
        servers().erase(it);
    }
}

} // namespace

Result<Handle> listen(std::string_view name, bool first) {
    std::lock_guard guard(g_lock);
    const std::string key(name);
    auto it = servers().find(key);
    if (it != servers().end() && first) return Error::exists;
    if (it == servers().end()) {
        const int socket = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (socket < 0) return last_error();
        sockaddr_un address;
        const auto length = address_of(name, address);
        if (::bind(socket, reinterpret_cast<sockaddr*>(&address), length) != 0 || ::listen(socket, 64) != 0) {
            const int error = errno;
            ::close(socket);
            return error == EADDRINUSE ? Error::exists : linux_backend::error(error);
        }
        it = servers().emplace(key, Server{socket, 0}).first;
    }
    const int placeholder = eventfd(0, EFD_CLOEXEC);
    if (placeholder < 0) {
        const auto error = last_error();
        if (it->second.pending == 0) {
            ::close(it->second.socket);
            servers().erase(it);
        }
        return error;
    }
    ++it->second.pending;
    pending_listeners()[placeholder] = key;
    return handle_of(placeholder);
}

Status accept(Handle listener) {
    const int placeholder = fd_of(listener);
    int socket = -1;
    {
        std::lock_guard guard(g_lock);
        const auto it = pending_listeners().find(placeholder);
        if (it == pending_listeners().end()) return Error::invalid_argument;
        socket = servers().at(it->second).socket;
    }
    for (;;) {
        pollfd fds[2] = {{socket, POLLIN, 0}, {placeholder, POLLIN, 0}};
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            return last_error();
        }
        if (fds[1].revents) return Error::interrupted;   // cancelled
        const int connection = accept4(socket, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (connection < 0) {
            if (errno == EAGAIN || errno == EINTR || errno == ECONNABORTED) continue;   // another listener took it
            return last_error();
        }
        // Blocking I/O from here on, as on a pipe opened without PIPE_NOWAIT.
        const int flags = fcntl(connection, F_GETFL);
        fcntl(connection, F_SETFL, flags & ~O_NONBLOCK);
        std::lock_guard guard(g_lock);
        const auto it = pending_listeners().find(placeholder);
        const std::string name = it->second;
        pending_listeners().erase(it);
        const int replaced = dup3(connection, placeholder, O_CLOEXEC);
        ::close(connection);
        release_listener(name);
        if (replaced < 0) return last_error();
        return {};
    }
}

Result<Handle> connect(std::string_view name) {
    sockaddr_un address;
    const auto length = address_of(name, address);
    for (int attempt = 0; attempt < 20; ++attempt) {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return last_error();
        if (::connect(fd, reinterpret_cast<sockaddr*>(&address), length) == 0) return handle_of(fd);
        const int error = errno;
        ::close(fd);
        if (error == ECONNREFUSED || error == ENOENT) return Error::not_found;
        if (error != EAGAIN) return linux_backend::error(error);
        usleep(100000);   // the backlog is full: every listener is taken
    }
    return Error::busy;
}

Result<std::size_t> read(Handle channel, Bytes buffer) {
    for (;;) {
        const auto got = ::recv(fd_of(channel), buffer.data(), buffer.size(), 0);
        if (got >= 0) return static_cast<std::size_t>(got);
        if (errno == EINTR) continue;
        if (errno == ECONNRESET || errno == ENOTCONN) return std::size_t{0};
        return last_error();
    }
}

Result<std::size_t> write(Handle channel, ConstBytes data) {
    for (;;) {
        const auto put = ::send(fd_of(channel), data.data(), data.size(), MSG_NOSIGNAL);
        if (put >= 0) return static_cast<std::size_t>(put);
        if (errno == EINTR) continue;
        if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN) return std::size_t{0};
        return last_error();
    }
}

void cancel(Handle channel) noexcept {
    const int fd = fd_of(channel);
    {
        std::lock_guard guard(g_lock);
        if (pending_listeners().count(fd)) {   // wake a pending accept
            const std::uint64_t one = 1;
            static_cast<void>(::write(fd, &one, sizeof(one)));
            return;
        }
    }
    ::shutdown(fd, SHUT_RDWR);   // a connection: blocked reads and writes return
}

} // namespace alchemy::host::ipc

namespace alchemy::host::linux_backend {

void forget_listener(int fd) noexcept {
    std::lock_guard guard(ipc::g_lock);
    const auto it = ipc::pending_listeners().find(fd);
    if (it == ipc::pending_listeners().end()) return;
    const std::string name = it->second;
    ipc::pending_listeners().erase(it);
    ipc::release_listener(name);
}

} // namespace alchemy::host::linux_backend
