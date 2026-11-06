#include "alchemy/darwin/libsystem/misc/xpc_internal.hpp"

#include "alchemy/darwin/launchd/plist.hpp"
#include "alchemy/path.hpp"

#include <bit>
#include <cstring>
#include <deque>
#include <filesystem>
#include <set>
#include <optional>
#include <stdexcept>

// XPC connections, endpoints, replies, xpc_main and transactions (<xpc/connection.h>), moved
// here from libsystem/misc/xpc_objects.cpp; the objects stay there, their internals are
// misc/xpc_internal.hpp. Both ends are this libxpc, so the wire format is Alchemy's own
// (nothing of Apple's is copied): one Mach message per XPC message, the object graph
// serialized inline, and port descriptors for what holds a right.
//
// * A client (xpc_connection_create_mach_service without the listener flag, or from an
//   endpoint) looks its service up at resume (across process trees: the session launchd keeps
//   jobs' MachServices, darwin/launchd/), makes an inbox and sends a hello carrying a send right
//   to it. The listener answers with a send right to the peer's own inbox; messages wait until
//   then. A dead peer is XPC_ERROR_CONNECTION_INTERRUPTED for a Mach-service client (the next
//   send connects again) and XPC_ERROR_CONNECTION_INVALID otherwise.
// * A listener (XPC_CONNECTION_MACH_SERVICE_LISTENER, an anonymous xpc_connection_create(NULL),
//   xpc_main's service) checks its name in or makes a port, and hands each hello to its handler
//   as a new peer connection; a peer's messages wait until the peer is resumed.
// * Replies travel on a send-once right; xpc_dictionary_create_reply's dictionary goes back on
//   it. The peer's pid, uid, gid and audit token come from the received messages' audit trailer.
// * Delivery is on guest dispatch queues (MACH_RECV sources, and MACH_SEND dead-name sources
//   for the peer), never on host threads.
// * Bundled XPC services (xpc_connection_create with a name): the .xpc bundle whose
//   CFBundleIdentifier is the name, in the main bundle's Contents/XPCServices or a framework's
//   XPCServices of the main bundle. One instance serves the application's process tree
//   (ServiceType Application): a client looks the name up in the tree, and when no instance has
//   checked it in, starts the service as its child (XPC_SERVICE_NAME = the name, plus the
//   XPCService EnvironmentVariables) and waits for its xpc_main to check in.
// Not here: the privileged flag (the system domain) is invalid; ServiceType User/System and
// JoinExistingSession are treated as Application; shared memory objects do not travel.
namespace alchemy::export_fill::xpc_detail {
namespace {

// <mach/message.h>
constexpr std::uint32_t move_receive = 16, move_send = 17, move_send_once = 18, copy_send = 19, make_send = 20, make_send_once = 21;
constexpr std::uint32_t send_msg = 1, rcv_msg = 2, rcv_large = 4, rcv_timeout = 0x100, trailer_audit = 3u << 24;
constexpr std::uint32_t complex_bit = 0x80000000u, port_descriptor = 0;
constexpr std::uint32_t rcv_timed_out = 0x10004003, rcv_too_large = 0x10004004;
constexpr std::uint32_t right_receive = 1;
// The connection flags of <xpc/connection.h>.
constexpr std::uint64_t mach_service_listener = 1, mach_service_privileged = 2;
// Alchemy's message ids.
constexpr std::int32_t id_message = 0x58504301, id_reply = 0x58504302, id_hello = 0x58504303, id_hello_ack = 0x58504304, id_goodbye = 0x58504305;

enum class Role { client, listener, peer };

struct Port { std::uint32_t name{}; std::uint32_t disposition{}; };

struct PendingReply {
    std::uint32_t port{};          // the receive right the reply comes to
    std::uint64_t source{}, block{}, queue{};
};

struct Conn {
    std::uint64_t object{};        // its OS_xpc_connection (not retained here)
    Role role{Role::client};
    bool mach_service{}, interrupts{};   // a Mach-service client: a dead peer interrupts, it does not invalidate
    bool bundled{};                // a bundled XPC service of this application (the name is its bundle identifier)
    std::uint32_t service{};       // client: a send right to the listener; listener: its receive right
    std::uint32_t inbox{};         // client and peer: this end's receive right
    std::uint32_t peer{};          // client and peer: a send right to the other end's inbox
    bool connecting{}, connected{}, dead{}, finished{};
    bool held{};                   // a peer: kept alive by the listener until it ends, as libxpc does
    std::uint64_t queue{};         // the serial queue events are delivered on (owned)
    std::uint64_t service_source{}, inbox_source{}, dead_source{};
    std::deque<std::uint64_t> outgoing;      // retained messages waiting for the connection
    std::deque<std::uint64_t> incoming;      // retained events waiting for the handler (resume)
    std::vector<PendingReply> replies;
    std::array<std::uint32_t, 8> audit{};
};

struct Received {
    std::uint32_t reply{};         // the sender's send-once reply right (0: none)
    std::uint64_t connection{};    // the connection it arrived on (not retained)
};

struct Channel {
    std::uint64_t connection{};
    enum Kind { service, inbox, dead, reply } kind{};
    std::uint32_t port{};
};

class Connections {
public:
    Connections(Provider& provider, std::shared_ptr<Xpc> xpc) : p(provider), xpc_(std::move(xpc)) {}

    Provider p;
    std::shared_ptr<Xpc> xpc_;
    std::mutex mutex;
    std::map<std::uint64_t, std::shared_ptr<Conn>> conns;
    std::map<std::uint64_t, Channel> channels;
    std::map<std::uint64_t, Received> received;       // dictionary -> where it came from
    std::map<std::uint64_t, std::uint32_t> reply_to;  // reply dictionary -> the send-once right it answers
    std::uint64_t next_channel{1};
    std::uint64_t event_entry{}, connect_entry{};

    ProcessContext& process() { return p.process(); }
    GuestMemory& memory() { return process().calling_engine(); }
    std::uint64_t sys(const char* name, std::initializer_list<std::uint64_t> arguments) { return p.call(libsystem_library, name, arguments); }
    std::uint64_t symbol(const char* name) {
        const auto found = process().resolve(libsystem_library, name);
        return found ? found->address : 0;
    }
    // The cross-process Mach layer (alchemy-fx's libsystem/mach/): without it no connection is made.
    bool mach() { return symbol("_mach_task_self_") && symbol("_bootstrap_port") && symbol("_mach_msg") && symbol("__dispatch_source_type_mach_recv"); }
    std::uint32_t task() { const auto at = symbol("_mach_task_self_"); return at ? memory().read32(at) : 0; }
    std::uint32_t bootstrap() { const auto at = symbol("_bootstrap_port"); return at ? memory().read32(at) : 0; }

    std::shared_ptr<Conn> conn(std::uint64_t object) {
        if (!xpc_->typed(object, Type::connection)) throw std::runtime_error("Alchemy XPC: a connection function on an object that is not an XPC connection");
        std::lock_guard lock(mutex);
        auto& found = conns[object];
        if (!found) { found = std::make_shared<Conn>(); found->object = object; }
        return found;
    }
    std::shared_ptr<Value> value(std::uint64_t object) { return xpc_->typed(object, Type::connection); }

