// Linux host backend: descriptors for other processes.
//
// Windows' DuplicateHandle puts a handle into another process; Linux can only
// pass a descriptor over a UNIX socket (SCM_RIGHTS), with the receiver taking
// part. So every process that links this backend runs a descriptor broker: a
// thread accepting on the abstract SOCK_SEQPACKET socket
// "alchemy-fd-broker.<uid>.<pid>", started when the process starts. A sender
// connects to the target's broker, sends one descriptor, and reads back the
// number the target's kernel gave it; that number is the duplicate's value in
// the target (process::duplicate_into, sections::duplicate_into,
// net::export_socket). The broker keeps nothing: the received descriptor is
// the target's, closed by whatever the macOS layer hands it to.
//
// Only peers of the same user are served (SO_PEERCRED), as only the same user
// may DuplicateHandle into a process on Windows.
#include "alchemy/host/linux/linux.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <thread>

namespace alchemy::host::linux_backend {

namespace {

sockaddr_un broker_address(std::uint32_t pid, socklen_t& length) {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const auto name = "alchemy-fd-broker." + std::to_string(getuid()) + "." + std::to_string(pid);
    // Abstract namespace: a leading NUL, no file.
    std::memcpy(address.sun_path + 1, name.data(), std::min(name.size(), sizeof(address.sun_path) - 2));
    length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());
    return address;
}

void serve(int listener) {
    for (;;) {
        const int connection = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
        if (connection < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            return;
        }
        ucred peer{};
        socklen_t peer_size = sizeof(peer);
        if (getsockopt(connection, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) != 0 || peer.uid != getuid()) {
            ::close(connection);
            continue;
        }
        char byte = 0;
        iovec part{&byte, 1};
        alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
        msghdr message{};
        message.msg_iov = &part;
        message.msg_iovlen = 1;
        message.msg_control = control;
        message.msg_controllen = sizeof(control);
        std::int32_t received = -1;
        if (recvmsg(connection, &message, MSG_CMSG_CLOEXEC) > 0)
            for (auto* header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header))
                if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS && header->cmsg_len >= CMSG_LEN(sizeof(int)))
                    std::memcpy(&received, CMSG_DATA(header), sizeof(int));
        static_cast<void>(::send(connection, &received, sizeof(received), MSG_NOSIGNAL));
        ::close(connection);
    }
}

// The broker, listening before main runs (a sender may name this process as
// soon as it exists). A process that cannot listen just receives nothing.
[[maybe_unused]] const bool g_broker_started = [] {
    const int listener = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (listener < 0) return false;
    socklen_t length = 0;
    const auto address = broker_address(static_cast<std::uint32_t>(getpid()), length);
    if (::bind(listener, reinterpret_cast<const sockaddr*>(&address), length) != 0 || ::listen(listener, 64) != 0) {
        ::close(listener);
        return false;
    }
    try {
        std::thread(serve, listener).detach();
    } catch (...) {
        ::close(listener);
        return false;
    }
    return true;
}();

} // namespace

Result<int> send_descriptor(int fd, std::uint32_t pid) {
    if (pid == 0 || pid == static_cast<std::uint32_t>(getpid())) {
        const int copy = fcntl(fd, F_DUPFD_CLOEXEC, 0);
        if (copy < 0) return last_error();
        return copy;
    }
    // A child started suspended has no broker yet: its gate carries it.
    if (auto gated = send_to_gated_child(fd, pid); gated || gated.error() != Error::not_found) return gated;
    const int connection = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (connection < 0) return last_error();
    struct Close { int fd; ~Close() { ::close(fd); } } closer{connection};
    socklen_t length = 0;
    const auto address = broker_address(pid, length);
    if (::connect(connection, reinterpret_cast<const sockaddr*>(&address), length) != 0)
        return errno == ECONNREFUSED || errno == ENOENT ? Error::not_found : last_error();
    char byte = 0;
    iovec part{&byte, 1};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
    msghdr message{};
    message.msg_iov = &part;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    auto* header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(header), &fd, sizeof(int));
    if (::sendmsg(connection, &message, MSG_NOSIGNAL) < 0) return last_error();
    // The number the target's kernel gave the descriptor (the broker answers at once).
    pollfd ready{connection, POLLIN, 0};
    int polled = 0;
    while ((polled = ::poll(&ready, 1, 10000)) < 0 && errno == EINTR) {
    }
    if (polled <= 0) return Error::timed_out;
    std::int32_t remote = -1;
    if (::recv(connection, &remote, sizeof(remote), 0) != static_cast<ssize_t>(sizeof(remote)) || remote < 0)
        return Error::io;
    return static_cast<int>(remote);
}

} // namespace alchemy::host::linux_backend
