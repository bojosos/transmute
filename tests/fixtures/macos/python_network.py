"""Local-only Darwin socket, file ownership, resolver, and readiness checks."""
import errno
import os
import select
import selectors
import socket
import threading
import time


def require(condition, message):
    if not condition:
        raise AssertionError(message)


addresses = socket.getaddrinfo("localhost", "0", socket.AF_UNSPEC, socket.SOCK_STREAM)
require(addresses and all(item[0] in (socket.AF_INET, socket.AF_INET6) for item in addresses), "localhost resolution")
require(socket.gethostname(), "hostname")
require(socket.gethostbyname("localhost") == "127.0.0.1", "legacy localhost lookup")
require(socket.gethostbyname_ex("localhost")[2], "legacy host record")
require(socket.getservbyname("http", "tcp") == 80 and socket.getservbyport(80, "tcp") == "http", "service lookup")
require(socket.getprotobyname("tcp") == socket.IPPROTO_TCP, "protocol lookup")
interfaces = socket.if_nameindex()
require(interfaces, "interface enumeration")
for index, name in interfaces:
    require(socket.if_nametoindex(name) == index and socket.if_indextoname(index) == name, "interface name/index roundtrip")
require(socket.getnameinfo(("127.0.0.1", 81), socket.NI_NUMERICHOST | socket.NI_NUMERICSERV) == ("127.0.0.1", "81"), "numeric nameinfo")
for family, address in ((socket.AF_INET, "127.0.0.1"), (socket.AF_INET6, "::1")):
    require(socket.inet_ntop(family, socket.inet_pton(family, address)) == address, "address conversion")

errors = []
with socket.socket() as listener:
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    require(listener.getsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR) != 0, "reuseaddr option")
    listener.bind(("127.0.0.1", 0))
    listener.listen(2)

    def serve():
        try:
            with listener.accept()[0] as connection:
                require(connection.getpeername()[0] == "127.0.0.1", "peer address")
                with connection.makefile("rwb") as stream:
                    require(stream.readline() == b"python socket file\n", "server file read")
                    stream.write(b"network reply\n")
                    stream.flush()
        except BaseException as error:
            errors.append(error)

    worker = threading.Thread(target=serve)
    worker.start()
    with socket.create_connection(listener.getsockname(), timeout=2.0) as connection:
        connection.settimeout(None)
        stream = connection.makefile("rwb")
        connection.close()
        # makefile retains the descriptor until its own close.
        stream.write(b"python socket file\n")
        stream.flush()
        require(stream.readline() == b"network reply\n", "client file read after socket close")
        stream.close()
    worker.join(3)
    require(not worker.is_alive() and not errors, "threaded TCP exchange")

with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as server, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
    server.bind(("127.0.0.1", 0))
    server.setblocking(False)
    try:
        server.recv(1)
        raise AssertionError("nonblocking receive unexpectedly succeeded")
    except BlockingIOError as error:
        require(error.errno in (errno.EAGAIN, errno.EWOULDBLOCK), "nonblocking Darwin errno")
    require(select.select([server], [], [], 0) == ([], [], []), "select immediate timeout")
    poller = select.poll() if hasattr(select, "poll") else None
    if poller is not None:
        poller.register(server, select.POLLIN)
        require(poller.poll(0) == [], "poll immediate timeout")
    client.sendto(b"udp payload", server.getsockname())
    if poller is not None:
        require(poller.poll(1000)[0][1] & select.POLLIN, "poll UDP readiness")
    require(select.select([server], [], [], 1)[0] == [server], "select UDP readiness")
    with selectors.DefaultSelector() as selected:
        selected.register(server, selectors.EVENT_READ, "udp")
        require(selected.select(1)[0][0].data == "udp", "DefaultSelector readiness and userdata")
        selected.modify(server, selectors.EVENT_READ | selectors.EVENT_WRITE, "updated")
        require(selected.select(1)[0][0].data == "updated", "DefaultSelector modification")
        selected.unregister(server)
    payload, source = server.recvfrom(64)
    require(payload == b"udp payload" and source[0] == "127.0.0.1", "UDP payload/source")
    server.settimeout(0.02)
    began = time.monotonic()
    try:
        server.recv(1)
        raise AssertionError("timed receive unexpectedly succeeded")
    except socket.timeout:
        require(time.monotonic() - began >= 0.015, "receive timeout returned too early")
    copied = socket.socket(fileno=os.dup(server.fileno()))
    server.close()
    copied.settimeout(1)
    client.sendto(b"duplicate", copied.getsockname())
    require(copied.recv(64) == b"duplicate", "duplicated socket lifetime")
    copied.close()

with socket.socket(socket.AF_INET6, socket.SOCK_DGRAM) as ipv6:
    ipv6.bind(("::1", 0))
    ipv6.settimeout(1)
    ipv6.sendto(b"ipv6", ipv6.getsockname())
    require(ipv6.recv(4) == b"ipv6", "IPv6 loopback")

for kind in (socket.SOCK_STREAM, socket.SOCK_DGRAM):
    left, right = socket.socketpair(type=kind)
    with left, right:
        left.sendall(b"pair")
        require(right.recv(4) == b"pair", "anonymous socketpair")
        duplicate = socket.socket(fileno=os.dup(left.fileno()))
        left.close()
        duplicate.sendall(b"duplicate")
        require(right.recv(64) == b"duplicate", "socketpair duplicate ownership")
        duplicate.close()

print("Alchemy Python networking passed")