    // ---- ports ----------------------------------------------------------------------------
    std::uint32_t new_receive() {
        if (!mach()) return 0;
        GuestCell cell(*this);
        if (sys("_mach_port_allocate", {task(), right_receive, cell.address}) != 0) return 0;
        return memory().read32(cell.address);
    }
    void drop_receive(std::uint32_t name) { if (name) static_cast<void>(sys("_mach_port_mod_refs", {task(), name, right_receive, static_cast<std::uint64_t>(-1)})); }
    void drop_send(std::uint32_t name) { if (name) static_cast<void>(sys("_mach_port_deallocate", {task(), name})); }

    struct GuestCell {
        explicit GuestCell(Connections& owner, std::size_t size = 8) : self(owner), address(owner.process().allocate_guest(size)) {
            owner.memory().write64(address, 0);
        }
        ~GuestCell() { self.process().release_guest(address); }
        Connections& self;
        std::uint64_t address;
    };

    // A dispatch source on `port` for connection `object`, its events to on_event.
    std::uint64_t source(std::uint64_t object, Channel::Kind kind, std::uint32_t port, std::uint64_t queue) {
        const auto type = symbol(kind == Channel::dead ? "__dispatch_source_type_mach_send" : "__dispatch_source_type_mach_recv");
        const auto made = sys("_dispatch_source_create", {type, port, kind == Channel::dead ? 1u : 0u, queue});
        if (!made) return 0;
        std::uint64_t id;
        { std::lock_guard lock(mutex); id = next_channel++; channels[id] = Channel{object, kind, port}; }
        static_cast<void>(sys("_dispatch_set_context", {made, id}));
        static_cast<void>(sys("_dispatch_source_set_event_handler_f", {made, event_entry}));
        static_cast<void>(sys("_dispatch_resume", {made}));
        return made;
    }
    void cancel_source(std::uint64_t& source) {
        if (!source) return;
        static_cast<void>(sys("_dispatch_source_cancel", {source}));
        static_cast<void>(sys("_dispatch_release", {source}));
        source = 0;
    }

    // ---- the wire format -------------------------------------------------------------------
    void put32(std::vector<std::uint8_t>& out, std::uint32_t v) { const auto* b = reinterpret_cast<const std::uint8_t*>(&v); out.insert(out.end(), b, b + 4); }
    void put64(std::vector<std::uint8_t>& out, std::uint64_t v) { const auto* b = reinterpret_cast<const std::uint8_t*>(&v); out.insert(out.end(), b, b + 8); }
    void put_text(std::vector<std::uint8_t>& out, const std::string& text) { put32(out, static_cast<std::uint32_t>(text.size())); out.insert(out.end(), text.begin(), text.end()); }

    // Serializes `object` (a message: a dictionary at the top).
    void encode(std::uint64_t object, std::vector<std::uint8_t>& out, std::vector<Port>& ports, int depth = 0) {
        const auto found = xpc_->value(object);
        if (!found || depth > 64) { out.push_back('N'); return; }
        std::unique_lock lock(found->mutex);
        switch (found->type) {
        case Type::dictionary: {
            const auto entries = found->entries;
            lock.unlock();
            out.push_back('d');
            put32(out, static_cast<std::uint32_t>(entries.size()));
            for (const auto& [key, item] : entries) { put_text(out, key); encode(item, out, ports, depth + 1); }
            return;
        }
        case Type::array: {
            const auto items = found->items;
            lock.unlock();
            out.push_back('a');
            put32(out, static_cast<std::uint32_t>(items.size()));
            for (const auto item : items) encode(item, out, ports, depth + 1);
            return;
        }
        case Type::string: out.push_back('s'); put_text(out, found->text); return;
        case Type::int64: out.push_back('i'); put64(out, static_cast<std::uint64_t>(found->integer)); return;
        case Type::uint64: out.push_back('u'); put64(out, static_cast<std::uint64_t>(found->integer)); return;
        case Type::boolean: out.push_back('b'); out.push_back(found->integer ? 1 : 0); return;
        case Type::double_value: out.push_back('f'); put64(out, std::bit_cast<std::uint64_t>(found->real)); return;
        case Type::date: out.push_back('t'); put64(out, static_cast<std::uint64_t>(found->integer)); return;
        case Type::data: out.push_back('D'); put_text(out, std::string(found->bytes.begin(), found->bytes.end())); return;
        case Type::uuid: out.push_back('U'); out.insert(out.end(), found->bytes.begin(), found->bytes.end()); out.resize(out.size() + (16 - std::min<std::size_t>(16, found->bytes.size()))); return;
        case Type::null: out.push_back('N'); return;
        case Type::fd: {
            const auto fd = found->integer;
            lock.unlock();
            GuestCell cell(*this);
            if (fd < 0 || sys("_fileport_makeport", {static_cast<std::uint64_t>(fd), cell.address}) != 0) { out.push_back('N'); return; }
            out.push_back('F');
            put32(out, static_cast<std::uint32_t>(ports.size()));
            ports.push_back(Port{memory().read32(cell.address), move_send});
            return;
        }
        case Type::mach_send:
            out.push_back('P');
            put32(out, static_cast<std::uint32_t>(ports.size()));
            ports.push_back(Port{static_cast<std::uint32_t>(found->integer), copy_send});
            return;
        case Type::endpoint: {
            const auto target = found->connection;
            lock.unlock();
            const auto c = conn(target);
            std::uint32_t name{}, disposition{};
            {
                std::lock_guard held(mutex);
                if (c->role == Role::listener && c->service) { name = c->service; disposition = make_send; }
                else if (c->role == Role::client && c->service) { name = c->service; disposition = copy_send; }
            }
            if (!name) { out.push_back('N'); return; }
            out.push_back('e');
            put32(out, static_cast<std::uint32_t>(ports.size()));
            ports.push_back(Port{name, disposition});
            return;
        }
        default:
            out.push_back('N');   // errors, connections, shared memory: they do not travel
            return;
        }
    }

    struct Cursor {
        const std::vector<std::uint8_t>& bytes;
        std::size_t at{};
        bool ok{true};
        std::uint8_t u8() { if (at + 1 > bytes.size()) { ok = false; return 0; } return bytes[at++]; }
        std::uint32_t u32() { std::uint32_t v{}; if (at + 4 > bytes.size()) { ok = false; return 0; } std::memcpy(&v, bytes.data() + at, 4); at += 4; return v; }
        std::uint64_t u64() { std::uint64_t v{}; if (at + 8 > bytes.size()) { ok = false; return 0; } std::memcpy(&v, bytes.data() + at, 8); at += 8; return v; }
        std::string text() { const auto n = u32(); if (!ok || at + n > bytes.size()) { ok = false; return {}; } std::string t(bytes.begin() + at, bytes.begin() + at + n); at += n; return t; }
    };

