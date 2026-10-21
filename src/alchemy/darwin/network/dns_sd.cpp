#include "alchemy/darwin/network/network_internal.hpp"
#include "alchemy/darwin/network/mdns.hpp"

#include "alchemy/engine_adapter.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>

// dns_sd (libSystem's DNS Service Discovery client API, /usr/include/dns_sd.h)
// over the host mDNS engine (mdns.hpp). libndi (NDI discovery in QLab)
// imports it. Every export is installed only when no other provider exports
// that symbol.
//
// Model. A DNSServiceRef is an opaque guest handle. A primary ref (one made
// without kDNSServiceFlagsShareConnection, or by DNSServiceCreateConnection)
// owns a connection: a guest AF_UNIX socket pair whose read end is
// DNSServiceRefSockFD and a queue of results. Host mDNS callbacks append
// results to the queue; the first result after the queue was drained makes
// the guest pump write one byte to the write end, so the descriptor becomes
// readable exactly as with mDNSResponder. DNSServiceProcessResult reads that
// byte (blocking when nothing is pending, like the real call), takes every
// queued result and runs the reply callbacks on the calling thread.
// Subordinate refs (kDNSServiceFlagsShareConnection with *sdRef the primary)
// share the primary's socket and queue; deallocating the primary ends them.
//
// Provided: DNSServiceCreateConnection, DNSServiceRefSockFD,
// DNSServiceProcessResult, DNSServiceRefDeallocate, DNSServiceBrowse,
// DNSServiceResolve, DNSServiceRegister, DNSServiceGetAddrInfo,
// DNSServiceConstructFullName and the TXTRecord* helpers. Only the "local."
// domain exists: another domain, a host-proxy registration (host != NULL)
// and the LocalOnly / Unicast / P2P / BLE interface indexes return
// kDNSServiceErr_Unsupported. Resolves call back once (Windows resolves are
// one-shot) and registrations keep the requested name (Windows reports no
// rename). Not provided (unresolved, so callers fail at bind time):
// DNSServiceQueryRecord, Add/Update/RemoveRecord, EnumerateDomains,
// NATPortMapping, GetProperty, ReconfirmRecord.
namespace alchemy::gui::network {
namespace {

constexpr std::string_view libsystem = security::libsystem_library;
namespace err = mdns::error;

namespace flag {
inline constexpr std::uint64_t more_coming = 0x1, add = 0x2, share_connection = 0x4000;
} // namespace flag
namespace interface_index {
inline constexpr std::uint32_t local_only = 0xffffffffu, unicast = 0xfffffffeu, p2p = 0xfffffffdu, ble = 0xfffffffcu;
} // namespace interface_index

enum class Kind { connection, browse, resolve, registration, address };

struct Pending { Address handle; mdns::Event event; };

struct Connection {
    std::mutex mutex;
    std::deque<Pending> queue;
    bool signal_requested{};
    std::mutex io;                       // held while the pump writes or the connection closes
    std::int32_t read_fd{-1}, write_fd{-1};
    Address buffers{};                   // [0] the wake-up byte written, [8] the byte read
    Address handle{};                    // the primary ref
    std::vector<Address> members;        // every ref on this connection (primary first)
    std::shared_ptr<void> lease;
};

struct Record {
    Address handle{};
    Kind kind{};
    Address callback{}, context{};
    std::shared_ptr<Connection> connection;
    std::unique_ptr<mdns::Operation> operation;
};

std::uint16_t swap16(std::uint64_t value) {
    const auto v = static_cast<std::uint16_t>(value);
    return static_cast<std::uint16_t>((v >> 8) | (v << 8));
}

std::string read_cstring(GuestMemory& m, Address at, std::size_t limit = 1024) {
    std::string text;
    if (!at) return text;
    for (; text.size() < limit; ++at) {
        const auto c = m.read8(at);
        if (!c) break;
        text.push_back(static_cast<char>(c));
    }
    return text;
}

class DnsSd : public std::enable_shared_from_this<DnsSd> {
public:
    explicit DnsSd(std::shared_ptr<Bridge> bridge) : b(std::move(bridge)), pump(guest_pump()) {}
    std::shared_ptr<Bridge> b;
    std::shared_ptr<GuestPump> pump;
    std::mutex mutex;
    std::map<Address, std::shared_ptr<Record>> records;

