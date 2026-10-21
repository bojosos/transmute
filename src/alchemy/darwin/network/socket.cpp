#include "alchemy/darwin/network/network_internal.hpp"

#include "alchemy/darwin_threads.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>

// CFSocket (CoreFoundation's run-loop socket wrapper) over Alchemy's guest
// BSD sockets. It replaces the explicit failures foundation_shells::install_cfsocket_shell
// registers in alchemy.exe (in alchemy-fx that shell is empty).
//
// A CFSocket wraps a guest descriptor. Its run-loop source is an ordinary
// version-0 CFRunLoopSource made through the guest's CFRunLoopSourceCreate
// (so CFRunLoopAddSource / modes / invalidation are the run-loop provider's)
// whose perform callback is host code. One watcher on a guest dispatch
// worker polls the descriptors of every socket that has a source and enabled
// callbacks (guest poll, 50 ms slices), disables the callback types that
// became ready, records them as pending and signals the source; perform then
// runs the callouts on the run loop's thread and re-enables the types the
// socket flags ask for (kCFSocketAutomaticallyReenable*). The watcher ends
// when no socket needs watching and restarts on demand.
//
// Provided: CFSocketCreate, CFSocketCreateWithNative, CFSocketCreateRunLoopSource,
// Get/SetSocketFlags, Enable/DisableCallBacks, GetNative, GetContext,
// IsValid, Invalidate, SetAddress (bind + listen), CopyAddress,
// CopyPeerAddress, GetTypeID. Read, accept, data, connect and write
// callbacks. Not provided (unresolved, so a caller fails at bind time):
// CFSocketConnectToAddress, CFSocketSendData and socket signatures.
namespace alchemy::gui::network {
namespace {

using security::TypeId;

constexpr std::string_view core = core_foundation_library;
constexpr std::string_view libsystem = security::libsystem_library;

namespace callback {
inline constexpr std::uint64_t read = 1, accept = 2, data = 3, connect = 4, write = 8, read_kind = 3;
} // namespace callback
namespace flag {
inline constexpr std::uint64_t reenable_read_kind = 3, reenable_write = 8, close_on_invalidate = 128;
inline constexpr std::uint64_t defaults = reenable_read_kind | close_on_invalidate;
} // namespace flag
namespace poll_bits {
inline constexpr std::uint16_t in = 1, out = 4, err = 8, hup = 0x10, nval = 0x20;
} // namespace poll_bits

struct SocketState final : ObjectPayload {
    std::mutex mutex;
    Address object{};             // the CFSocket (not retained: the state lives in it)
    std::int32_t fd{-1};
    std::uint64_t types{};        // callback types given at creation
    std::uint64_t enabled{};      // enabled callback types
    std::uint64_t pending{};      // ready, waiting for perform
    std::uint64_t flags{flag::defaults};
    bool valid{true}, connected{};
    Address callout{};
    // CFSocketContext: version, info, retain, release, copyDescription.
    Address info{}, retain_info{}, release_info{}, describe{};
    Address source{};             // +1 CFRunLoopSource
};

class Sockets : public std::enable_shared_from_this<Sockets> {
public:
    explicit Sockets(std::shared_ptr<Bridge> bridge) : b(std::move(bridge)) {}
    std::shared_ptr<Bridge> b;
    Address socket_class{}, perform{}, cf_retain{}, cf_release{};

    std::mutex mutex;
    std::condition_variable changed;
    std::map<Address, std::shared_ptr<SocketState>> watched;   // sockets with a source
    bool watcher_running{};

    std::shared_ptr<SocketState> state(Address object) const {
        auto value = object ? b->objc.find_host_state<SocketState>(object) : nullptr;
        if (!value) security::fail("expected a CFSocket");
        return value;
    }

    std::int32_t guest_errno() const {
        const auto cell = b->call(libsystem, "___error", {});
        return cell ? static_cast<std::int32_t>(b->memory().read32(cell)) : 0;
    }

    // +1 CFSocket for `fd`.
    Address create(std::int32_t fd, std::uint64_t types, Address callout, Address context) {
        auto& m = b->memory();
        auto value = std::make_shared<SocketState>();
        value->fd = fd;
        value->types = value->enabled = types & (callback::read_kind | callback::connect | callback::write);
        value->callout = callout;
        if (context) {
            if (m.read64(context) != 0) security::fail("unsupported CFSocketContext version");
            value->info = m.read64(context + 8);
            value->retain_info = m.read64(context + 16);
            value->release_info = m.read64(context + 24);
            value->describe = m.read64(context + 32);
        }
        if (value->info && value->retain_info) value->info = b->call_address(value->retain_info, std::array{value->info});
        const auto object = b->objc.allocate(socket_class);
        value->object = object;
        static_cast<void>(b->objc.host_state<SocketState>(object, [&] { return value; }));
        return object;
    }