    // A +1 object from the wire; `ports` are the received rights, each taken at most once.
    std::uint64_t decode(Cursor& in, std::vector<Port>& ports, int depth = 0) {
        if (depth > 64) { in.ok = false; return 0; }
        const auto take = [&](std::uint32_t index) -> std::uint32_t {
            if (index >= ports.size()) return 0;
            return std::exchange(ports[index].name, 0);
        };
        switch (in.u8()) {
        case 'd': {
            const auto dictionary = xpc_->make(Type::dictionary);
            const auto n = in.u32();
            for (std::uint32_t i = 0; i < n && in.ok; ++i) {
                const auto key = in.text();
                const auto item = decode(in, ports, depth + 1);
                if (item) xpc_->set(dictionary, key, item, true);
            }
            return dictionary;
        }
        case 'a': {
            const auto array = xpc_->make(Type::array);
            const auto n = in.u32();
            for (std::uint32_t i = 0; i < n && in.ok; ++i) {
                const auto item = decode(in, ports, depth + 1);
                if (item) xpc_->put(array, UINT64_MAX, item, true);
            }
            return array;
        }
        case 's': { const auto text = in.text(); return xpc_->string_object(text); }
        case 'i': { const auto v = in.u64(); return xpc_->make(Type::int64, [&](Value& x) { x.integer = static_cast<std::int64_t>(v); }); }
        case 'u': { const auto v = in.u64(); return xpc_->make(Type::uint64, [&](Value& x) { x.integer = static_cast<std::int64_t>(v); }); }
        case 'b': return xpc_->retain(in.u8() ? xpc_->bool_true : xpc_->bool_false);
        case 'f': { const auto v = in.u64(); return xpc_->make(Type::double_value, [&](Value& x) { x.real = std::bit_cast<double>(v); }); }
        case 't': { const auto v = in.u64(); return xpc_->make(Type::date, [&](Value& x) { x.integer = static_cast<std::int64_t>(v); }); }
        case 'D': { const auto t = in.text(); return xpc_->make(Type::data, [&](Value& x) { x.bytes.assign(t.begin(), t.end()); }); }
        case 'U': {
            std::vector<std::uint8_t> uuid(16);
            for (auto& byte : uuid) byte = in.u8();
            return xpc_->make(Type::uuid, [&](Value& x) { x.bytes = uuid; });
        }
        case 'N': return xpc_->make(Type::null);
        case 'F': {
            const auto port = take(in.u32());
            const auto fd = port ? static_cast<std::int32_t>(sys("_fileport_makefd", {port})) : -1;
            drop_send(port);
            if (fd < 0) return xpc_->make(Type::null);
            return xpc_->make(Type::fd, [&](Value& x) { x.integer = fd; });
        }
        case 'P': {
            const auto port = take(in.u32());
            return xpc_->make(Type::mach_send, [&](Value& x) { x.integer = port; });
        }
        case 'e': {
            const auto port = take(in.u32());
            if (!port) return xpc_->make(Type::null);
            // The endpoint names a listener: a client connection holds the right.
            const auto target = make_connection("endpoint", 0);
            { const auto c = conn(target); std::lock_guard lock(mutex); c->service = port; }
            return xpc_->make(Type::endpoint, [&](Value& x) { x.connection = target; });
        }
        default:
            in.ok = false;
            return 0;
        }
    }

    // ---- messages ---------------------------------------------------------------------------
    // Sends `body` (serialized, with `ports`) to `destination`. False when the peer is gone.
    bool send(std::uint32_t destination, std::uint32_t destination_disposition, std::uint32_t local, std::uint32_t local_disposition,
              std::int32_t id, const std::vector<std::uint8_t>& body, const std::vector<Port>& ports) {
        const auto padded = (body.size() + 3) & ~std::size_t{3};
        const auto size = 24 + (ports.empty() ? 0 : 4 + 12 * ports.size()) + 4 + padded;
        const auto message = process().allocate_guest(size);
        std::vector<std::uint8_t> image(size, 0);
        const auto at32 = [&](std::size_t offset, std::uint32_t v) { std::memcpy(image.data() + offset, &v, 4); };
        at32(0, destination_disposition | (local_disposition << 8) | (ports.empty() ? 0 : complex_bit));
        at32(4, static_cast<std::uint32_t>(size));
        at32(8, destination);
        at32(12, local);
        at32(20, static_cast<std::uint32_t>(id));
        std::size_t offset = 24;
        if (!ports.empty()) {
            at32(offset, static_cast<std::uint32_t>(ports.size()));
            offset += 4;
            for (const auto& port : ports) {
                at32(offset, port.name);
                image[offset + 10] = static_cast<std::uint8_t>(port.disposition);
                image[offset + 11] = static_cast<std::uint8_t>(port_descriptor);
                offset += 12;
            }
        }
        at32(offset, static_cast<std::uint32_t>(body.size()));
        if (!body.empty()) std::memcpy(image.data() + offset + 4, body.data(), body.size());
        memory().write_bytes(message, image);
        const auto status = static_cast<std::uint32_t>(sys("_mach_msg", {message, send_msg, size, 0, 0, 0, 0}));
        process().release_guest(message);
        return status == 0;
    }

    struct Incoming {
        std::int32_t id{};
        std::uint32_t remote{}, remote_type{};   // the reply right the sender gave (received type)
        std::vector<std::uint8_t> body;
        std::vector<Port> ports;
        std::array<std::uint32_t, 8> audit{};
    };

    // Receives one message from `port` without waiting (or up to `timeout_ms`).
    std::optional<Incoming> receive(std::uint32_t port, std::uint32_t timeout_ms = 0) {
        std::size_t capacity = 64 * 1024;
        for (int attempt = 0; attempt < 4; ++attempt) {
            const auto buffer = process().allocate_guest(capacity + 64);
            const auto status = static_cast<std::uint32_t>(sys("_mach_msg", {buffer, rcv_msg | rcv_timeout | rcv_large | trailer_audit, 0, capacity, port, timeout_ms, 0}));
            if (status == rcv_too_large) {
                capacity = std::max<std::size_t>(capacity * 2, memory().read32(buffer + 4) + 64);
                process().release_guest(buffer);
                continue;
            }
            if (status != 0) { process().release_guest(buffer); return std::nullopt; }
            const auto size = memory().read32(buffer + 4);
            std::vector<std::uint8_t> image(size + 52);
            memory().read_bytes(buffer, std::span(image));
            process().release_guest(buffer);
            Incoming in;
            std::uint32_t bits{};
            std::memcpy(&bits, image.data(), 4);
            std::memcpy(&in.remote, image.data() + 8, 4);
            std::memcpy(&in.id, image.data() + 20, 4);
            in.remote_type = bits & 0x1f;
            std::size_t offset = 24;
            if (bits & complex_bit) {
                std::uint32_t count{};
                std::memcpy(&count, image.data() + offset, 4);
                offset += 4;
                for (std::uint32_t i = 0; i < count && offset + 12 <= size; ++i, offset += 12) {
                    Port port;
                    std::memcpy(&port.name, image.data() + offset, 4);
                    port.disposition = image[offset + 10];
                    in.ports.push_back(port);
                }
            }
            if (offset + 4 <= size) {
                std::uint32_t length{};
                std::memcpy(&length, image.data() + offset, 4);
                if (offset + 4 + length <= size) in.body.assign(image.begin() + offset + 4, image.begin() + offset + 4 + length);
            }
            const auto trailer = (size + 3) & ~std::uint32_t{3};
            if (trailer + 52 <= image.size()) std::memcpy(in.audit.data(), image.data() + trailer + 20, 32);
            return in;
        }
        return std::nullopt;
    }