    std::shared_ptr<Record> find(Address handle) {
        std::lock_guard lock(mutex);
        const auto found = records.find(handle);
        return found == records.end() ? nullptr : found->second;
    }

    // A new primary connection with its socket pair (guest thread).
    std::int32_t open_connection(std::shared_ptr<Record>& out) {
        auto& m = b->memory();
        const auto fds = b->process.allocate_guest(8);
        const auto status = static_cast<std::int32_t>(b->call(libsystem, "_socketpair", {1, 1, 0, fds}));   // AF_UNIX, SOCK_STREAM
        const auto read_fd = static_cast<std::int32_t>(m.read32(fds)), write_fd = static_cast<std::int32_t>(m.read32(fds + 4));
        b->process.release_guest(fds);
        if (status != 0) return err::service_not_running;
        auto connection = std::make_shared<Connection>();
        connection->read_fd = read_fd;
        connection->write_fd = write_fd;
        connection->buffers = b->process.allocate_guest(16);
        m.write8(connection->buffers, 1);
        connection->lease = pump->lease();
        auto record = std::make_shared<Record>();
        record->kind = Kind::connection;
        record->connection = connection;
        record->handle = b->process.allocate_guest(16);
        connection->handle = record->handle;
        connection->members.push_back(record->handle);
        {
            std::lock_guard lock(mutex);
            records[record->handle] = record;
        }
        out = record;
        return err::none;
    }

    // The record for a new operation: a fresh primary, or a subordinate of
    // *sdRef with kDNSServiceFlagsShareConnection (guest thread).
    std::int32_t make_record(Address out, std::uint64_t flags, Kind kind, Address callback, Address context,
                             std::shared_ptr<Record>& result) {
        if (!out) return err::bad_param;
        auto& m = b->memory();
        if (flags & flag::share_connection) {
            const auto primary = find(m.read64(out));
            if (!primary || primary->kind != Kind::connection) return err::bad_reference;
            auto record = std::make_shared<Record>();
            record->kind = kind;
            record->callback = callback;
            record->context = context;
            record->connection = primary->connection;
            record->handle = b->process.allocate_guest(16);
            {
                std::lock_guard lock(primary->connection->mutex);
                primary->connection->members.push_back(record->handle);
            }
            {
                std::lock_guard lock(mutex);
                records[record->handle] = record;
            }
            result = record;
            return err::none;
        }
        std::shared_ptr<Record> record;
        if (const auto status = open_connection(record); status != err::none) return status;
        record->kind = kind;
        record->callback = callback;
        record->context = context;
        result = record;
        return err::none;
    }

    // Host sink for a record's operation: queue the event and wake the socket.
    mdns::Sink sink(const std::shared_ptr<Record>& record) {
        const std::weak_ptr<Connection> weak = record->connection;
        const auto handle = record->handle;
        const auto self = shared_from_this();
        return [self, weak, handle](mdns::Event event) {
            const auto connection = weak.lock();
            if (!connection) return;
            bool signal{};
            {
                std::lock_guard lock(connection->mutex);
                connection->queue.push_back({handle, std::move(event)});
                if (!connection->signal_requested) { connection->signal_requested = true; signal = true; }
            }
            if (!signal) return;
            self->pump->post([self, weak] {
                const auto alive = weak.lock();
                if (!alive) return;
                std::lock_guard io(alive->io);
                if (alive->write_fd < 0) return;
                static_cast<void>(self->b->call(libsystem, "_write", {static_cast<Address>(alive->write_fd), alive->buffers, 1}));
            });
        };
    }

