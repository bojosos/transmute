#include "alchemy/darwin_services.hpp"
#include <array>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
constexpr std::string_view library = "/usr/lib/libSystem.B.dylib";
void require(bool result, std::string_view message) { if (!result) throw std::runtime_error(std::string(message)); }
struct Fixture {
    transmute::ReferenceEngine engine{transmute::ExecutionStart{0}};
    alchemy::ProcessContext context{engine, [](unsigned, auto bytes) { return alchemy::OutputResult{bytes.size(), 0}; }};
    alchemy::DarwinServices services{context};
    transmute::GuestMemory& memory() { return engine; }
    std::uint64_t call(std::string_view name, std::initializer_list<std::uint64_t> args = {}) {
        const auto symbol = context.resolve(library, name); require(symbol.has_value(), "network export missing");
        return engine.invoke_callback(symbol->address, std::span(args.begin(), args.size()));
    }
    std::uint32_t error() { return engine.read32(context.current_thread()->errno_address); }
    std::uint64_t bytes(std::uint64_t size = 64) { const auto value = context.allocate_guest(size); engine.fill_bytes(value, 0, size); return value; }
    std::uint64_t string(std::string_view text) { const auto value = bytes(text.size() + 1); engine.write_bytes(value, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size())); return value; }
    std::string read_string(std::uint64_t address) { std::string value; for (unsigned index = 0; index < 1024; ++index) { const auto byte = memory().read8(address + index); if (!byte) return value; value += static_cast<char>(byte); } throw std::runtime_error("unterminated network string"); }
    std::uint64_t address(bool ipv6 = false) {
        const auto value = bytes(); memory().write8(value, ipv6 ? 28 : 16); memory().write8(value + 1, ipv6 ? 30 : 2);
        if (ipv6) memory().write8(value + 23, 1); else { memory().write8(value + 4, 127); memory().write8(value + 7, 1); }
        return value;
    }
};
struct TcpPair { std::uint64_t server{}, client{}, accepted{}, address{}; };
TcpPair pair(Fixture& f, bool ipv6 = false) {
    TcpPair value; value.address = f.address(ipv6); const auto length = f.bytes(); f.engine.write32(length, 64);
    value.server = f.call("_socket", {ipv6 ? 30ULL : 2ULL, 1, 0}); require(value.server != UINT64_MAX, "TCP socket creation failed");
    require(f.call("_bind", {value.server, value.address, ipv6 ? 28ULL : 16ULL}) == 0 && f.call("_listen", {value.server, 4}) == 0, "TCP listen failed");
    require(f.call("_getsockname", {value.server, value.address, length}) == 0, "TCP assigned address failed");
    value.client = f.call("_socket", {ipv6 ? 30ULL : 2ULL, 1, 0});
    require(value.client != UINT64_MAX && f.call("_connect", {value.client, value.address, ipv6 ? 28ULL : 16ULL}) == 0, "TCP connect failed");
    value.accepted = f.call("_accept", {value.server, 0, 0}); require(value.accepted != UINT64_MAX, "TCP accept failed"); return value;
}
void stream_and_descriptor_lifetime() {
    Fixture f; const auto connection = pair(f); const auto input = f.string("loopback"), output = f.bytes();
    require(f.call("_send", {connection.client, input, 8, 0}) == 8 && f.call("_recv", {connection.accepted, output, 8, 0x40}) == 8,
        "TCP transfer failed");
    require(f.engine.read64(output) == f.engine.read64(input), "TCP transfer changed bytes");
    const auto duplicate = f.call("_dup", {connection.client}); require(duplicate != UINT64_MAX, "socket dup failed");
    require(f.call("_close", {connection.client}) == 0 && f.call("_write", {duplicate, input, 8}) == 8 &&
        f.call("_read", {connection.accepted, output, 8}) == 8, "one alias close destroyed the socket");
    const auto info = f.bytes(144); require(f.call("_fstat", {duplicate, info}) == 0 && f.memory().read8(info + 5) == 0xc1, "socket stat type incorrect");
    const auto inode = f.engine.read64(info + 8), alias = f.call("_dup", {duplicate});
    require(f.call("_fstat", {alias, info}) == 0 && f.engine.read64(info + 8) == inode, "socket stat identity changed across dup");
    require(f.call("_lseek", {alias, 0, 0}) == UINT64_MAX && f.error() == 29, "socket seek did not fail ESPIPE");
    require(f.call("_recv", {alias, 1, 1, 0}) == UINT64_MAX && f.error() == 14, "socket bad guest buffer accepted");
    require(f.call("_send", {9999, input, 1, 0}) == UINT64_MAX && f.error() == 9, "invalid descriptor error incorrect");
    require(f.call("_send", {alias, input, 1, 0x1000000}) == UINT64_MAX && f.error() == 45, "unsupported send flag accepted");
    f.engine.write32((f.engine.state().sp - 256) & ~15ULL, 4);
    require(f.call("_fcntl", {alias, 4}) == 0 && f.call("_fcntl", {duplicate, 3}) == 6, "nonblocking status not shared by dup");
    require(f.call("_recv", {alias, output, 1, 0}) == UINT64_MAX && f.error() == 35, "empty nonblocking recv did not return EAGAIN");
    require(f.call("_shutdown", {duplicate, 1}) == 0 && f.call("_recv", {connection.accepted, output, 1, 0}) == 0, "write shutdown did not produce EOF");
    for (const auto descriptor : {alias, duplicate, connection.accepted, connection.server}) require(f.call("_close", {descriptor}) == 0, "socket cleanup failed");
}
void datagrams_and_readiness() {
    Fixture f; const auto address = f.address(), length = f.bytes(), source = f.bytes(), data = f.string("udp"), output = f.bytes();
    const auto server = f.call("_socket", {2, 2, 0}), client = f.call("_socket", {2, 2, 0});
    require(f.call("_bind", {server, address, 16}) == 0, "UDP bind failed"); f.engine.write32(length, 64);
    require(f.call("_getsockname", {server, address, length}) == 0, "UDP assigned address failed");
    const auto poll = f.bytes(16); f.engine.write32(poll, static_cast<std::uint32_t>(server)); f.memory().write8(poll + 4, 1);
    require(f.call("_poll", {poll, 1, 0}) == 0, "empty UDP reported readable");
    // POLLPRI (libcurl asks for POLLRDBAND|POLLPRI): there is no urgent data on Windows, so it is
    // never reported, and asking for it does not fail the poll.
    f.memory().write8(poll + 4, 1 | 2 | 0x80);
    require(f.call("_poll", {poll, 1, 0}) == 0 && f.memory().read8(poll + 6) == 0, "POLLPRI|POLLRDBAND poll of an idle socket failed");
    require(f.call("_sendto", {client, data, 3, 0, address, 16}) == 3 && f.call("_poll", {poll, 1, 1000}) == 1 &&
        (f.memory().read8(poll + 6) & 1) && !(f.memory().read8(poll + 6) & 2), "UDP poll readiness failed");
    f.memory().write8(poll + 4, 1);
    f.engine.write32(length, 2);
    require(f.call("_recvfrom", {server, output, 64, 0, source, length}) == 3 && f.engine.read32(length) == 16 && f.memory().read8(source + 1) == 2,
        "UDP receive/source address truncation contract failed");
    const auto mask = f.bytes(128), timeout = f.bytes(16); f.engine.write32(mask, 1U << server);
    require(f.call("_select$DARWIN_EXTSN", {server + 1, mask, 0, 0, timeout}) == 0 && f.engine.read32(mask) == 0, "select immediate timeout did not clear mask");
    require(f.call("_sendto", {client, data, 0, 0, address, 16}) == 0, "zero-length datagram send failed");
    f.engine.write32(mask, 1U << server); f.engine.write64(timeout, 1);
    require(f.call("_select$DARWIN_EXTSN", {server + 1, mask, 0, 0, timeout}) == 1 && f.call("_recv", {server, output, 64, 0}) == 0, "zero-length datagram lost or select failed");
    f.engine.write32(poll + 8, 99999); f.memory().write8(poll + 12, 1);
    require(f.call("_poll", {poll + 8, 1, 0}) == 1 && f.memory().read8(poll + 14) == 32, "poll invalid FD not POLLNVAL");
    require(f.call("_close", {server}) == 0 && f.call("_close", {client}) == 0, "UDP cleanup failed");
}
void close_cancels_and_reuse_is_safe() {
    Fixture f; const auto connection = pair(f); const auto output = f.bytes(), retained = f.call("_dup", {connection.accepted});
    auto worker = f.engine.fork_thread(0, 0); const auto symbol = f.context.resolve(library, "_recv"); require(symbol.has_value(), "recv export missing");
    std::promise<void> started;
    auto result = std::async(std::launch::async, [&] { const std::array<std::uint64_t, 4> arguments{connection.accepted, output, 1, 0}; started.set_value(); return worker->invoke_callback(symbol->address, arguments); });
    started.get_future().wait(); std::this_thread::sleep_for(std::chrono::milliseconds(25));
    require(result.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout, "blocking receive returned early");
    require(f.call("_close", {connection.accepted}) == 0, "closing waited socket deadlocked");
    const auto reused = f.call("_socket", {2, 2, 0}); require(reused == connection.accepted, "guest FD slot was not reusable");
    if (result.wait_for(std::chrono::seconds(2)) != std::future_status::ready) f.context.request_exit(1);
    require(result.get() == UINT64_MAX && worker->read32(f.context.thread(*worker)->errno_address) == 9, "closed recv resumed on reused descriptor");
    f.context.release_thread(*worker);
    require(f.call("_send", {connection.client, f.string("x"), 1, 0}) == 1 && f.call("_recv", {retained, output, 1, 0}) == 1, "surviving socket alias unusable after waiter cancellation");
    const auto poll = f.bytes(8); f.engine.write32(poll, static_cast<std::uint32_t>(retained)); f.memory().write8(poll + 4, 1);
    const auto poll_symbol = f.context.resolve(library, "_poll"); require(poll_symbol.has_value(), "poll export missing");
    std::promise<void> polling;
    auto ready = std::async(std::launch::async, [&] { const std::array<std::uint64_t, 3> args{poll, 1, UINT64_MAX}; polling.set_value(); return worker->invoke_callback(poll_symbol->address, args); });
    polling.get_future().wait(); std::this_thread::sleep_for(std::chrono::milliseconds(25));
    require(f.call("_close", {retained}) == 0, "closing poll socket deadlocked");
    const auto new_socket = f.call("_socket", {2, 1, 0});
    if (ready.wait_for(std::chrono::seconds(2)) != std::future_status::ready) f.context.request_exit(1);
    require(ready.get() == 1 && f.memory().read8(poll + 6) == 32, "poll did not retain closure identity across reuse");
    f.context.release_thread(*worker);
    for (const auto descriptor : {connection.server, connection.client, reused, new_socket}) require(f.call("_close", {descriptor}) == 0, "cancellation fixture cleanup failed");
}
void options_resolver_and_kernel_queue() {
    Fixture f; const auto connection = pair(f);
    const auto value = f.bytes(), length = f.bytes(), timeout = f.bytes(16), data = f.string("x");
    f.engine.write32(value, 1);
    require(f.call("_setsockopt", {connection.client, 0xffff, 8, value, 4}) == 0, "SO_KEEPALIVE set failed");
    f.engine.write32(length, 4); require(f.call("_getsockopt", {connection.client, 0xffff, 8, value, length}) == 0 && f.engine.read32(value) != 0,
        "SO_KEEPALIVE query failed");
    require(f.call("_setsockopt", {connection.client, 0xffff, 0x7000, value, 4}) == UINT64_MAX && f.error() == 42, "unknown option accepted");
    f.engine.write32(timeout + 8, 10000);
    require(f.call("_setsockopt", {connection.accepted, 0xffff, 0x1006, timeout, 16}) == 0, "SO_RCVTIMEO set failed");
    const auto begin = std::chrono::steady_clock::now();
    require(f.call("_recv", {connection.accepted, value, 1, 0}) == UINT64_MAX && f.error() == 35 &&
        std::chrono::steady_clock::now() - begin >= std::chrono::milliseconds(8), "managed receive timeout incorrect");
    f.engine.write32(timeout + 8, 1000000);
    require(f.call("_setsockopt", {connection.accepted, 0xffff, 0x1006, timeout, 16}) == UINT64_MAX && f.error() == 22, "invalid timeval accepted");
    const auto queue = f.call("_kqueue"), changes = f.bytes(32), events = f.bytes(64), zero = f.bytes(16);
    require(queue != UINT64_MAX, "kqueue creation failed");
    f.engine.write64(changes, connection.accepted); f.memory().write8(changes + 8, 0xff); f.memory().write8(changes + 9, 0xff);
    f.memory().write8(changes + 10, 1); f.engine.write64(changes + 24, 0xabcde);
    require(f.call("_kevent", {queue, changes, 1, 0, 0, 0}) == 0, "kqueue read registration failed");
    require(f.call("_kevent", {queue, 0, 0, events, 2, zero}) == 0, "empty kqueue reported readable");
    require(f.call("_send", {connection.client, data, 1, 0}) == 1 && f.call("_kevent", {queue, 0, 0, events, 2, zero}) == 1 &&
        f.engine.read64(events) == connection.accepted && f.engine.read64(events + 24) == 0xabcde && f.engine.read64(events + 16) == 1,
        "kqueue readiness/udata/available byte count failed");
    require(f.call("_recv", {connection.accepted, value, 1, 0}) == 1, "kqueue data drain failed");
    f.memory().write8(changes + 10, 1 | 0x40);
    require(f.call("_kevent", {queue, changes, 1, events, 2, zero}) == 1 && f.memory().read8(events + 11) == 0x40 && f.engine.read64(events + 16) == 0,
        "kqueue receipt result failed");
    f.memory().write8(changes + 10, 1 | 0x20);
    require(f.call("_kevent", {queue, changes, 1, 0, 0, 0}) == 0, "EV_CLEAR registration failed");
    require(f.call("_send", {connection.client, data, 1, 0}) == 1 && f.call("_kevent", {queue, 0, 0, events, 2, zero}) == 1 &&
        f.engine.read64(events + 16) == 1 && (f.memory().read8(events + 10) & 0x20), "EV_CLEAR edge not reported");
    require(f.call("_kevent", {queue, 0, 0, events, 2, zero}) == 0, "EV_CLEAR reported an unchanged readable state again");
    const auto queue_copy = f.call("_dup", {queue}); require(f.call("_close", {queue}) == 0, "queue close failed");
    require(f.call("_send", {connection.client, data, 1, 0}) == 1 && f.call("_kevent", {queue_copy, 0, 0, events, 2, zero}) == 1,
        "queue alias lost registration after close");
    require(f.call("_close", {connection.accepted}) == 0 && f.call("_kevent", {queue_copy, 0, 0, events, 2, zero}) == 1 &&
        f.memory().read8(events + 11) == 0x40 && f.engine.read64(events + 16) == 9, "monitored descriptor close did not wake kernel queue");
    for (const auto descriptor : {queue_copy, connection.client, connection.server}) require(f.call("_close", {descriptor}) == 0, "queue fixture cleanup failed");
    const auto hints = f.bytes(48), result = f.bytes(), name = f.bytes(64), service = f.bytes(32);
    f.engine.write32(hints, 4 | 0x1000); f.engine.write32(hints + 4, 2); f.engine.write32(hints + 8, 1);
    require(f.call("_getaddrinfo", {f.string("127.0.0.1"), f.string("81"), hints, result}) == 0, "numeric resolver failed");
    const auto head = f.engine.read64(result), address = f.engine.read64(head + 32);
    require(f.engine.read32(head + 4) == 2 && f.engine.read32(head + 16) == 16 && f.memory().read8(address + 7) == 1,
        "Darwin addrinfo layout incorrect");
    require(f.call("_getnameinfo", {address, 16, name, 64, service, 32, 2 | 8}) == 0 && f.memory().read8(service) == '8' && f.memory().read8(service + 1) == '1',
        "numeric nameinfo failed");
    require(f.call("_getnameinfo", {address, 16, name, 1, service, 32, 2 | 8}) == 14, "nameinfo overflow did not return EAI_OVERFLOW");
    require(f.call("_freeaddrinfo", {head}) == 0 && f.call("_freeaddrinfo", {head}) == UINT64_MAX && f.error() == 22,
        "resolver allocation ownership incorrect");
    require(f.call("_getaddrinfo", {f.string("not a numeric host"), f.string("81"), hints, result}) == 8 && f.engine.read64(result) == 0,
        "numeric host validation/error layout incorrect");
    require(f.call("_getaddrinfo", {f.string("127.0.0.1"), 0, hints, 1}) == 11 && f.error() == 14, "resolver bad result pointer contract incorrect");
}
void pairs_legacy_lookups_and_interfaces() {
    Fixture f; const auto pair_data = f.bytes(), input = f.string("pair"), output = f.bytes();
    require(f.call("_socketpair", {1, 1, 0, 1}) == UINT64_MAX && f.error() == 14, "socketpair unchecked descriptor output");
    for (const auto type : {1ULL, 2ULL}) {
        require(f.call("_socketpair", {1, type, 0, pair_data}) == 0, "anonymous socketpair failed");
        const auto left = static_cast<std::uint64_t>(f.engine.read32(pair_data)), right = static_cast<std::uint64_t>(f.engine.read32(pair_data + 4));
        require(f.call("_send", {left, input, 4, 0}) == 4 && f.call("_recv", {right, output, 4, 0}) == 4 && f.engine.read32(input) == f.engine.read32(output),
            "socketpair transfer failed");
        const auto name = f.bytes(), length = f.bytes(); f.engine.write32(length, 64);
        require(f.call("_getpeername", {left, name, length}) == 0 && f.engine.read32(length) == 2 && f.memory().read8(name + 1) == 1,
            "anonymous socketpair exposed host internet address");
        // sendmsg/recvmsg move msg_iov data; ancillary data is refused (EOPNOTSUPP).
        require(f.call("_sendmsg", {left, 0, 0}) == UINT64_MAX && f.error() == 14 &&
            f.call("_recvmsg", {right, 0, 0}) == UINT64_MAX && f.error() == 14, "NULL msghdr did not fail ssize_t/EFAULT");
        const auto vector = f.bytes(16), header = f.bytes(48), received = f.bytes(16), in_vector = f.bytes(16), in_header = f.bytes(48);
        f.engine.write64(vector, input); f.engine.write64(vector + 8, 4);
        f.engine.write64(header + 16, vector); f.engine.write32(header + 24, 1);
        f.engine.write64(in_vector, received); f.engine.write64(in_vector + 8, 16);
        f.engine.write64(in_header + 16, in_vector); f.engine.write32(in_header + 24, 1);
        require(f.call("_sendmsg", {left, header, 0}) == 4 && f.call("_recvmsg", {right, in_header, 0}) == 4 &&
            f.engine.read32(received) == f.engine.read32(input), "sendmsg/recvmsg data transfer failed");
        f.engine.write64(header + 32, f.bytes(16)); f.engine.write32(header + 40, 16);
        require(f.call("_sendmsg", {left, header, 0}) == UINT64_MAX && f.error() == 45, "ancillary data did not fail ssize_t/EOPNOTSUPP");
        require(f.call("_close", {left}) == 0 && f.call("_close", {right}) == 0, "socketpair cleanup failed");
    }
    require(f.call("_socketpair", {2, 1, 0, pair_data}) == UINT64_MAX && f.error() == 47, "socketpair wrong family accepted");
    const auto host = f.call("_gethostbyname", {f.string("localhost")}); require(host != 0, "legacy localhost lookup failed");
    require(f.engine.read32(host + 16) == 2 && f.engine.read32(host + 20) == 4 && f.engine.read64(host + 24) != 0,
        "Darwin hostent layout incorrect");
    const auto http = f.call("_getservbyname", {f.string("http"), f.string("tcp")}); require(http != 0, "service name lookup failed");
    require(f.engine.read32(http + 16) == 0x5000 && f.read_string(f.engine.read64(http + 24)) == "tcp", "Darwin servent layout incorrect");
    const auto service = f.call("_getservbyport", {0x5000, f.string("tcp")}); require(service && f.read_string(f.engine.read64(service)) == "http", "service port lookup failed");
    const auto protocol = f.call("_getprotobyname", {f.string("tcp")}); require(protocol != 0 && f.engine.read32(protocol + 16) == 6, "Darwin protoent layout incorrect");
    const auto interfaces = f.call("_if_nameindex"); require(interfaces != 0 && f.engine.read32(interfaces) != 0, "native interface enumeration failed");
    const auto index = f.engine.read32(interfaces); const auto name = f.engine.read64(interfaces + 8), copy = f.bytes(16);
    require(f.call("_if_nametoindex", {name}) == index && f.call("_if_indextoname", {index, copy}) == copy && f.read_string(name) == f.read_string(copy),
        "interface name/index roundtrip failed");
    require(f.call("_if_freenameindex", {interfaces}) == 0 && f.call("_if_freenameindex", {interfaces}) == UINT64_MAX && f.error() == 22,
        "interface result allocation ownership incorrect");
    require(f.call("_if_nametoindex", {f.string("missing-if")}) == 0 && f.error() == 6, "missing interface name error incorrect");
    require(f.call("_if_indextoname", {UINT32_MAX, copy}) == 0 && f.error() == 6, "missing interface index error incorrect");
    require(f.call("_inet_ntop", {2, 1, copy, 16}) == 0 && f.error() == 14, "inet source pointer unchecked");
    require(f.call("_inet_pton", {2, f.string("not-an-ip"), output}) == 0, "inet invalid string convention incorrect");
}
}
int main() {
    try { stream_and_descriptor_lifetime(); datagrams_and_readiness(); close_cancels_and_reuse_is_safe(); options_resolver_and_kernel_queue(); pairs_legacy_lookups_and_interfaces();
        Fixture ipv6; const auto connection = pair(ipv6, true);
        for (const auto descriptor : {connection.server, connection.client, connection.accepted}) require(ipv6.call("_close", {descriptor}) == 0, "IPv6 close failed");
        std::cout << "Alchemy networking passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