    void release_ports(std::vector<Port>& ports) {
        for (auto& port : ports) { if (port.name) drop_send(port.name); port.name = 0; }
    }

    // ---- connection life -----------------------------------------------------------------------
    std::uint64_t make_connection(const std::string& name, std::uint64_t queue) {
        if (queue) static_cast<void>(sys("_dispatch_retain", {queue}));
        const auto object = xpc_->make(Type::connection, [&](Value& v) { v.text = name; v.queue = queue; });
        static_cast<void>(conn(object));
        return object;
    }

    // The serial queue a connection's events run on.
    std::uint64_t queue_of(const std::shared_ptr<Conn>& c) {
        const auto v = value(c->object);
        std::uint64_t target;
        { std::lock_guard lock(v->mutex); target = v->queue; }
        std::lock_guard lock(mutex);
        if (!c->queue) {
            const auto label = process().allocate_guest(32);
            put_string(memory(), label, "com.apple.xpc.connection");
            c->queue = sys("_dispatch_queue_create", {label, 0});
            if (target) static_cast<void>(sys("_dispatch_set_target_queue", {c->queue, target}));
        }
        return c->queue;
    }

    // Calls the connection's handler with `event` (on its queue: we are there), or keeps the
    // event until the connection is resumed.
    void handle(const std::shared_ptr<Conn>& c, std::uint64_t event) {
        const auto v = value(c->object);
        std::uint64_t handler;
        bool resumed;
        { std::lock_guard lock(v->mutex); handler = v->handler; resumed = v->resumed; }
        if (!resumed) { std::lock_guard lock(mutex); c->incoming.push_back(xpc_->retain(event)); return; }
        if (!handler) return;
        static_cast<void>(EngineAdapter(process().calling_engine()).call(memory().read64(handler + 16), std::array<std::uint64_t, 2>{handler, event}));
    }

    // `event` to the handler on the connection's queue (asynchronously), or kept until resume.
    void post(const std::shared_ptr<Conn>& c, std::uint64_t event) {
        const auto v = value(c->object);
        std::uint64_t handler;
        bool resumed;
        { std::lock_guard lock(v->mutex); handler = v->handler; resumed = v->resumed; }
        if (!resumed) { std::lock_guard lock(mutex); c->incoming.push_back(xpc_->retain(event)); return; }
        if (handler) xpc_->deliver(queue_of(c), handler, event);
    }

    // The error a connection that cannot send reports: invalid once it has ended (or never
    // had a service), interrupted for a Mach-service client whose peer went away.
    std::uint64_t failure(const std::shared_ptr<Conn>& c) {
        std::lock_guard lock(mutex);
        return c->finished || !c->interrupts ? xpc_->connection_invalid : xpc_->connection_interrupted;
    }

    // The peer is gone or the connection ended: pending replies get the error, then the handler.
    void fail(const std::shared_ptr<Conn>& c, bool interrupted) {
        std::vector<PendingReply> replies;
        {
            std::lock_guard lock(mutex);
            if (c->finished && !interrupted) return;
            replies.swap(c->replies);
            c->connected = c->connecting = false;
            if (!interrupted) c->finished = true;
        }
        const auto error = interrupted ? xpc_->connection_interrupted : xpc_->connection_invalid;
        for (auto& reply : replies) {
            cancel_source(reply.source);
            drop_receive(reply.port);
            xpc_->deliver(reply.queue, reply.block, error);
            static_cast<void>(sys("__Block_release", {reply.block}));
        }
        cancel_source(c->dead_source);
        cancel_source(c->inbox_source);
        std::uint32_t inbox, peer;
        { std::lock_guard lock(mutex); inbox = std::exchange(c->inbox, 0); peer = std::exchange(c->peer, 0); }
        drop_receive(inbox);
        drop_send(peer);
        if (interrupted && (c->mach_service || c->bundled)) {
            // The next send looks the service up again (its job may run anew).
            std::uint32_t service;
            { std::lock_guard lock(mutex); service = std::exchange(c->service, 0); }
            drop_send(service);
        }
        post(c, error);
        release_held(c);
    }

    // A peer the listener kept alive is let go once it has ended.
    void release_held(const std::shared_ptr<Conn>& c) {
        bool held;
        { std::lock_guard lock(mutex); held = c->finished && std::exchange(c->held, false); }
        if (held) xpc_->release(c->object);
    }

    // A client connects: hello to the service, carrying a send right to its new inbox.
    void connect(const std::shared_ptr<Conn>& c) {
        const auto v = value(c->object);
        std::string name;
        { std::lock_guard lock(v->mutex); name = v->text; }
        std::uint32_t service;
        {
            std::lock_guard lock(mutex);
            if (c->connecting || c->connected || c->finished) return;
            service = c->service;
        }
        if (!mach()) { fail(c, false); return; }
        if (!service && c->mach_service) service = look_up(name);
        if (!service && c->bundled) service = start_bundled(name);
        if (!service) { fail(c, false); return; }
        const auto inbox = new_receive();
        const auto queue = queue_of(c);
        {
            std::lock_guard lock(mutex);
            c->service = service;
            c->inbox = inbox;
            c->connecting = true;
        }
        c->inbox_source = source(c->object, Channel::inbox, inbox, queue);
        if (!send(service, copy_send, inbox, make_send, id_hello, {}, {})) {
            if (c->mach_service) { std::lock_guard lock(mutex); c->service = 0; }
            fail(c, c->interrupts);
        }
    }

    // A send right to `name` in the bootstrap namespace (0: unknown).
    std::uint32_t look_up(const std::string& name) {
        GuestCell cell(*this);
        const auto text = process().allocate_guest(name.size() + 1);
        put_string(memory(), text, name);
        const auto status = sys("_bootstrap_look_up", {bootstrap(), text, cell.address});
        process().release_guest(text);
        return status == 0 ? memory().read32(cell.address) : 0;
    }

    struct Bundled {
        std::filesystem::path executable;
        std::vector<std::pair<std::string, std::string>> environment;
    };

    // The .xpc bundle of the main bundle (or one of its frameworks) whose identifier is `name`.
    std::optional<Bundled> find_bundled(const std::string& name) {
        // The main bundle: the executable's Contents/MacOS/.. when it is in one.
        const auto executable = process().executable;
        const auto macos = executable.parent_path();
        if (macos.filename() != "MacOS" || macos.parent_path().filename() != "Contents") return std::nullopt;
        const auto contents = macos.parent_path();
        std::vector<std::filesystem::path> folders{contents / "XPCServices"};
        std::error_code error;
        for (const auto& framework : std::filesystem::directory_iterator(contents / "Frameworks", error)) {
            if (framework.path().extension() != ".framework") continue;
            folders.push_back(framework.path() / "XPCServices");
            for (const auto& version : std::filesystem::directory_iterator(framework.path() / "Versions", error))
                if (version.path().filename() != "Current") folders.push_back(version.path() / "XPCServices");
        }
        for (const auto& folder : folders) {
            std::error_code ignored;
            for (const auto& bundle : std::filesystem::directory_iterator(folder, ignored)) {
                if (bundle.path().extension() != ".xpc") continue;
                try {
                    const auto info = launchd::read_plist_file(path_utf8(bundle.path() / "Contents" / "Info.plist"));
                    const auto* identifier = info.find("CFBundleIdentifier");
                    const auto* program = info.find("CFBundleExecutable");
                    if (!identifier || identifier->text != name || !program || program->text.empty()) continue;
                    Bundled found;
                    found.executable = bundle.path() / "Contents" / "MacOS" / utf8_path(program->text);
                    if (const auto* service = info.find("XPCService"))
                        if (const auto* variables = service->find("EnvironmentVariables"))
                            for (const auto& [key, item] : variables->dictionary)
                                if (item.is(launchd::Value::Kind::string)) found.environment.emplace_back(key, item.text);
                    return found;
                } catch (const std::exception&) {
                    continue;
                }
            }
        }
        return std::nullopt;
    }