    // Completes a new record: publishes the handle to *sdRef, or undoes it.
    std::int32_t finish(Address out, const std::shared_ptr<Record>& record, std::unique_ptr<mdns::Operation> operation,
                        std::int32_t status) {
        if (!operation) {
            deallocate(record->handle);
            return status == err::none ? err::unknown : status;
        }
        record->operation = std::move(operation);
        b->memory().write64(out, record->handle);
        return err::none;
    }

    // DNSServiceRefDeallocate (guest thread).
    void deallocate(Address handle) {
        const auto record = find(handle);
        if (!record) return;
        std::vector<std::shared_ptr<Record>> ended;
        const auto connection = record->connection;
        const bool primary = connection->handle == handle;
        {
            std::lock_guard lock(mutex);
            if (primary) {
                std::vector<Address> members;
                {
                    std::lock_guard guard(connection->mutex);
                    members = connection->members;
                    connection->members.clear();
                    connection->queue.clear();
                }
                for (const auto member : members) {
                    if (const auto found = records.find(member); found != records.end()) {
                        ended.push_back(found->second);
                        records.erase(found);
                    }
                }
            } else {
                records.erase(handle);
                ended.push_back(record);
                std::lock_guard guard(connection->mutex);
                std::erase(connection->members, handle);
                std::erase_if(connection->queue, [handle](const Pending& item) { return item.handle == handle; });
            }
        }
        for (const auto& item : ended) {
            if (item->operation) item->operation->cancel();
            b->process.release_guest(item->handle);
        }
        if (!primary) return;
        std::int32_t read_fd, write_fd;
        {
            std::lock_guard io(connection->io);
            read_fd = std::exchange(connection->read_fd, -1);
            write_fd = std::exchange(connection->write_fd, -1);
        }
        if (read_fd >= 0) static_cast<void>(b->call(libsystem, "_close", {static_cast<Address>(read_fd)}));
        if (write_fd >= 0) static_cast<void>(b->call(libsystem, "_close", {static_cast<Address>(write_fd)}));
        connection->lease.reset();
        // The buffers stay allocated: a pump job may still hold the connection
        // (it checks write_fd under `io` before writing).
    }

    // DNSServiceProcessResult (guest thread).
    std::int32_t process(Address handle) {
        const auto record = find(handle);
        if (!record) return err::bad_reference;
        const auto connection = record->connection;
        if (connection->handle != handle) return err::bad_reference;   // only the primary ref
        std::int32_t read_fd;
        {
            std::lock_guard io(connection->io);
            read_fd = connection->read_fd;
        }
        if (read_fd < 0) return err::bad_reference;
        const auto got = static_cast<std::int64_t>(b->call(libsystem, "_read", {static_cast<Address>(read_fd), connection->buffers + 8, 1}));
        if (got <= 0) return err::service_not_running;
        std::deque<Pending> batch;
        {
            std::lock_guard lock(connection->mutex);
            batch.swap(connection->queue);
            connection->signal_requested = false;
        }
        for (auto& item : batch) {
            const auto target = find(item.handle);
            if (!target || target->connection != connection) continue;   // deallocated by an earlier callback
            deliver(*target, item.event);
        }
        return err::none;
    }

    // Guest C strings for one callback.
    struct Strings {
        Bridge& b;
        std::vector<Address> owned;
        Address operator()(std::string_view text) {
            const auto at = b.process.allocate_guest(text.size() + 1);
            b.memory().write_bytes(at, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
            b.memory().write8(at + text.size(), 0);
            owned.push_back(at);
            return at;
        }
        Address bytes(std::span<const std::uint8_t> data) {
            const auto at = b.process.allocate_guest(std::max<std::size_t>(data.size(), 1));
            if (!data.empty()) b.memory().write_bytes(at, data);
            owned.push_back(at);
            return at;
        }
        ~Strings() { for (const auto at : owned) b.process.release_guest(at); }
    };