    void invalidate(Address object) {
        const auto value = state(object);
        Address source{}, info{}, release_info{};
        std::int32_t fd{-1};
        {
            std::lock_guard lock(value->mutex);
            if (!value->valid) return;
            value->valid = false;
            value->enabled = value->pending = 0;
            source = std::exchange(value->source, 0);
            info = std::exchange(value->info, 0);
            release_info = value->release_info;
            if (value->flags & flag::close_on_invalidate) fd = std::exchange(value->fd, -1);
        }
        {
            std::lock_guard lock(mutex);
            watched.erase(object);
        }
        changed.notify_all();
        if (source) {
            static_cast<void>(b->call(core, "_CFRunLoopSourceInvalidate", {source}));
            b->release(source);
        }
        if (fd >= 0) static_cast<void>(b->call(libsystem, "_close", {static_cast<Address>(fd)}));
        if (info && release_info) static_cast<void>(b->call_address(release_info, std::array{info}));
    }

    // Starts the watcher when a watched socket has work and none runs.
    void kick() {
        bool start{};
        {
            std::lock_guard lock(mutex);
            if (!watched.empty() && !watcher_running) { watcher_running = true; start = true; }
        }
        changed.notify_all();
        if (!start) return;
        const auto self = shared_from_this();
        const auto queue = b->call(libsystem, "_dispatch_get_global_queue", {0, 0});
        try {
            b->dispatch_async(queue, [self] { self->watch(); });
        } catch (...) {
            std::lock_guard lock(mutex);
            watcher_running = false;
            throw;
        }
    }

    // The watcher (guest dispatch worker).
    void watch() {
        struct Entry { std::shared_ptr<SocketState> state; std::int32_t fd; std::uint16_t events; };
        Address fds{};
        std::size_t capacity{};
        struct Scratch {
            Sockets& self; Address& fds;
            ~Scratch() { if (fds) self.b->process.release_guest(fds); }
        } scratch{*this, fds};
        for (;;) {
            if (guest_worker_must_stop(b->process)) throw GuestThreadStop{};
            std::vector<Entry> entries;
            {
                std::unique_lock lock(mutex);
                if (watched.empty()) { watcher_running = false; return; }
                for (const auto& [object, value] : watched) {
                    std::lock_guard guard(value->mutex);
                    if (!value->valid || value->fd < 0) continue;
                    std::uint16_t events{};
                    const auto wanted = value->enabled & ~value->pending;
                    if (wanted & callback::read_kind) events |= poll_bits::in;
                    if (wanted & (callback::write | callback::connect)) events |= poll_bits::out;
                    if (events) entries.push_back({value, value->fd, events});
                }
                if (entries.empty()) {
                    changed.wait_for(lock, std::chrono::milliseconds(50));
                    continue;
                }
            }
            auto& m = b->memory();
            if (entries.size() > capacity) {
                if (fds) b->process.release_guest(std::exchange(fds, 0));
                capacity = entries.size() * 2;
                fds = b->process.allocate_guest(capacity * 8);
            }
            for (std::size_t index = 0; index < entries.size(); ++index) {
                const auto at = fds + index * 8;
                m.write32(at, static_cast<std::uint32_t>(entries[index].fd));
                m.write32(at + 4, entries[index].events);   // events, revents = 0
            }
            const auto ready = static_cast<std::int32_t>(b->call(libsystem, "_poll", {fds, entries.size(), 50}));
            if (ready <= 0) continue;
            for (std::size_t index = 0; index < entries.size(); ++index) {
                const auto revents = static_cast<std::uint16_t>(m.read32(fds + index * 8 + 4) >> 16);
                if (!revents) continue;
                const auto& entry = entries[index];
                Address source{};
                {
                    std::lock_guard guard(entry.state->mutex);
                    if (!entry.state->valid || entry.state->fd != entry.fd) continue;
                    const auto wanted = entry.state->enabled & ~entry.state->pending;
                    std::uint64_t fired{};
                    // Errors and hang-ups wake readers and connectors (they learn the
                    // outcome from recv / SO_ERROR), as BSD poll reports them.
                    if (revents & (poll_bits::in | poll_bits::err | poll_bits::hup | poll_bits::nval)) fired |= wanted & callback::read_kind;
                    if (revents & (poll_bits::out | poll_bits::err | poll_bits::hup | poll_bits::nval)) fired |= wanted & (callback::write | callback::connect);
                    if (!fired) continue;
                    entry.state->pending |= fired;
                    entry.state->enabled &= ~fired;
                    source = entry.state->source;
                }
                if (source) static_cast<void>(b->call(core, "_CFRunLoopSourceSignal", {source}));
            }
        }
    }