    std::set<std::string> starting;   // bundled services this process is starting (under mutex)

    // A bundled service of this application: the instance the tree has, or a new one started
    // here and waited for (it checks its name in from xpc_main).
    std::uint32_t start_bundled(const std::string& name) {
        if (const auto found = look_up(name)) return found;
        const auto bundle = find_bundled(name);
        if (!bundle) return 0;
        bool mine;
        { std::lock_guard lock(mutex); mine = starting.insert(name).second; }
        if (mine) {
            // argv {path}, the environment plus XPC_SERVICE_NAME and the bundle's variables.
            std::vector<std::string> extra{"XPC_SERVICE_NAME=" + name};
            for (const auto& [key, text] : bundle->environment) extra.push_back(key + "=" + text);
            const auto environment_cell = sys("__NSGetEnviron", {});
            const auto environment_array = environment_cell ? memory().read64(environment_cell) : 0;
            std::vector<std::uint64_t> pointers;
            for (std::uint64_t at = environment_array; at; at += 8) {
                const auto entry = memory().read64(at);
                if (!entry) break;
                if (read_text(memory(), entry).starts_with("XPC_SERVICE_NAME=")) continue;
                pointers.push_back(entry);
            }
            std::vector<std::uint64_t> owned;
            const auto guest_text = [&](const std::string& text) {
                const auto address = process().allocate_guest(text.size() + 1);
                put_string(memory(), address, text);
                owned.push_back(address);
                return address;
            };
            for (const auto& item : extra) pointers.push_back(guest_text(item));
            const auto path = guest_text(guest_path_string(bundle->executable));
            const auto envp = process().allocate_guest((pointers.size() + 1) * 8);
            for (std::size_t i = 0; i < pointers.size(); ++i) memory().write64(envp + i * 8, pointers[i]);
            memory().write64(envp + pointers.size() * 8, 0);
            const auto argv = process().allocate_guest(16);
            memory().write64(argv, path);
            memory().write64(argv + 8, 0);
            GuestCell pid(*this);
            const auto status = sys("_posix_spawn", {pid.address, path, 0, 0, argv, envp});
            process().release_guest(argv);
            process().release_guest(envp);
            for (const auto address : owned) process().release_guest(address);
            if (status != 0) { std::lock_guard lock(mutex); starting.erase(name); return 0; }
        }
        // Wait for its check-in (a cold service takes a while to start).
        std::uint32_t found = 0;
        for (int i = 0; i < 600 && !found; ++i) {
            found = look_up(name);
            if (!found) static_cast<void>(sys("_usleep", {50000}));
        }
        if (mine) { std::lock_guard lock(mutex); starting.erase(name); }
        return found;
    }

    void flush(const std::shared_ptr<Conn>& c) {
        for (;;) {
            std::uint64_t message;
            std::uint32_t peer;
            {
                std::lock_guard lock(mutex);
                if (!c->connected || c->outgoing.empty()) return;
                message = c->outgoing.front();
                c->outgoing.pop_front();
                peer = c->peer;
            }
            deliver_message(c, peer, message, 0, 0);
            xpc_->release(message);
        }
    }

    // A message to the peer, with an optional reply right (local).
    void deliver_message(const std::shared_ptr<Conn>& c, std::uint32_t peer, std::uint64_t message, std::uint32_t local, std::uint32_t local_disposition) {
        std::vector<std::uint8_t> body;
        std::vector<Port> ports;
        encode(message, body, ports);
        if (!send(peer, copy_send, local, local_disposition, id_message, body, ports)) fail(c, c->interrupts);
    }

    // Port events (a dispatch source handler on the connection's queue).
    void on_event(std::uint64_t id) {
        Channel channel;
        std::shared_ptr<Conn> c;
        {
            std::lock_guard lock(mutex);
            const auto found = channels.find(id);
            if (found == channels.end()) return;
            channel = found->second;
            const auto owner = conns.find(channel.connection);
            if (owner == conns.end()) return;
            c = owner->second;
        }
        if (channel.kind == Channel::dead) {
            { std::lock_guard lock(mutex); c->dead = true; }
            fail(c, c->interrupts);
            return;
        }
        if (channel.kind == Channel::reply) {
            while (auto in = receive(channel.port)) on_reply(c, channel.port, *in);
            return;
        }
        while (auto in = receive(channel.port)) {
            if (channel.kind == Channel::service) on_hello(c, *in);
            else on_inbox(c, *in);
        }
    }

    // A listener's hello: a new peer connection to its handler.
    void on_hello(const std::shared_ptr<Conn>& listener, Incoming& in) {
        if (in.id != id_hello || !in.remote) { release_ports(in.ports); if (in.remote) drop_send(in.remote); return; }
        const auto lv = value(listener->object);
        std::string name;
        { std::lock_guard lock(lv->mutex); name = lv->text; }
        const auto object = make_connection(name, 0);
        const auto peer = conn(object);
        const auto inbox = new_receive();
        {
            std::lock_guard lock(mutex);
            peer->role = Role::peer;
            peer->peer = in.remote;
            peer->inbox = inbox;
            peer->connected = true;
            peer->held = true;
            peer->audit = in.audit;
        }
        const auto queue = queue_of(peer);
        peer->inbox_source = source(object, Channel::inbox, inbox, queue);
        peer->dead_source = source(object, Channel::dead, in.remote, queue);
        static_cast<void>(send(in.remote, copy_send, inbox, make_send, id_hello_ack, {}, {}));
        handle(listener, object);   // +1 kept until the peer ends (release_held)
    }

    // A client or peer's inbox: the hello's answer, messages, goodbye.
    void on_inbox(const std::shared_ptr<Conn>& c, Incoming& in) {
        if (in.id == id_hello_ack) {
            std::uint64_t queue;
            {
                std::lock_guard lock(mutex);
                if (c->peer) drop_send(c->peer);
                c->peer = in.remote;
                c->connected = true;
                c->connecting = false;
                c->audit = in.audit;
                queue = c->queue;
            }
            c->dead_source = source(c->object, Channel::dead, in.remote, queue);
            flush(c);
            return;
        }
        if (in.id == id_goodbye) {
            if (in.remote) drop_send(in.remote);
            fail(c, false);
            return;
        }
        if (in.id != id_message) { release_ports(in.ports); if (in.remote) drop_send(in.remote); return; }
        { std::lock_guard lock(mutex); c->audit = in.audit; }
        Cursor cursor{in.body};
        const auto message = decode(cursor, in.ports);
        release_ports(in.ports);
        if (!message || !xpc_->typed(message, Type::dictionary)) {
            if (message) xpc_->release(message);
            if (in.remote) drop_send(in.remote);
            return;
        }
        {
            std::lock_guard lock(mutex);
            received[message] = Received{in.remote_type == move_send_once ? in.remote : 0, c->object};
        }
        handle(c, message);
        xpc_->release(message);
    }