    void deliver(const Record& record, const mdns::Event& event) {
        if (!record.callback) return;
        Strings text{*b};
        const bool failed = event.kind == mdns::Event::Kind::failed;
        const std::uint64_t error = static_cast<std::uint32_t>(failed ? event.error : err::none);
        std::uint64_t flags = event.more_coming ? flag::more_coming : 0;
        switch (record.kind) {
        case Kind::browse: {
            if (event.kind == mdns::Event::Kind::found) flags |= flag::add;
            // DNSServiceBrowseReply(sdRef, flags, interfaceIndex, errorCode, serviceName, regtype, replyDomain, context)
            const std::array<Address, 8> arguments{record.handle, flags, event.interface_index, error, text(event.name),
                                                   text(event.type), text(event.domain.empty() ? "local." : event.domain), record.context};
            static_cast<void>(b->call_address(record.callback, arguments));
            break;
        }
        case Kind::resolve: {
            // DNSServiceResolveReply(sdRef, flags, interfaceIndex, errorCode, fullname, hosttarget,
            //                        port (network order), txtLen, txtRecord, context)
            const auto full = failed ? std::string{} : mdns::escape_label(event.name) + "." + event.type + "local.";
            CallRegs registers;
            registers.x = {record.handle, flags, event.interface_index, error, text(full), text(event.host), swap16(event.port),
                           event.txt.size()};
            registers.stack.resize(16);
            const auto txt = text.bytes(event.txt);
            std::memcpy(registers.stack.data(), &txt, 8);
            std::memcpy(registers.stack.data() + 8, &record.context, 8);
            static_cast<void>(EngineAdapter(b->process.calling_engine()).call_regs(record.callback, registers));
            break;
        }
        case Kind::registration: {
            if (!failed) flags |= flag::add;
            // DNSServiceRegisterReply(sdRef, flags, errorCode, name, regtype, domain, context)
            const std::array<Address, 7> arguments{record.handle, flags, error, text(event.name), text(event.type),
                                                   text(event.domain.empty() ? "local." : event.domain), record.context};
            static_cast<void>(b->call_address(record.callback, arguments));
            break;
        }
        case Kind::address: {
            Address address{};
            if (!failed && !event.addresses.empty()) {
                flags |= flag::add;
                address = text.bytes(event.addresses.front());
            } else {
                const std::array<std::uint8_t, 16> empty{16, 2};   // sockaddr_in, AF_INET, no address
                address = text.bytes(empty);
            }
            // DNSServiceGetAddrInfoReply(sdRef, flags, interfaceIndex, errorCode, hostname, address, ttl, context)
            const std::array<Address, 8> arguments{record.handle, flags, event.interface_index, error, text(event.host), address,
                                                   event.ttl, record.context};
            static_cast<void>(b->call_address(record.callback, arguments));
            break;
        }
        case Kind::connection: break;
        }
    }
};

// ---- TXTRecord (TXTRecordRef: buffer pointer, buffer size, data length, malloced) ----------------
struct TxtRef {
    Address buffer{};
    std::uint16_t size{}, length{};
    bool malloced{};
};
TxtRef read_ref(GuestMemory& m, Address ref) {
    return {m.read64(ref), static_cast<std::uint16_t>(m.read32(ref + 8)), static_cast<std::uint16_t>(m.read32(ref + 8) >> 16),
            m.read8(ref + 12) != 0};
}
void write_ref(GuestMemory& m, Address ref, const TxtRef& value) {
    m.write64(ref, value.buffer);
    m.write32(ref + 8, static_cast<std::uint32_t>(value.size) | (static_cast<std::uint32_t>(value.length) << 16));
    m.write8(ref + 12, value.malloced ? 1 : 0);
}
Bytes read_bytes(GuestMemory& m, Address at, std::size_t size) {
    Bytes bytes(size);
    if (size && at) m.read_bytes(at, bytes);
    return bytes;
}

// The offset of the item for `key` (case-insensitive) in a TXT record, or npos.
std::size_t search(std::span<const std::uint8_t> txt, std::string_view key) {
    std::size_t at = 0;
    while (at < txt.size()) {
        const std::size_t length = txt[at];
        if (at + 1 + length > txt.size()) return std::string_view::npos;
        if (length >= key.size()) {
            bool same = true;
            for (std::size_t index = 0; index < key.size() && same; ++index)
                same = std::tolower(txt[at + 1 + index]) == std::tolower(static_cast<unsigned char>(key[index]));
            if (same && (length == key.size() || txt[at + 1 + key.size()] == '=')) return at;
        }
        at += 1 + length;
    }
    return std::string_view::npos;
}

bool valid_key(std::string_view key) {
    if (key.empty()) return false;
    return std::all_of(key.begin(), key.end(), [](char c) { return c >= 0x20 && c <= 0x7e && c != '='; });
}

} // namespace

void install_dns_sd(const std::shared_ptr<Bridge>& b) {
    const auto self = std::make_shared<DnsSd>(b);
    static std::shared_ptr<DnsSd> keep;
    keep = self;
    const auto define = [&](std::string_view name, transmute::GuestCallHandler handler) {
        if (!b->exported(libsystem, name)) b->function(libsystem, name, std::move(handler));
    };
    const auto set_status = [](CpuState& s, std::int32_t status) { s.x[0] = static_cast<std::uint32_t>(status); };
    const auto interface_ok = [](std::uint64_t index) {
        const auto value = static_cast<std::uint32_t>(index);
        return value != interface_index::local_only && value != interface_index::unicast && value != interface_index::p2p &&
               value != interface_index::ble;
    };

    // DNSServiceCreateConnection(DNSServiceRef *sdRef)
    define("_DNSServiceCreateConnection", [self, set_status](CpuState& s, GuestMemory& m) {
        if (!s.x[0]) { set_status(s, err::bad_param); return; }
        std::shared_ptr<Record> record;
        const auto out = s.x[0];
        const auto status = self->open_connection(record);
        if (status == err::none) m.write64(out, record->handle);
        set_status(s, status);
    });
    define("_DNSServiceRefSockFD", [self](CpuState& s, GuestMemory&) {
        const auto record = self->find(s.x[0]);
        std::int32_t fd = -1;
        if (record) { std::lock_guard io(record->connection->io); fd = record->connection->read_fd; }
        s.x[0] = static_cast<std::uint32_t>(fd);
    });
    define("_DNSServiceProcessResult", [self, set_status](CpuState& s, GuestMemory&) { set_status(s, self->process(s.x[0])); });
    define("_DNSServiceRefDeallocate", [self](CpuState& s, GuestMemory&) { self->deallocate(s.x[0]); s.x[0] = 0; });

    // DNSServiceBrowse(sdRef*, flags, interfaceIndex, regtype, domain, callBack, context)
    define("_DNSServiceBrowse", [self, set_status, interface_ok](CpuState& s, GuestMemory& m) {
        const auto out = s.x[0], flags = s.x[1], index = s.x[2], callback = s.x[5], context = s.x[6];
        const auto type = read_cstring(m, s.x[3]), domain = read_cstring(m, s.x[4]);
        if (!out || !s.x[3] || !callback) { set_status(s, err::bad_param); return; }
        if (!interface_ok(index) || !mdns::is_local_domain(domain)) { set_status(s, err::unsupported); return; }
        std::shared_ptr<Record> record;
        if (const auto status = self->make_record(out, flags, Kind::browse, callback, context, record); status != err::none) {
            set_status(s, status); return;
        }
        std::int32_t status{};
        auto operation = mdns::browse(type, static_cast<std::uint32_t>(index), self->sink(record), status);
        set_status(s, self->finish(out, record, std::move(operation), status));
    });
    // DNSServiceResolve(sdRef*, flags, interfaceIndex, name, regtype, domain, callBack, context)
    define("_DNSServiceResolve", [self, set_status, interface_ok](CpuState& s, GuestMemory& m) {
        const auto out = s.x[0], flags = s.x[1], index = s.x[2], callback = s.x[6], context = s.x[7];
        const auto name = read_cstring(m, s.x[3]), type = read_cstring(m, s.x[4]), domain = read_cstring(m, s.x[5]);
        if (!out || !s.x[3] || !s.x[4] || !callback) { set_status(s, err::bad_param); return; }
        if (!interface_ok(index) || !mdns::is_local_domain(domain)) { set_status(s, err::unsupported); return; }
        std::shared_ptr<Record> record;
        if (const auto status = self->make_record(out, flags, Kind::resolve, callback, context, record); status != err::none) {
            set_status(s, status); return;
        }
        std::int32_t status{};
        auto operation = mdns::resolve(name, type, static_cast<std::uint32_t>(index), self->sink(record), status);
        set_status(s, self->finish(out, record, std::move(operation), status));
    });
    // DNSServiceRegister(sdRef*, flags, interfaceIndex, name, regtype, domain, host, port (network order),
    //                    txtLen (stack +0, uint16), txtRecord (+8), callBack (+16), context (+24))
    define("_DNSServiceRegister", [self, set_status, interface_ok](CpuState& s, GuestMemory& m) {
        const auto out = s.x[0], flags = s.x[1], index = s.x[2];
        const auto name = read_cstring(m, s.x[3]), type = read_cstring(m, s.x[4]), domain = read_cstring(m, s.x[5]);
        const auto host = read_cstring(m, s.x[6]);
        const auto port = swap16(s.x[7]);
        const auto txt_length = static_cast<std::uint16_t>(m.read32(s.sp));
        const auto txt = m.read64(s.sp + 8), callback = m.read64(s.sp + 16), context = m.read64(s.sp + 24);
        if (!out || !s.x[4]) { set_status(s, err::bad_param); return; }
        if (!interface_ok(index) || !mdns::is_local_domain(domain) || !host.empty()) { set_status(s, err::unsupported); return; }
        const auto txt_bytes = read_bytes(m, txt, txt ? txt_length : 0);
        std::shared_ptr<Record> record;
        if (const auto status = self->make_record(out, flags, Kind::registration, callback, context, record); status != err::none) {
            set_status(s, status); return;
        }
        std::int32_t status{};
        auto operation = mdns::register_service(name, type, port, txt_bytes, static_cast<std::uint32_t>(index), self->sink(record), status);
        set_status(s, self->finish(out, record, std::move(operation), status));
    });
    // DNSServiceGetAddrInfo(sdRef*, flags, interfaceIndex, protocol, hostname, callBack, context)
    define("_DNSServiceGetAddrInfo", [self, set_status, interface_ok](CpuState& s, GuestMemory& m) {
        const auto out = s.x[0], flags = s.x[1], index = s.x[2], protocol = s.x[3], callback = s.x[5], context = s.x[6];
        const auto host = read_cstring(m, s.x[4]);
        if (!out || host.empty() || !callback || (protocol & ~std::uint64_t{3})) { set_status(s, err::bad_param); return; }
        if (!interface_ok(index)) { set_status(s, err::unsupported); return; }
        std::shared_ptr<Record> record;
        if (const auto status = self->make_record(out, flags, Kind::address, callback, context, record); status != err::none) {
            set_status(s, status); return;
        }
        std::int32_t status{};
        auto operation = mdns::get_addr_info(host, static_cast<std::uint32_t>(protocol), static_cast<std::uint32_t>(index),
                                             self->sink(record), status);
        set_status(s, self->finish(out, record, std::move(operation), status));
    });
    // DNSServiceConstructFullName(char *fullName, service, regtype, domain): 0, or -1 for bad input.
    define("_DNSServiceConstructFullName", [](CpuState& s, GuestMemory& m) {
        const auto out = s.x[0];
        const auto service = s.x[1] ? std::optional(read_cstring(m, s.x[1], 64)) : std::nullopt;
        auto type = read_cstring(m, s.x[2]), domain = read_cstring(m, s.x[3]);
        if (!out || type.empty() || domain.empty()) { s.x[0] = static_cast<std::uint32_t>(-1); return; }
        if (type.back() != '.') type += '.';
        if (domain.back() != '.') domain += '.';
        std::string full = (service ? mdns::escape_label(*service) + "." : std::string{}) + type + domain;
        if (full.size() >= 1009) { s.x[0] = static_cast<std::uint32_t>(-1); return; }   // kDNSServiceMaxDomainName
        m.write_bytes(out, std::span(reinterpret_cast<const std::uint8_t*>(full.data()), full.size()));
        m.write8(out + full.size(), 0);
        s.x[0] = 0;
    });

    // ---- TXTRecord helpers ----------------------------------------------------------------------
    define("_TXTRecordCreate", [](CpuState& s, GuestMemory& m) {
        write_ref(m, s.x[0], {s.x[2], static_cast<std::uint16_t>(s.x[2] ? s.x[1] : 0), 0, false});
        s.x[0] = 0;
    });
    define("_TXTRecordDeallocate", [b](CpuState& s, GuestMemory& m) {
        const auto ref = read_ref(m, s.x[0]);
        if (ref.malloced && ref.buffer) static_cast<void>(b->call(libsystem, "_free", {ref.buffer}));
        write_ref(m, s.x[0], {});
        s.x[0] = 0;
    });
    // TXTRecordSetValue(txtRecord*, key, valueSize (uint8), value)
    define("_TXTRecordSetValue", [b, set_status](CpuState& s, GuestMemory& m) {
        const auto key = read_cstring(m, s.x[1], 256);
        const auto value_size = static_cast<std::uint8_t>(s.x[2]);
        if (!valid_key(key)) { set_status(s, err::invalid); return; }
        const std::size_t item = key.size() + (s.x[3] ? 1 + value_size : 0);
        if (item > 255) { set_status(s, err::invalid); return; }
        auto ref = read_ref(m, s.x[0]);
        auto data = read_bytes(m, ref.buffer, ref.length);
        if (const auto at = search(data, key); at != std::string_view::npos)
            data.erase(data.begin() + static_cast<std::ptrdiff_t>(at), data.begin() + static_cast<std::ptrdiff_t>(at + 1 + data[at]));
        data.push_back(static_cast<std::uint8_t>(item));
        data.insert(data.end(), key.begin(), key.end());
        if (s.x[3]) {
            data.push_back('=');
            const auto value = read_bytes(m, s.x[3], value_size);
            data.insert(data.end(), value.begin(), value.end());
        }
        if (data.size() > 0xffff) { set_status(s, err::invalid); return; }
        if (data.size() > ref.size) {
            if (ref.buffer && !ref.malloced) { set_status(s, err::no_memory); return; }   // the caller's buffer is full
            const auto grown = b->call(libsystem, "_malloc", {data.size()});
            if (!grown) { set_status(s, err::no_memory); return; }
            if (ref.malloced && ref.buffer) static_cast<void>(b->call(libsystem, "_free", {ref.buffer}));
            ref.buffer = grown;
            ref.size = static_cast<std::uint16_t>(data.size());
            ref.malloced = true;
        }
        if (!data.empty()) m.write_bytes(ref.buffer, data);
        ref.length = static_cast<std::uint16_t>(data.size());
        write_ref(m, s.x[0], ref);
        set_status(s, err::none);
    });
    define("_TXTRecordRemoveValue", [set_status](CpuState& s, GuestMemory& m) {
        const auto key = read_cstring(m, s.x[1], 256);
        auto ref = read_ref(m, s.x[0]);
        auto data = read_bytes(m, ref.buffer, ref.length);
        const auto at = search(data, key);
        if (at == std::string_view::npos) { set_status(s, err::no_such_key); return; }
        data.erase(data.begin() + static_cast<std::ptrdiff_t>(at), data.begin() + static_cast<std::ptrdiff_t>(at + 1 + data[at]));
        if (!data.empty()) m.write_bytes(ref.buffer, data);
        ref.length = static_cast<std::uint16_t>(data.size());
        write_ref(m, s.x[0], ref);
        set_status(s, err::none);
    });
    define("_TXTRecordGetLength", [](CpuState& s, GuestMemory& m) { s.x[0] = read_ref(m, s.x[0]).length; });
    define("_TXTRecordGetBytesPtr", [](CpuState& s, GuestMemory& m) { s.x[0] = read_ref(m, s.x[0]).buffer; });
    // TXTRecordContainsKey(txtLen, txtRecord, key)
    define("_TXTRecordContainsKey", [](CpuState& s, GuestMemory& m) {
        const auto data = read_bytes(m, s.x[1], static_cast<std::uint16_t>(s.x[0]));
        s.x[0] = search(data, read_cstring(m, s.x[2], 256)) != std::string_view::npos ? 1 : 0;
    });
    // TXTRecordGetValuePtr(txtLen, txtRecord, key, uint8_t *valueLen): NULL when absent or without a value.
    define("_TXTRecordGetValuePtr", [](CpuState& s, GuestMemory& m) {
        const auto key = read_cstring(m, s.x[2], 256);
        const auto data = read_bytes(m, s.x[1], static_cast<std::uint16_t>(s.x[0]));
        const auto at = search(data, key);
        if (at == std::string_view::npos || data[at] <= key.size()) { s.x[0] = 0; return; }
        if (s.x[3]) m.write8(s.x[3], static_cast<std::uint8_t>(data[at] - key.size() - 1));
        s.x[0] = s.x[1] + at + 1 + key.size() + 1;
    });
    // TXTRecordGetCount(txtLen, txtRecord)
    define("_TXTRecordGetCount", [](CpuState& s, GuestMemory& m) {
        const auto data = read_bytes(m, s.x[1], static_cast<std::uint16_t>(s.x[0]));
        std::uint64_t count{};
        for (std::size_t at = 0; at < data.size(); at += 1 + data[at]) ++count;
        if (data.size() == 1 && data[0] == 0) count = 0;   // the empty TXT record
        s.x[0] = count;
    });
    // TXTRecordGetItemAtIndex(txtLen, txtRecord, itemIndex, keyBufLen, key, uint8_t *valueLen, const void **value)
    define("_TXTRecordGetItemAtIndex", [set_status](CpuState& s, GuestMemory& m) {
        const auto record = s.x[1];
        const auto data = read_bytes(m, record, static_cast<std::uint16_t>(s.x[0]));
        std::size_t at = 0;
        for (std::uint64_t index = 0; at < data.size() && index < s.x[2]; ++index) at += 1 + data[at];
        if (at >= data.size() || at + 1 + data[at] > data.size() || data[at] == 0) { set_status(s, err::invalid); return; }
        const std::string_view item(reinterpret_cast<const char*>(data.data() + at + 1), data[at]);
        const auto equals = item.find('=');
        const auto key = item.substr(0, equals);
        if (key.size() + 1 > static_cast<std::uint16_t>(s.x[3])) { set_status(s, err::no_memory); return; }
        m.write_bytes(s.x[4], std::span(reinterpret_cast<const std::uint8_t*>(key.data()), key.size()));
        m.write8(s.x[4] + key.size(), 0);
        if (equals == std::string_view::npos) {
            if (s.x[5]) m.write8(s.x[5], 0);
            if (s.x[6]) m.write64(s.x[6], 0);
        } else {
            if (s.x[5]) m.write8(s.x[5], static_cast<std::uint8_t>(item.size() - equals - 1));
            if (s.x[6]) m.write64(s.x[6], record + at + 1 + equals + 1);
        }
        set_status(s, err::none);
    });
}

} // namespace alchemy::gui::network