    // The source's perform callback (run loop thread): deliver pending callbacks.
    void deliver(Address object) {
        const auto value = state(object);
        std::uint64_t pending{}, types{};
        Address callout{}, info{};
        std::int32_t fd{};
        {
            std::lock_guard lock(value->mutex);
            if (!value->valid) return;
            pending = std::exchange(value->pending, 0);
            types = value->types; callout = value->callout; info = value->info; fd = value->fd;
        }
        if (!pending) return;
        auto& m = b->memory();
        const auto call_out = [&](std::uint64_t type, Address address, Address data) {
            if (callout) static_cast<void>(b->call_address(callout, std::array{object, type, address, data, info}));
        };
        const auto still_valid = [&] { std::lock_guard lock(value->mutex); return value->valid; };

        // Connect first (CF delivers it before reads and writes).
        if (pending & callback::connect) {
            const auto cell = b->process.allocate_guest(8);
            m.write32(cell, 0); m.write32(cell + 4, 4);
            const auto status = static_cast<std::int32_t>(b->call(libsystem, "_getsockopt",
                {static_cast<Address>(fd), 0xffff, 0x1007, cell, cell + 4}));   // SOL_SOCKET, SO_ERROR
            const auto error = status == 0 ? static_cast<std::int32_t>(m.read32(cell)) : guest_errno();
            m.write32(cell, static_cast<std::uint32_t>(error));
            {
                std::lock_guard lock(value->mutex);
                value->connected = error == 0;
                value->types &= ~callback::connect;   // the connect callback fires once
            }
            // data: NULL on success, a pointer to the SInt32 error otherwise.
            try { call_out(callback::connect, 0, error ? cell : 0); } catch (...) { b->process.release_guest(cell); throw; }
            b->process.release_guest(cell);
            if (!still_valid()) return;
        }
        const auto read_kind = pending & callback::read_kind;
        if (read_kind == callback::read) {
            call_out(callback::read, 0, 0);
        } else if (read_kind == callback::accept) {
            const auto accepted = static_cast<std::int32_t>(b->call(libsystem, "_accept", {static_cast<Address>(fd), 0, 0}));
            if (accepted >= 0) {
                const auto cell = b->process.allocate_guest(8);
                m.write32(cell, static_cast<std::uint32_t>(accepted));
                const auto address = peer_address(accepted);
                try { call_out(callback::accept, address, cell); } catch (...) { b->release(address); b->process.release_guest(cell); throw; }
                b->release(address);
                b->process.release_guest(cell);
            }
        } else if (read_kind == callback::data) {
            constexpr std::size_t chunk = 65536;
            const auto buffer = b->process.allocate_guest(chunk);
            const auto received = static_cast<std::int64_t>(b->call(libsystem, "_recv", {static_cast<Address>(fd), buffer, chunk, 0}));
            Bytes bytes;
            if (received > 0) {
                bytes.resize(static_cast<std::size_t>(received));
                m.read_bytes(buffer, bytes);
            }
            b->process.release_guest(buffer);
            if (received >= 0) {
                // A zero-length CFData reports end of stream; the data callback is then left disabled.
                const auto data = b->data(bytes);
                const auto address = peer_address(fd);
                try { call_out(callback::data, address, data); } catch (...) { b->release(data); b->release(address); throw; }
                b->release(data); b->release(address);
                if (received == 0) {
                    std::lock_guard lock(value->mutex);
                    value->types &= ~callback::read_kind;
                }
            }
        }
        if (read_kind && !still_valid()) return;
        if (pending & callback::write) call_out(callback::write, 0, 0);

        // Re-enable per the socket flags (connect never; reads only while the type remains).
        {
            std::lock_guard lock(value->mutex);
            if (!value->valid) return;
            if (read_kind && (value->flags & flag::reenable_read_kind) && (value->types & callback::read_kind))
                value->enabled |= read_kind;
            if ((pending & callback::write) && (value->flags & flag::reenable_write)) value->enabled |= callback::write;
            if ((pending & callback::connect) && (value->types & callback::write) && !(pending & callback::write))
                value->enabled |= callback::write;
            static_cast<void>(types);
        }
        kick();
    }