    void on_reply(const std::shared_ptr<Conn>& c, std::uint32_t port, Incoming& in) {
        PendingReply reply;
        {
            std::lock_guard lock(mutex);
            const auto found = std::find_if(c->replies.begin(), c->replies.end(), [&](const PendingReply& r) { return r.port == port; });
            if (found == c->replies.end()) { release_ports(in.ports); return; }
            reply = *found;
            c->replies.erase(found);
            c->audit = in.audit;
        }
        Cursor cursor{in.body};
        const auto message = in.id == id_reply ? decode(cursor, in.ports) : 0;
        release_ports(in.ports);
        cancel_source(reply.source);
        drop_receive(reply.port);
        const auto event = message ? message : xpc_->retain(xpc_->connection_invalid);
        xpc_->deliver(reply.queue, reply.block, event);
        xpc_->release(event);
        static_cast<void>(sys("__Block_release", {reply.block}));
    }

    // Where a message goes: a reply dictionary to its send-once right, else to the peer.
    void send_message(std::uint64_t object, std::uint64_t message, std::uint64_t reply_queue, std::uint64_t reply_block) {
        const auto c = conn(object);
        std::uint32_t answer_to = 0;
        {
            std::lock_guard lock(mutex);
            if (const auto found = reply_to.find(message); found != reply_to.end()) { answer_to = found->second; reply_to.erase(found); }
        }
        if (answer_to) {
            std::vector<std::uint8_t> body;
            std::vector<Port> ports;
            encode(message, body, ports);
            static_cast<void>(send(answer_to, move_send_once, 0, 0, id_reply, body, ports));
            return;
        }
        if (reply_block) {
            bool ended;
            { std::lock_guard lock(mutex); ended = c->finished; }
            if (ended) { xpc_->deliver(reply_queue ? reply_queue : queue_of(c), reply_block, xpc_->connection_invalid); return; }
            const auto port = new_receive();
            const auto block = sys("__Block_copy", {reply_block});
            const auto queue = reply_queue ? reply_queue : queue_of(c);
            PendingReply pending{port, 0, block, queue};
            pending.source = source(object, Channel::reply, port, queue);
            std::uint32_t peer;
            bool connected, finished;
            {
                std::lock_guard lock(mutex);
                c->replies.push_back(pending);
                connected = c->connected; finished = c->finished; peer = c->peer;
            }
            if (finished) { fail(c, false); return; }   // ended meanwhile: the reply above gets the error
            if (!connected) {
                // A reply right needs the peer: connect and send once there.
                ensure_connected(c);
                std::lock_guard lock(mutex);
                connected = c->connected; peer = c->peer;
            }
            if (!connected || !peer) { fail(c, c->interrupts); return; }
            std::vector<std::uint8_t> body;
            std::vector<Port> ports;
            encode(message, body, ports);
            if (!send(peer, copy_send, port, make_send_once, id_message, body, ports)) fail(c, c->interrupts);
            return;
        }
        bool connected;
        std::uint32_t peer;
        {
            std::lock_guard lock(mutex);
            connected = c->connected;
            peer = c->peer;
            if (!connected) c->outgoing.push_back(xpc_->retain(message));
        }
        if (connected) deliver_message(c, peer, message, 0, 0);
        else schedule_connect(c);
    }

    // A resumed client that is not connected connects on its queue; the waiting messages follow.
    void schedule_connect(const std::shared_ptr<Conn>& c) {
        bool resumed;
        { const auto v = value(c->object); std::lock_guard lock(v->mutex); resumed = v->resumed; }
        {
            std::lock_guard lock(mutex);
            if (!resumed || c->role != Role::client || c->connecting || c->connected || c->finished) return;
        }
        static_cast<void>(xpc_->retain(c->object));
        static_cast<void>(sys("_dispatch_async_f", {queue_of(c), c->object, connect_entry}));
    }

    // A client that is not connected (yet, or after an interruption) connects, and waits a
    // little for the answer when called off its queue (sync replies need it).
    void ensure_connected(const std::shared_ptr<Conn>& c) {
        bool resumed;
        { const auto v = value(c->object); std::lock_guard lock(v->mutex); resumed = v->resumed; }
        if (!resumed || c->role != Role::client) return;
        connect(c);
        // The answer is read here directly (the inbox source may run on another thread first).
        for (int i = 0; i < 200; ++i) {
            std::uint32_t inbox;
            { std::lock_guard lock(mutex); if (c->connected || c->finished || !c->connecting) return; inbox = c->inbox; }
            if (auto in = receive(inbox, 50)) on_inbox(c, *in);
        }
    }

    std::uint64_t send_sync(std::uint64_t object, std::uint64_t message) {
        const auto c = conn(object);
        ensure_connected(c);
        std::uint32_t peer;
        bool connected;
        { std::lock_guard lock(mutex); connected = c->connected; peer = c->peer; }
        if (!connected) return xpc_->retain(failure(c));
        const auto port = new_receive();
        std::vector<std::uint8_t> body;
        std::vector<Port> ports;
        encode(message, body, ports);
        if (!send(peer, copy_send, port, make_send_once, id_message, body, ports)) {
            drop_receive(port);
            return xpc_->retain(failure(c));
        }
        for (;;) {
            if (auto in = receive(port, 200)) {
                drop_receive(port);
                { std::lock_guard lock(mutex); c->audit = in->audit; }
                Cursor cursor{in->body};
                const auto reply = in->id == id_reply ? decode(cursor, in->ports) : 0;
                release_ports(in->ports);
                return reply ? reply : xpc_->retain(xpc_->connection_invalid);
            }
            bool gone;
            { std::lock_guard lock(mutex); gone = c->dead || c->finished || !c->peer; }
            if (gone) {
                drop_receive(port);
                return xpc_->retain(failure(c));
            }
        }
    }

    void resume(std::uint64_t object) {
        const auto c = conn(object);
        const auto v = value(object);
        bool first;
        { std::lock_guard lock(v->mutex); first = !v->resumed; v->resumed = true; }
        if (!first) return;
        // Events that waited for the handler.
        std::deque<std::uint64_t> waiting;
        { std::lock_guard lock(mutex); waiting.swap(c->incoming); }
        const auto queue = queue_of(c);
        for (const auto event : waiting) {
            std::uint64_t handler;
            { std::lock_guard lock(v->mutex); handler = v->handler; }
            if (handler) xpc_->deliver(queue, handler, event);
            xpc_->release(event);
        }
        Role role;
        std::uint32_t service;
        { std::lock_guard lock(mutex); role = c->role; service = c->service; }
        if (role == Role::listener) {
            if (service) c->service_source = source(object, Channel::service, service, queue);
            else fail(c, false);
            return;
        }
        if (role != Role::client) return;
        bool bundled;
        { std::lock_guard lock(mutex); bundled = c->bundled; }
        if (!bundled) { connect(c); return; }
        // Starting a bundled service can take seconds: on the connection's queue, not here.
        static_cast<void>(xpc_->retain(object));
        static_cast<void>(sys("_dispatch_async_f", {queue, object, connect_entry}));
    }