    // +1 CFData sockaddr of the descriptor's local / peer address (0 when none).
    Address socket_address(std::int32_t fd, bool peer) {
        auto& m = b->memory();
        const auto buffer = b->process.allocate_guest(256 + 8);
        m.write32(buffer + 256, 256);
        const auto status = static_cast<std::int32_t>(b->call(libsystem, peer ? "_getpeername" : "_getsockname",
                                                              {static_cast<Address>(fd), buffer, buffer + 256}));
        Address result{};
        if (status == 0) {
            const auto length = std::min<std::uint32_t>(m.read32(buffer + 256), 256);
            Bytes bytes(length);
            m.read_bytes(buffer, bytes);
            result = b->data(bytes);
        }
        b->process.release_guest(buffer);
        return result;
    }
    Address peer_address(std::int32_t fd) { return socket_address(fd, true); }
};

} // namespace

void install_sockets(const std::shared_ptr<Bridge>& b) {
    // CFSocket is owned here in GUI builds: it replaces the explicit failures
    // the headless additions register (qt-vfb merge fix).
    const auto self = std::make_shared<Sockets>(b);
    static std::shared_ptr<Sockets> keep;
    keep = self;
    self->socket_class = b->define_class("_AlchemyCFSocket", b->object_class, core);
    b->register_type_id(self->socket_class, TypeId::socket);
    self->cf_retain = b->symbol(core, "_CFRetain");
    self->cf_release = b->symbol(core, "_CFRelease");
    self->perform = b->process.install_function("_alchemy_cfsocket_perform", [self](CpuState& s, GuestMemory&) {
        self->deliver(s.x[0]);
        s.x[0] = 0;
    }, "alchemy.cfsocket");

    b->method(self->socket_class, "dealloc", [self](CpuState& s, GuestMemory&) {
        if (const auto value = self->b->objc.find_host_state<SocketState>(s.x[0])) {
            bool valid;
            { std::lock_guard lock(value->mutex); valid = value->valid; }
            if (valid) self->invalidate(s.x[0]);
        }
        static_cast<void>(self->b->objc.send_super(s.x[0], self->socket_class, self->b->objc.selector("dealloc")));
    }, false, "v@:");

    b->function(core, "_CFSocketGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(TypeId::socket); });
    // CFSocketCreateWithNative(allocator, sock, callBackTypes, callout, context)
    b->function(core, "_CFSocketCreateWithNative", [self](CpuState& s, GuestMemory&) {
        const auto fd = static_cast<std::int32_t>(s.x[1]);
        if (fd < 0) { s.x[0] = 0; return; }
        s.x[0] = self->create(fd, s.x[2], s.x[3], s.x[4]);
    });
    // CFSocketCreate(allocator, protocolFamily, socketType, protocol, callBackTypes, callout, context)
    b->function(core, "_CFSocketCreate", [self](CpuState& s, GuestMemory&) {
        const auto family = static_cast<std::int32_t>(s.x[1]) > 0 ? s.x[1] : 2;   // PF_INET
        const auto type = static_cast<std::int32_t>(s.x[2]) > 0 ? s.x[2] : 1;     // SOCK_STREAM
        const auto protocol = static_cast<std::int32_t>(s.x[3]) > 0 ? s.x[3] : 0;
        const auto types = s.x[4], callout = s.x[5], context = s.x[6];
        const auto fd = static_cast<std::int32_t>(self->b->call(libsystem, "_socket", {family, type, protocol}));
        s.x[0] = fd < 0 ? 0 : self->create(fd, types, callout, context);
    });
    // CFSocketCreateRunLoopSource(allocator, socket, order): +1, one source per socket.
    b->function(core, "_CFSocketCreateRunLoopSource", [self](CpuState& s, GuestMemory&) {
        const auto object = s.x[1];
        const auto value = self->state(object);
        {
            std::lock_guard lock(value->mutex);
            if (!value->valid) { s.x[0] = 0; return; }
            if (value->source) { s.x[0] = self->b->retain(value->source); return; }
        }
        auto& m = self->b->memory();
        const auto context = self->b->process.allocate_guest(80);
        // CFRunLoopSourceContext: version, info, retain, release, copyDescription,
        // equal, hash, schedule, cancel, perform.
        const std::array<Address, 10> fields{0, object, self->cf_retain, self->cf_release, 0, 0, 0, 0, 0, self->perform};
        for (std::size_t index = 0; index < fields.size(); ++index) m.write64(context + index * 8, fields[index]);
        Address source{};
        try { source = self->b->call(core, "_CFRunLoopSourceCreate", {s.x[0], s.x[2], context}); }
        catch (...) { self->b->process.release_guest(context); throw; }
        self->b->process.release_guest(context);
        {
            std::lock_guard lock(value->mutex);
            value->source = source;   // the socket's reference
        }
        {
            std::lock_guard lock(self->mutex);
            self->watched[object] = value;
        }
        self->kick();
        s.x[0] = self->b->retain(source);
    });
    b->function(core, "_CFSocketGetSocketFlags", [self](CpuState& s, GuestMemory&) {
        const auto value = self->state(s.x[0]);
        std::lock_guard lock(value->mutex);
        s.x[0] = value->flags;
    });
    b->function(core, "_CFSocketSetSocketFlags", [self](CpuState& s, GuestMemory&) {
        const auto value = self->state(s.x[0]);
        std::lock_guard lock(value->mutex);
        value->flags = s.x[1];
    });
    b->function(core, "_CFSocketEnableCallBacks", [self](CpuState& s, GuestMemory&) {
        const auto value = self->state(s.x[0]);
        {
            std::lock_guard lock(value->mutex);
            if (!value->valid) return;
            auto types = s.x[1] & value->types;
            if ((s.x[1] & callback::connect) && value->connected) types &= ~callback::connect;
            value->enabled |= types;
        }
        self->kick();
    });
    b->function(core, "_CFSocketDisableCallBacks", [self](CpuState& s, GuestMemory&) {
        const auto value = self->state(s.x[0]);
        std::lock_guard lock(value->mutex);
        value->enabled &= ~s.x[1];
    });
    b->function(core, "_CFSocketGetNative", [self](CpuState& s, GuestMemory&) {
        const auto value = self->state(s.x[0]);
        std::lock_guard lock(value->mutex);
        s.x[0] = static_cast<Address>(static_cast<std::int64_t>(value->fd));
    });
    b->function(core, "_CFSocketIsValid", [self](CpuState& s, GuestMemory&) {
        const auto value = self->state(s.x[0]);
        std::lock_guard lock(value->mutex);
        s.x[0] = value->valid ? 1 : 0;
    });
    b->function(core, "_CFSocketInvalidate", [self](CpuState& s, GuestMemory&) {
        self->invalidate(s.x[0]);
        s.x[0] = 0;
    });
    b->function(core, "_CFSocketGetContext", [self](CpuState& s, GuestMemory& m) {
        const auto value = self->state(s.x[0]);
        if (!s.x[1] || m.read64(s.x[1]) != 0) security::fail("unsupported CFSocketContext version");
        std::lock_guard lock(value->mutex);
        m.write64(s.x[1] + 8, value->info); m.write64(s.x[1] + 16, value->retain_info);
        m.write64(s.x[1] + 24, value->release_info); m.write64(s.x[1] + 32, value->describe);
    });
    // CFSocketSetAddress(socket, CFData sockaddr): bind and listen (kCFSocketSuccess 0, kCFSocketError -1).
    b->function(core, "_CFSocketSetAddress", [self](CpuState& s, GuestMemory&) {
        const auto value = self->state(s.x[0]);
        std::int32_t fd;
        { std::lock_guard lock(value->mutex); fd = value->valid ? value->fd : -1; }
        if (fd < 0 || !s.x[1] || !self->b->is_data(s.x[1])) { s.x[0] = static_cast<Address>(-1); return; }
        const auto bytes = self->b->bytes(s.x[1]);
        const auto address = self->b->process.allocate_guest(bytes.size() + 1);
        self->b->memory().write_bytes(address, bytes);
        const auto bound = static_cast<std::int32_t>(self->b->call(libsystem, "_bind", {static_cast<Address>(fd), address, bytes.size()}));
        self->b->process.release_guest(address);
        if (bound != 0) { s.x[0] = static_cast<Address>(-1); return; }
        const auto listening = static_cast<std::int32_t>(self->b->call(libsystem, "_listen", {static_cast<Address>(fd), 256}));
        s.x[0] = listening == 0 ? 0 : static_cast<Address>(-1);
    });
    b->function(core, "_CFSocketCopyAddress", [self](CpuState& s, GuestMemory&) {
        const auto value = self->state(s.x[0]);
        std::int32_t fd;
        { std::lock_guard lock(value->mutex); fd = value->valid ? value->fd : -1; }
        s.x[0] = fd < 0 ? 0 : self->socket_address(fd, false);
    });
    b->function(core, "_CFSocketCopyPeerAddress", [self](CpuState& s, GuestMemory&) {
        const auto value = self->state(s.x[0]);
        std::int32_t fd;
        { std::lock_guard lock(value->mutex); fd = value->valid ? value->fd : -1; }
        s.x[0] = fd < 0 ? 0 : self->socket_address(fd, true);
    });
}

} // namespace alchemy::gui::network