    void cancel(std::uint64_t object) {
        const auto c = conn(object);
        const auto v = value(object);
        { std::lock_guard lock(v->mutex); v->cancelled = true; v->resumed = true; }
        std::uint32_t peer, service;
        Role role;
        { std::lock_guard lock(mutex); peer = c->peer; service = c->service; role = c->role; }
        if (peer) static_cast<void>(send(peer, copy_send, 0, 0, id_goodbye, {}, {}));
        cancel_source(c->service_source);
        if (role == Role::listener) { drop_receive(service); std::lock_guard lock(mutex); c->service = 0; }
        // The invalid error comes last, on the connection's queue.
        const auto queue = queue_of(c);
        std::uint64_t handler;
        { std::lock_guard lock(v->mutex); handler = v->handler; }
        std::vector<PendingReply> replies;
        {
            std::lock_guard lock(mutex);
            if (c->finished) return;
            c->finished = true;
            replies.swap(c->replies);
        }
        for (auto& reply : replies) {
            cancel_source(reply.source);
            drop_receive(reply.port);
            xpc_->deliver(reply.queue, reply.block, xpc_->connection_invalid);
            static_cast<void>(sys("__Block_release", {reply.block}));
        }
        cancel_source(c->dead_source);
        cancel_source(c->inbox_source);
        std::uint32_t inbox;
        { std::lock_guard lock(mutex); inbox = std::exchange(c->inbox, 0); peer = std::exchange(c->peer, 0); }
        drop_receive(inbox);
        drop_send(peer);
        if (handler) xpc_->deliver(queue, handler, xpc_->connection_invalid);
        release_held(c);
    }
};

} // namespace

void install_xpc_connections(Provider& p, const std::shared_ptr<Xpc>& xpc) {
    const auto self = std::make_shared<Connections>(p, xpc);
    self->event_entry = p.process().install_function("__alchemy_xpc_port_event", [self](CpuState& s, GuestMemory&) {
        self->on_event(s.x[0]);
        s.x[0] = 0;
    });
    // A resumed client's connect, on its queue (context: the connection, retained by resume).
    self->connect_entry = p.process().install_function("__alchemy_xpc_connect", [self](CpuState& s, GuestMemory&) {
        const auto object = s.x[0];
        self->connect(self->conn(object));
        self->flush(self->conn(object));
        self->xpc_->release(object);
        s.x[0] = 0;
    });

    // ---- creation ----
    // xpc_connection_create(name, queue): NULL is an anonymous listener; a name is a bundled XPC
    // service of this application.
    p.libsystem("_xpc_connection_create", [self](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto name = s.x[0] ? read_text(m, s.x[0]) : std::string("anonymous");
        const auto object = self->make_connection(name, s.x[1]);
        const auto c = self->conn(object);
        if (!s.x[0]) {
            const auto port = self->new_receive();
            std::lock_guard lock(self->mutex);
            c->role = Role::listener;
            c->service = port;
        } else {
            // A bundled XPC service; a dead instance is replaced on the next send.
            std::lock_guard lock(self->mutex);
            c->role = Role::client;
            c->bundled = c->interrupts = true;
        }
        return object;
    }, 0);
    // xpc_connection_create_mach_service(name, queue, flags).
    p.libsystem("_xpc_connection_create_mach_service", [self](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto name = s.x[0] ? read_text(m, s.x[0]) : std::string();
        const auto object = self->make_connection(name, s.x[1]);
        const auto c = self->conn(object);
        const auto flags = s.x[2];
        if (flags & mach_service_privileged) return object;   // the system domain: invalid at resume
        if ((flags & mach_service_listener) && !self->mach()) { std::lock_guard lock(self->mutex); c->role = Role::listener; return object; }
        if (flags & mach_service_listener) {
            Connections::GuestCell cell(*self);
            const auto text = self->process().allocate_guest(name.size() + 1);
            put_string(m, text, name);
            const auto status = self->sys("_bootstrap_check_in", {self->bootstrap(), text, cell.address});
            self->process().release_guest(text);
            std::lock_guard lock(self->mutex);
            c->role = Role::listener;
            c->service = status == 0 ? m.read32(cell.address) : 0;
            return object;
        }
        std::lock_guard lock(self->mutex);
        c->role = Role::client;
        c->mach_service = c->interrupts = true;
        return object;
    }, 0);
    p.libsystem("_xpc_connection_create_from_endpoint", [self](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto endpoint = self->xpc_->typed(s.x[0], Type::endpoint);
        if (!endpoint) return 0;
        const auto target = self->conn(endpoint->connection);
        const auto tv = self->value(endpoint->connection);
        std::string name;
        { std::lock_guard lock(tv->mutex); name = tv->text; }
        std::uint32_t service;
        Role role;
        { std::lock_guard lock(self->mutex); service = target->service; role = target->role; }
        const auto object = self->make_connection(name, 0);
        const auto c = self->conn(object);
        // A send right of our own to the listener.
        if (service) static_cast<void>(self->sys("_mach_port_insert_right", {self->task(), service, service, role == Role::listener ? make_send : copy_send}));
        std::lock_guard lock(self->mutex);
        c->role = Role::client;
        c->service = service;
        return object;
    }, 0);
    p.libsystem("_xpc_endpoint_create", [self](CpuState& s, GuestMemory&) -> std::uint64_t {
        static_cast<void>(self->conn(s.x[0]));
        const auto connection = self->xpc_->retain(s.x[0]);
        return self->xpc_->make(Type::endpoint, [&](Value& v) { v.connection = connection; });
    }, 0);

    // ---- configuration ----
    p.libsystem("_xpc_connection_set_event_handler", [self](CpuState& s, GuestMemory&) -> std::uint64_t {
        static_cast<void>(self->conn(s.x[0]));
        const auto v = self->value(s.x[0]);
        const auto copy = s.x[1] ? self->sys("__Block_copy", {s.x[1]}) : 0;
        std::uint64_t old;
        { std::lock_guard lock(v->mutex); old = std::exchange(v->handler, copy); }
        if (old) static_cast<void>(self->sys("__Block_release", {old}));
        return 0;
    });
    p.libsystem("_xpc_connection_set_target_queue", [self](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto c = self->conn(s.x[0]);
        const auto v = self->value(s.x[0]);
        if (s.x[1]) static_cast<void>(self->sys("_dispatch_retain", {s.x[1]}));
        std::uint64_t old;
        { std::lock_guard lock(v->mutex); old = std::exchange(v->queue, s.x[1]); }
        std::uint64_t own;
        { std::lock_guard lock(self->mutex); own = c->queue; }
        if (own && s.x[1]) static_cast<void>(self->sys("_dispatch_set_target_queue", {own, s.x[1]}));
        if (old) static_cast<void>(self->sys("_dispatch_release", {old}));
        return 0;
    });
    for (const auto* name : {"_xpc_connection_resume", "_xpc_connection_activate"})
        p.libsystem(name, [self](CpuState& s, GuestMemory&) -> std::uint64_t { self->resume(s.x[0]); return 0; });
    p.libsystem("_xpc_connection_suspend", [self](CpuState& s, GuestMemory&) -> std::uint64_t { static_cast<void>(self->conn(s.x[0])); return 0; });
    p.libsystem("_xpc_connection_cancel", [self](CpuState& s, GuestMemory&) -> std::uint64_t { self->cancel(s.x[0]); return 0; });

    // ---- messages ----
    p.libsystem("_xpc_connection_send_message", [self](CpuState& s, GuestMemory&) -> std::uint64_t {
        self->send_message(s.x[0], s.x[1], 0, 0);
        return 0;
    });
    p.libsystem("_xpc_connection_send_message_with_reply", [self](CpuState& s, GuestMemory&) -> std::uint64_t {
        self->send_message(s.x[0], s.x[1], s.x[2], s.x[3]);
        return 0;
    });
    p.libsystem("_xpc_connection_send_message_with_reply_sync", [self](CpuState& s, GuestMemory&) -> std::uint64_t {
        return self->send_sync(s.x[0], s.x[1]);
    }, 0);
    p.libsystem("_xpc_connection_send_barrier", [self](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto c = self->conn(s.x[0]);
        if (s.x[1]) self->xpc_->deliver(self->queue_of(c), s.x[1], 0);   // dispatch_block_t ignores the argument
        return 0;
    });
    // A reply to a message received from a peer: the sender's reply right.
    p.libsystem("_xpc_dictionary_create_reply", [self](CpuState& s, GuestMemory&) -> std::uint64_t {
        std::uint32_t right;
        {
            std::lock_guard lock(self->mutex);
            const auto found = self->received.find(s.x[0]);
            if (found == self->received.end() || !found->second.reply) return 0;
            right = std::exchange(found->second.reply, 0);
        }
        const auto reply = self->xpc_->make(Type::dictionary);
        std::lock_guard lock(self->mutex);
        self->reply_to[reply] = right;
        return reply;
    }, 0);
    p.libsystem("_xpc_dictionary_get_remote_connection", [self](CpuState& s, GuestMemory&) -> std::uint64_t {
        std::lock_guard lock(self->mutex);
        const auto found = self->received.find(s.x[0]);
        return found == self->received.end() ? 0 : found->second.connection;
    }, 0);

    // ---- identity ----
    p.libsystem("_xpc_connection_get_name", [self](CpuState& s, GuestMemory&) -> std::uint64_t {
        static_cast<void>(self->conn(s.x[0]));
        const auto v = self->value(s.x[0]);
        std::lock_guard lock(v->mutex);
        if (!v->guest) v->guest = self->xpc_->guest_copy(std::span(reinterpret_cast<const std::uint8_t*>(v->text.data()), v->text.size()), true);
        return v->guest;
    }, 0);
    // audit_token_t: [1] euid, [2] egid, [3] ruid, [4] rgid, [5] pid, [6] asid, [7] pidversion.
    const std::array<std::pair<const char*, std::size_t>, 4> identity{{
        {"_xpc_connection_get_pid", 5}, {"_xpc_connection_get_euid", 1}, {"_xpc_connection_get_egid", 2}, {"_xpc_connection_get_asid", 6}}};
    for (const auto& [name, index] : identity)
        p.libsystem(name, [self, index = index](CpuState& s, GuestMemory&) -> std::uint64_t {
            const auto c = self->conn(s.x[0]);
            std::lock_guard lock(self->mutex);
            return c->audit[index];
        });
    p.libsystem("_xpc_connection_get_audit_token", [self](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto c = self->conn(s.x[0]);
        std::array<std::uint8_t, 32> bytes{};
        { std::lock_guard lock(self->mutex); std::memcpy(bytes.data(), c->audit.data(), 32); }
        if (s.x[1]) m.write_bytes(s.x[1], bytes);
        return 0;
    });
    p.libsystem("_xpc_connection_set_context", [self](CpuState& s, GuestMemory&) -> std::uint64_t {
        static_cast<void>(self->conn(s.x[0])); const auto v = self->value(s.x[0]); std::lock_guard lock(v->mutex); v->context = s.x[1]; return 0;
    });
    p.libsystem("_xpc_connection_get_context", [self](CpuState& s, GuestMemory&) -> std::uint64_t {
        static_cast<void>(self->conn(s.x[0])); const auto v = self->value(s.x[0]); std::lock_guard lock(v->mutex); return v->context;
    }, 0);
    p.libsystem("_xpc_connection_set_finalizer_f", [self](CpuState& s, GuestMemory&) -> std::uint64_t {
        static_cast<void>(self->conn(s.x[0])); const auto v = self->value(s.x[0]); std::lock_guard lock(v->mutex); v->finalizer = s.x[1]; return 0;
    });
    // int xpc_connection_set_peer_code_signing_requirement(connection, const char*): Alchemy
    // has no code signatures to check a peer against; the requirement is kept, unchecked.
    p.libsystem("_xpc_connection_set_peer_code_signing_requirement", [self](CpuState& s, GuestMemory&) -> std::uint64_t {
        static_cast<void>(self->conn(s.x[0])); return 0;
    });
    // Connections stored in dictionaries (only within this process: they do not travel).
    p.libsystem("_xpc_dictionary_set_connection", [self](CpuState& s, GuestMemory& m) -> std::uint64_t {
        static_cast<void>(self->conn(s.x[2]));
        self->xpc_->set(s.x[0], read_text(m, s.x[1]), s.x[2], false);
        return 0;
    });
    p.libsystem("_xpc_dictionary_create_connection", [self](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto item = self->xpc_->get(s.x[0], read_text(m, s.x[1]));
        return self->xpc_->typed(item, Type::connection) ? self->xpc_->retain(item) : 0;
    }, 0);

    // xpc_main(handler): the service named XPC_SERVICE_NAME is a listener whose peers go to the
    // handler, and the process serves them until it is ended: a bundled XPC service (the name
    // checked in to its application's tree), or a job of the session launchd (its label, which
    // the job declares in MachServices). Elsewhere this process is not an XPC service, and
    // xpc_main stops.
    p.libsystem("_xpc_main", [self](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto key = self->process().allocate_guest(32);
        put_string(m, key, "XPC_SERVICE_NAME");
        const auto name = self->sys("_getenv", {key});
        self->process().release_guest(key);
        if (!name) throw std::runtime_error("Alchemy: xpc_main: this process was not started as an XPC service");
        const auto listener = self->sys("_xpc_connection_create_mach_service", {name, 0, mach_service_listener});
        const auto c = self->conn(listener);
        std::uint32_t service;
        { std::lock_guard lock(self->mutex); service = c->service; }
        if (!service) throw std::runtime_error("Alchemy: xpc_main: the service " + read_text(m, name) + " could not be checked in (is it in the job's MachServices?)");
        self->sys("_xpc_connection_set_event_handler", {listener, s.x[0]});
        self->resume(listener);
        static_cast<void>(self->sys("_dispatch_main", {}));
        return 0;
    });
    // Transactions count outstanding work for sudden termination, which never happens here.
    for (const auto* name : {"_xpc_transaction_begin", "_xpc_transaction_end"})
        p.libsystem(name, [](CpuState&, GuestMemory&) -> std::uint64_t { return 0; });
}

} // namespace alchemy::export_fill::xpc_detail
