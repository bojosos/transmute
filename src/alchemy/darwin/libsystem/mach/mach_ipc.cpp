#include "alchemy/darwin/libsystem/mach/mach_ipc.hpp"

#include "alchemy/engine_adapter.hpp"
#include "alchemy/host/process.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>

// The space and message transfer of mach_ipc.hpp. Every public operation
// takes the space lock; private helpers run with it held. Notifications are
// kernel messages enqueued while the lock is held.
namespace alchemy::libsystem::mach {
namespace {

using Clock = std::chrono::steady_clock;

// mach_port_type_t bits (<mach/port.h>): MACH_PORT_TYPE(right) = 1 << (right + 16).
constexpr std::uint32_t type_send = 1U << 16, type_receive = 1U << 17, type_send_once = 1U << 18, type_port_set = 1U << 19,
    type_dead_name = 1U << 20, type_dnrequest = 0x80000000U, type_sprequest = 0x40000000U;
// mach_port_options_t flags (MPO_*).
constexpr std::uint32_t mpo_context_as_guard = 0x01, mpo_qlimit = 0x02, mpo_tempowner = 0x04,
    mpo_importance_receiver = 0x08, mpo_insert_send_right = 0x10, mpo_strict = 0x20, mpo_denap_receiver = 0x40,
    mpo_immovable_receive = 0x80;
// mps_flags (MACH_PORT_STATUS_FLAG_*).
constexpr std::uint32_t status_tempowner = 0x01, status_guarded = 0x02, status_strict_guard = 0x04,
    status_imp_donation = 0x08, status_guard_immovable_receive = 0x40;
constexpr std::uint32_t qlimit_max = 1024, qlimit_kernel = 65534;
constexpr std::uint32_t urefs_max = 0xffff;
constexpr std::uint32_t message_limit = 64U * 1024 * 1024;
constexpr std::uint32_t complex_bit = 0x80000000U;
// Format-0 trailer sizes by MACH_RCV_TRAILER_ELEMENTS: NULL, SEQNO, SENDER,
// AUDIT, CTX, AV (and the two reserved values, which receive the AV trailer).
constexpr std::array<std::uint32_t, 8> trailer_sizes{8, 12, 20, 52, 60, 68, 68, 68};
// Descriptor types (mach_msg_descriptor_type_t).
constexpr std::uint8_t port_descriptor = 0, ool_descriptor = 1, ool_ports_descriptor = 2, ool_volatile_descriptor = 3,
    guarded_port_descriptor = 4;

std::uint32_t get32(const std::vector<std::uint8_t>& bytes, std::size_t at) {
    std::uint32_t value{};
    std::memcpy(&value, bytes.data() + at, 4);
    return value;
}
std::uint64_t get64(const std::vector<std::uint8_t>& bytes, std::size_t at) {
    std::uint64_t value{};
    std::memcpy(&value, bytes.data() + at, 8);
    return value;
}
void put32(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint32_t value) { std::memcpy(bytes.data() + at, &value, 4); }
void put64(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint64_t value) { std::memcpy(bytes.data() + at, &value, 8); }

bool send_disposition(std::uint8_t value) { return value >= disp::move_send && value <= disp::make_send_once; }
bool valid_disposition(std::uint8_t value) { return value >= disp::move_receive && value <= disp::make_send_once; }
std::uint8_t received_type(std::uint8_t disposition) {
    switch (disposition) {
    case disp::move_receive: return disp::port_receive;
    case disp::move_send_once: case disp::make_send_once: return disp::port_send_once;
    default: return disp::port_send;
    }
}

[[noreturn]] void guard_violation(const char* operation, Name name) {
    throw std::runtime_error(std::string("Alchemy: Mach port guard violation (") + operation + " of guarded port 0x" +
                             [&] { char text[16]; std::snprintf(text, sizeof(text), "%x", name); return std::string(text); }() +
                             "); macOS raises EXC_GUARD and ends the process");
}

} // namespace

// Snapshot of everything a message copy-in may change, restored when a
// later right in the same message is invalid (Mach copies in a message
// atomically). Names a copy-in releases are recycled only on commit.
struct MachIpc::Transaction {
    struct PortState {
        Name receiver{}, send_name{};
        std::set<Name> send_once_names;
        std::uint32_t srights{}, sorights{}, mscount{};
        std::vector<PortSet*> sets;
    };
    std::map<Name, std::optional<Entry>> entries;
    std::map<std::shared_ptr<Port>, PortState> ports;
    std::map<PortSet*, std::vector<std::shared_ptr<Port>>> sets;
    std::vector<std::pair<Name, Entry>> vanished;

    void save(Name name, const std::optional<Entry>& entry) { entries.try_emplace(name, entry); }
    void save(const std::shared_ptr<Port>& port) {
        if (!port || ports.contains(port)) return;
        ports.emplace(port, PortState{port->receiver, port->send_name, port->send_once_names, port->srights, port->sorights,
                                      port->mscount, port->sets});
        for (auto* set : port->sets) sets.try_emplace(set, set->members);
    }
};

// ---- names ---------------------------------------------------------------------

Entry* MachIpc::find(Name name) {
    const auto found = entries_.find(name);
    return found == entries_.end() ? nullptr : &found->second;
}

Name MachIpc::new_name() {
    for (;;) {
        Name name{};
        if (!free_indices_.empty()) {
            const auto [index, generation] = free_indices_.back();
            free_indices_.pop_back();
            const auto next = static_cast<std::uint8_t>(generation + 4 == 0 ? 4 : generation + 4);
            name = (index << 8) | next;
        } else {
            if (next_index_ > 0xffffffU) throw std::runtime_error("Alchemy: the Mach port name space is exhausted");
            name = (next_index_++ << 8) | 4U;
        }
        if (!entries_.contains(name)) return name;
    }
}

void MachIpc::erase(Name name) {
    const auto found = entries_.find(name);
    if (found == entries_.end()) return;
    if (const auto& port = found->second.port) {
        if (port->send_name == name) port->send_name = 0;
        port->send_once_names.erase(name);
        if (port->receiver == name) port->receiver = 0;
    }
    entries_.erase(found);
    if (allocated_name(name)) free_indices_.emplace_back(name >> 8, static_cast<std::uint8_t>(name & 0xff));
}

std::shared_ptr<Port> MachIpc::new_port() {
    auto port = std::make_shared<Port>();
    port->id = ++port_ids_;
    return port;
}

std::shared_ptr<Port> MachIpc::kernel_port(Name name) {
    auto& slot = kernel_ports_[name];
    if (!slot) {
        slot = new_port();
        slot->kernel_name = name;
    }
    return slot;
}

void MachIpc::touch() {
    ++epoch_;
    changed_.notify_all();
}

std::size_t MachIpc::live_names() {
    std::lock_guard lock(mutex_);
    return entries_.size();
}

// ---- rights in transit -----------------------------------------------------------

void MachIpc::enqueue(const std::shared_ptr<Port>& port, std::shared_ptr<Message> message) {
    message->stamp = ++stamp_;
    message->queued = std::chrono::steady_clock::now();
    port->queue.push_back(std::move(message));
    touch();
}

void MachIpc::drop_send(const std::shared_ptr<Port>& port) {
    if (port->srights) --port->srights;
    senders_changed(port);
}

// No send right is left here or in another process: the no-senders
// notification; a kernel object without rights goes away; a proxy without
// rights returns its units to its host.
void MachIpc::senders_changed(const std::shared_ptr<Port>& port) {
    if (port->srights || !port->holders.empty()) return;
    if (port->host) { retire_proxy(port); return; }
    if (port->nsrequest && port->active) {
        const auto request = *port->nsrequest;
        port->nsrequest.reset();
        notify(request, notify::no_senders, port->mscount);
    }
    if (port->kobject && !port->sorights) {
        port->active = false;
        port->kobject.reset();
    }
}

void MachIpc::drop_send_once(const std::shared_ptr<Port>& port) {
    if (port->sorights) --port->sorights;
    if (port->host) { retire_proxy(port); return; }
    if (port->kobject && !port->srights && !port->sorights) {
        port->active = false;
        port->kobject.reset();
    }
}

// Destroys a right outside the space. A destroyed send-once right sends a
// send-once notification to its port; a destroyed receive right destroys the
// port.
void MachIpc::release(Right right) {
    if (!right.port) return;
    const auto port = right.port;
    switch (right.type) {
    case disp::port_send: drop_send(port); break;
    case disp::port_send_once:
        if (port->host) {
            // Another process's port: its host sends the send-once notification.
            if (port->active && transport_) {
                Writer frame;
                frame.u32(port->key.pid);
                frame.u64(port->key.id);
                static_cast<void>(transport_->post(port->host, 3, std::move(frame.bytes)));   // send-once destroyed
                if (port->remote_once) --port->remote_once;
            }
            drop_send_once(port);
        } else if (port->active && !port->kernel_name && !port->kobject) notify(right, notify::send_once, 0);
        else drop_send_once(port);
        break;
    case disp::port_receive: destroy_receive(port); break;
    default: break;
    }
}

// The rights a message holds, released when it is destroyed unreceived. Its
// destination right only goes away (no notification about itself).
void MachIpc::discard(Message& message) {
    if (const auto port = message.destination.port) {
        if (message.destination.type == disp::port_send_once) drop_send_once(port);
        else drop_send(port);
    }
    message.destination = {};
    release(std::exchange(message.reply, {}));
    for (auto& descriptor : message.descriptors)
        for (auto& carried : descriptor.rights) release(std::exchange(carried, {}));
}

// A kernel message to the send-once right `target`: header only (send-once),
// header + NDR + one word (dead-name, no-senders, port-deleted,
// send-possible) or a complex message carrying a receive right
// (port-destroyed).
void MachIpc::notify(Right target, std::int32_t id, std::uint32_t value, Right carried) {
    if (!target.port) { release(carried); return; }
    auto message = std::make_shared<Message>();
    message->kernel = true;
    message->destination = target;
    if (carried.port) {
        message->bytes.assign(40, 0);
        put32(message->bytes, 0, complex_bit);
        put32(message->bytes, 24, 1);
        message->bytes[28 + 10] = disp::move_receive;
        Descriptor descriptor;
        descriptor.offset = 28;
        descriptor.type = port_descriptor;
        descriptor.received = disp::port_receive;
        descriptor.rights.push_back(std::move(carried));
        message->descriptors.push_back(std::move(descriptor));
    } else if (id == notify::send_once) {
        message->bytes.assign(24, 0);
    } else {
        message->bytes.assign(36, 0);
        message->bytes[24 + 4] = 1;   // NDR_record: little-endian integers, ASCII, IEEE floats
        put32(message->bytes, 32, value);
    }
    put32(message->bytes, 4, static_cast<std::uint32_t>(message->bytes.size()));
    put32(message->bytes, 20, static_cast<std::uint32_t>(id));
    const auto port = target.port;
    if (port->host && port->active && !carried.port && message->descriptors.empty()) {
        post_notification(port, std::move(message->bytes));   // a notify port in another process
        return;
    }
    if (port->active && !port->host && !port->kernel_name && !port->kobject && port->queue.size() < qlimit_kernel) enqueue(port, message);
    else discard(*message);
}

void MachIpc::send_possible(Port& port) {
    std::vector<Name> names(port.send_once_names.begin(), port.send_once_names.end());
    if (port.send_name) names.push_back(port.send_name);
    for (const auto name : names) {
        auto* entry = find(name);
        if (!entry || !entry->sprequest) continue;
        const auto request = *entry->sprequest;
        entry->sprequest.reset();
        notify(request, notify::send_possible, name);
    }
}

// A name stopped denoting a right other than through the port's death: its
// dead-name and send-possible requests receive port-deleted notifications.
void MachIpc::entry_vanished(Name name, Entry& entry) {
    if (entry.dnrequest) notify(*std::exchange(entry.dnrequest, std::nullopt), notify::port_deleted, name);
    if (entry.sprequest) notify(*std::exchange(entry.sprequest, std::nullopt), notify::port_deleted, name);
}

void MachIpc::leave_sets(Port& port) {
    for (auto* set : port.sets)
        std::erase_if(set->members, [&](const std::shared_ptr<Port>& member) { return member.get() == &port; });
    port.sets.clear();
}

void MachIpc::destroy_set(const std::shared_ptr<PortSet>& set) {
    for (const auto& member : set->members) std::erase(member->sets, set.get());
    set->members.clear();
}

// The receive right of `port` is destroyed (its name already removed from
// the space). With a port-destroyed request the right goes to the notify
// port instead; otherwise the port dies: queued messages are destroyed and
// every name for it becomes a dead name (with dead-name notifications).
void MachIpc::destroy_receive(const std::shared_ptr<Port>& port) {
    if (port->receiver) {
        const auto name = port->receiver;
        port->receiver = 0;
        if (auto* entry = find(name)) {
            entry->receive = false;
            if (entry->empty()) erase(name);
        }
    }
    leave_sets(*port);
    if (port->pdrequest && port->pdrequest->port && port->pdrequest->port->active) {
        const auto request = *port->pdrequest;
        port->pdrequest.reset();
        notify(request, notify::port_destroyed, 0, Right{port, disp::port_receive});
        touch();
        return;
    }
    kill_port(port);
}

// The port is gone (its receive right destroyed, or the process hosting it
// died): queued messages are destroyed, every name for it becomes a dead name
// (with dead-name notifications), and processes holding rights to it learn it.
void MachIpc::kill_port(std::shared_ptr<Port> port) {
    // ALCHEMY_TRACE_PROCESS=1: ports that die while other processes know them.
    static const bool trace = [] { const auto value = host::process::environment("ALCHEMY_TRACE_PROCESS"); return value && value->starts_with("1"); }();
    if (trace && port->active && (port->host || !port->holders.empty() || !port->once_holders.empty())) {
        std::string told;
        for (const auto& [pid, units] : port->holders) told += " " + std::to_string(pid);
        for (const auto& [pid, units] : port->once_holders) told += " " + std::to_string(pid) + "(once)";
        std::fprintf(stderr, "[alchemy mach] %u: port %u:%llu %s%s\n", self_, port->key.pid, static_cast<unsigned long long>(port->key.id),
                     port->host ? "of another process dies here" : "dies; holders:", told.c_str());
    }
    port->active = false;
    leave_sets(*port);
    if (port->nsrequest) release(*std::exchange(port->nsrequest, std::nullopt));
    if (port->pdrequest) release(*std::exchange(port->pdrequest, std::nullopt));
    if (transport_ && port->key.pid) {
        std::set<std::uint32_t> told;
        for (const auto& [pid, units] : port->holders) { static_cast<void>(units); told.insert(pid); }
        for (const auto& [pid, units] : port->once_holders) { static_cast<void>(units); told.insert(pid); }
        for (const auto pid : told) {
            Writer frame;
            frame.u32(port->key.pid);
            frame.u64(port->key.id);
            static_cast<void>(transport_->post(pid, 4, std::move(frame.bytes)));   // dead
        }
        port->holders.clear();
        port->once_holders.clear();
        if (const auto found = hosted_.find(port->key); found != hosted_.end() && found->second == port) hosted_.erase(found);
    }
    if (port->host) {
        if (const auto found = proxies_.find({port->host, port->key}); found != proxies_.end() && found->second == port) proxies_.erase(found);
        port->remote_refs = port->remote_once = 0;
    }
    auto queue = std::move(port->queue);
    port->queue.clear();
    for (auto& message : queue) discard(*message);
    std::vector<Name> names(port->send_once_names.begin(), port->send_once_names.end());
    if (port->send_name) names.push_back(port->send_name);
    for (const auto name : names) {
        auto* entry = find(name);
        if (!entry || entry->port != port) continue;
        entry->dead += entry->send + (entry->send_once ? 1U : 0U);
        entry->send = 0;
        entry->send_once = false;
        entry->receive = false;
        for (auto* request : {&entry->dnrequest, &entry->sprequest}) {
            if (!*request) continue;
            const auto target = **request;
            request->reset();
            ++entry->dead;   // the notification carries a user reference of the dead name
            notify(target, notify::dead_name, name);
            entry = find(name);
            if (!entry) break;
        }
        if (entry && entry->empty()) erase(name);
    }
    port->send_name = 0;
    port->send_once_names.clear();
    touch();
}

// Copies a right out of the space for a message or a held right (`out`).
std::uint32_t MachIpc::copyin(Name name, std::uint8_t disposition, Transaction* transaction, Right& out) {
    out = {};
    if (!valid_disposition(disposition)) return kern::invalid_value;
    if (name == port_null) return kern::success;
    if (name == port_dead) {
        if (disposition == disp::make_send || disposition == disp::make_send_once || disposition == disp::move_receive)
            return kern::invalid_right;
        out = Right{nullptr, received_type(disposition), true};
        return kern::success;
    }
    auto* entry = find(name);
    if (!entry) {
        if (!allocated_name(name) && guest_.kernel_name && guest_.kernel_name(name) &&
            (disposition == disp::copy_send || disposition == disp::move_send)) {
            const auto port = kernel_port(name);
            if (transaction) transaction->save(port);
            ++port->srights;
            out = Right{port, disp::port_send};
            return kern::success;
        }
        return kern::invalid_name;
    }
    const auto port = entry->port;
    const auto save = [&] {
        if (!transaction) return;
        transaction->save(name, *entry);
        transaction->save(port);
    };
    const auto vanish = [&] {
        if (!entry->empty()) return;
        if (transaction) {
            transaction->vanished.emplace_back(name, *entry);
            if (port) {
                if (port->send_name == name) port->send_name = 0;
                port->send_once_names.erase(name);
            }
            entries_.erase(name);
        } else {
            entry_vanished(name, *entry);
            erase(name);
        }
    };
    switch (disposition) {
    case disp::move_receive:
        if (!entry->receive) return kern::invalid_right;
        if (port->immovable) return kern::invalid_right;
        save();
        port->receiver = 0;
        leave_sets(*port);
        entry->receive = false;
        out = Right{port, disp::port_receive};
        vanish();
        return kern::success;
    case disp::copy_send:
        if (entry->send) {
            save();
            ++port->srights;
            out = Right{port, disp::port_send};
            return kern::success;
        }
        if (entry->dead) { out = Right{nullptr, disp::port_send, true}; return kern::success; }
        return kern::invalid_right;
    case disp::move_send:
        if (entry->send) {
            save();
            if (--entry->send) ++port->srights;   // the name keeps a right: one more exists
            out = Right{port, disp::port_send};
            vanish();
            return kern::success;
        }
        if (entry->dead) {
            save();
            --entry->dead;
            out = Right{nullptr, disp::port_send, true};
            vanish();
            return kern::success;
        }
        return kern::invalid_right;
    case disp::make_send:
        if (!entry->receive) return kern::invalid_right;
        save();
        ++port->mscount;
        ++port->srights;
        out = Right{port, disp::port_send};
        return kern::success;
    case disp::make_send_once:
        if (!entry->receive) return kern::invalid_right;
        save();
        ++port->sorights;
        out = Right{port, disp::port_send_once};
        return kern::success;
    case disp::move_send_once:
        if (entry->send_once) {
            save();
            entry->send_once = false;
            out = Right{port, disp::port_send_once};
            vanish();
            return kern::success;
        }
        if (entry->dead) {
            save();
            --entry->dead;
            out = Right{nullptr, disp::port_send_once, true};
            vanish();
            return kern::success;
        }
        return kern::invalid_right;
    default:
        return kern::invalid_value;
    }
}

// Inserts a right into the space: send rights merge into the port's name.
Name MachIpc::copyout(Right right) {
    if (!right.port) return right.dead ? port_dead : port_null;
    const auto port = right.port;
    if (port->kernel_name) {
        if (right.type == disp::port_send) drop_send(port);
        return port->kernel_name;
    }
    if (!port->active) {
        if (right.type == disp::port_send_once) drop_send_once(port);
        else if (right.type == disp::port_send) drop_send(port);
        return port_dead;
    }
    switch (right.type) {
    case disp::port_receive: {
        if (port->send_name) {
            entries_.at(port->send_name).receive = true;
            port->receiver = port->send_name;
            return port->send_name;
        }
        const auto name = new_name();
        auto& entry = entries_[name];
        entry.port = port;
        entry.receive = true;
        port->receiver = port->send_name = name;
        return name;
    }
    case disp::port_send: {
        if (port->send_name) {
            auto& entry = entries_.at(port->send_name);
            if (entry.send) --port->srights;   // merged into the name's existing right
            entry.send = std::min(entry.send + 1, urefs_max);
            return port->send_name;
        }
        const auto name = new_name();
        auto& entry = entries_[name];
        entry.port = port;
        entry.send = 1;
        port->send_name = name;
        return name;
    }
    case disp::port_send_once: {
        const auto name = new_name();
        auto& entry = entries_[name];
        entry.port = port;
        entry.send_once = true;
        port->send_once_names.insert(name);
        return name;
    }
    default:
        throw std::logic_error("Alchemy: copyout of an invalid Mach right type");
    }
}

// ---- names and rights ----------------------------------------------------------------

std::uint32_t MachIpc::allocate(std::uint32_t right_kind, Name& name) {
    std::lock_guard lock(mutex_);
    name = new_name();
    auto& entry = entries_[name];
    switch (right_kind) {
    case right::receive: {
        const auto port = new_port();
        entry.port = port;
        entry.receive = true;
        port->receiver = port->send_name = name;
        break;
    }
    case right::port_set:
        entry.set = std::make_shared<PortSet>();
        entry.set->name = name;
        break;
    case right::dead_name:
        entry.dead = 1;
        break;
    default:
        entries_.erase(name);
        name = 0;
        return kern::invalid_value;
    }
    return kern::success;
}

std::uint32_t MachIpc::allocate_name(std::uint32_t right_kind, Name name) {
    std::lock_guard lock(mutex_);
    if (name == port_null || name == port_dead) return kern::invalid_value;
    if (right_kind != right::receive && right_kind != right::port_set && right_kind != right::dead_name) return kern::invalid_value;
    if (entries_.contains(name) || (!allocated_name(name) && guest_.kernel_name && guest_.kernel_name(name))) return kern::name_exists;
    auto& entry = entries_[name];
    if (right_kind == right::receive) {
        const auto port = new_port();
        entry.port = port;
        entry.receive = true;
        port->receiver = port->send_name = name;
    } else if (right_kind == right::port_set) {
        entry.set = std::make_shared<PortSet>();
        entry.set->name = name;
    } else {
        entry.dead = 1;
    }
    return kern::success;
}

std::uint32_t MachIpc::construct(const Options& options, std::uint64_t context, Name& name) {
    std::lock_guard lock(mutex_);
    if ((options.flags & mpo_qlimit) && options.qlimit > qlimit_max) return kern::invalid_value;
    const auto port = new_port();
    name = new_name();
    auto& entry = entries_[name];
    entry.port = port;
    entry.receive = true;
    port->receiver = port->send_name = name;
    if (options.flags & mpo_qlimit) port->qlimit = options.qlimit;
    if (options.flags & mpo_insert_send_right) {
        entry.send = 1;
        port->srights = 1;
        port->mscount = 1;
    }
    port->context = context;
    if (options.flags & mpo_context_as_guard) {
        port->guarded = true;
        port->strict = (options.flags & mpo_strict) != 0;
    }
    port->immovable = (options.flags & mpo_immovable_receive) != 0;
    if (options.flags & mpo_tempowner) port->status_flags |= status_tempowner;
    if (options.flags & (mpo_importance_receiver | mpo_denap_receiver)) port->status_flags |= status_imp_donation;
    return kern::success;
}

std::uint32_t MachIpc::destruct(Name name, std::int32_t srdelta, std::uint64_t guard_value) {
    std::lock_guard lock(mutex_);
    auto* entry = find(name);
    if (!entry) return kern::invalid_name;
    if (!entry->receive) return kern::invalid_right;
    const auto port = entry->port;
    if (port->guarded && port->context != guard_value) guard_violation("mach_port_destruct", name);
    if (srdelta > 0 || (srdelta < 0 && entry->send < static_cast<std::uint32_t>(-static_cast<std::int64_t>(srdelta))))
        return kern::invalid_value;
    if (srdelta < 0) {
        entry->send -= static_cast<std::uint32_t>(-static_cast<std::int64_t>(srdelta));
        if (!entry->send) drop_send(port);
    }
    destroy_receive(port);
    return kern::success;
}

// Releases `count` user references of the name's send right.
std::uint32_t MachIpc::remove_send(Name name, Entry& entry, std::uint32_t count) {
    entry.send -= count;
    if (entry.send) return kern::success;
    const auto port = entry.port;
    if (entry.empty()) {
        Entry copy = entry;
        erase(name);
        entry_vanished(name, copy);
    }
    drop_send(port);
    return kern::success;
}

std::uint32_t MachIpc::deallocate(Name name) {
    std::lock_guard lock(mutex_);
    // MACH_PORT_NULL and MACH_PORT_DEAD (a right whose port died before it
    // was received) are not names: nothing to release (as Darwin).
    if (name == port_null || name == port_dead) return kern::success;
    auto* entry = find(name);
    if (!entry) return !allocated_name(name) && guest_.kernel_name && guest_.kernel_name(name) ? kern::success : kern::invalid_name;
    if (entry->send) { remove_send(name, *entry, 1); touch(); return kern::success; }
    if (entry->send_once) {
        Entry copy = *entry;
        erase(name);
        entry_vanished(name, copy);
        release(Right{copy.port, disp::port_send_once});
        touch();
        return kern::success;
    }
    if (entry->dead) {
        if (!--entry->dead && entry->empty()) erase(name);
        return kern::success;
    }
    return kern::invalid_right;
}

std::uint32_t MachIpc::destroy(Name name) {
    std::lock_guard lock(mutex_);
    if (name == port_null || name == port_dead) return kern::success;
    auto* entry = find(name);
    if (!entry) return !allocated_name(name) && guest_.kernel_name && guest_.kernel_name(name) ? kern::success : kern::invalid_name;
    Entry copy = *entry;
    if (copy.receive && copy.port->guarded) guard_violation("mach_port_destroy", name);
    erase(name);
    if (copy.set) destroy_set(copy.set);
    if (copy.send_once) release(Right{copy.port, disp::port_send_once});
    if (copy.send) drop_send(copy.port);
    if (copy.receive) {
        copy.port->receiver = 0;
        destroy_receive(copy.port);
    }
    if (copy.dnrequest) release(*copy.dnrequest);
    if (copy.sprequest) release(*copy.sprequest);
    touch();
    return kern::success;
}

std::uint32_t MachIpc::mod_refs(Name name, std::uint32_t right_kind, std::int32_t delta) {
    std::lock_guard lock(mutex_);
    if (right_kind > right::dead_name) return kern::invalid_value;
    if (name == port_null || name == port_dead)
        return right_kind == right::send || right_kind == right::send_once ? kern::success : kern::invalid_value;
    auto* entry = find(name);
    if (!entry) {
        if (!allocated_name(name) && guest_.kernel_name && guest_.kernel_name(name))
            return right_kind == right::send ? kern::success : kern::invalid_right;
        return kern::invalid_name;
    }
    const auto port = entry->port;
    switch (right_kind) {
    case right::send: {
        if (!entry->send) return kern::invalid_right;
        const auto updated = static_cast<std::int64_t>(entry->send) + delta;
        if (updated < 0) return kern::invalid_value;
        if (updated > urefs_max) return kern::urefs_overflow;
        if (delta < 0) remove_send(name, *entry, static_cast<std::uint32_t>(-static_cast<std::int64_t>(delta)));
        else entry->send = static_cast<std::uint32_t>(updated);
        break;
    }
    case right::receive:
        if (!entry->receive) return kern::invalid_right;
        if (delta == 0) return kern::success;
        if (delta != -1) return kern::invalid_value;
        if (port->guarded) guard_violation("mach_port_mod_refs", name);
        destroy_receive(port);
        break;
    case right::send_once:
        if (!entry->send_once) return kern::invalid_right;
        if (delta == 0) return kern::success;
        if (delta != -1) return kern::invalid_value;
        {
            Entry copy = *entry;
            erase(name);
            entry_vanished(name, copy);
            release(Right{copy.port, disp::port_send_once});
        }
        break;
    case right::port_set:
        if (!entry->set) return kern::invalid_right;
        if (delta == 0) return kern::success;
        if (delta != -1) return kern::invalid_value;
        {
            const auto set = entry->set;
            erase(name);
            destroy_set(set);
        }
        break;
    case right::dead_name: {
        if (!entry->dead) return kern::invalid_right;
        const auto updated = static_cast<std::int64_t>(entry->dead) + delta;
        if (updated < 0) return kern::invalid_value;
        if (updated > urefs_max) return kern::urefs_overflow;
        entry->dead = static_cast<std::uint32_t>(updated);
        if (entry->empty()) erase(name);
        break;
    }
    default:
        return kern::invalid_value;
    }
    touch();
    return kern::success;
}

std::uint32_t MachIpc::get_refs(Name name, std::uint32_t right_kind, std::uint32_t& refs) {
    std::lock_guard lock(mutex_);
    refs = 0;
    if (right_kind > right::dead_name) return kern::invalid_value;
    if (name == port_null || name == port_dead) {
        if (right_kind != right::send && right_kind != right::send_once) return kern::invalid_name;
        refs = 1;
        return kern::success;
    }
    const auto* entry = find(name);
    if (!entry) {
        if (!allocated_name(name) && guest_.kernel_name && guest_.kernel_name(name)) {
            refs = right_kind == right::send ? 1 : 0;
            return kern::success;
        }
        return kern::invalid_name;
    }
    switch (right_kind) {
    case right::send: refs = entry->send; break;
    case right::receive: refs = entry->receive ? 1 : 0; break;
    case right::send_once: refs = entry->send_once ? 1 : 0; break;
    case right::port_set: refs = entry->set ? 1 : 0; break;
    case right::dead_name: refs = entry->dead; break;
    default: break;
    }
    return kern::success;
}

std::uint32_t MachIpc::type(Name name, std::uint32_t& result) {
    std::lock_guard lock(mutex_);
    result = 0;
    if (name == port_null || name == port_dead) return kern::invalid_name;
    const auto* entry = find(name);
    if (!entry) {
        if (!allocated_name(name) && guest_.kernel_name && guest_.kernel_name(name)) { result = type_send; return kern::success; }
        return kern::invalid_name;
    }
    if (entry->send) result |= type_send;
    if (entry->receive) result |= type_receive;
    if (entry->send_once) result |= type_send_once;
    if (entry->set) result |= type_port_set;
    if (entry->dead) result |= type_dead_name;
    if (entry->dnrequest) result |= type_dnrequest;
    if (entry->sprequest) result |= type_sprequest;
    return kern::success;
}

std::uint32_t MachIpc::insert_right(Name name, Name poly, std::uint8_t disposition) {
    std::lock_guard lock(mutex_);
    if (name == port_null || name == port_dead) return kern::invalid_value;
    if (!valid_disposition(disposition)) return kern::invalid_value;
    if (poly == port_null || poly == port_dead) return kern::invalid_capability;
    // The name must be free or already denote the same port (send and
    // receive rights share one name); check before anything moves.
    const auto* source = find(poly);
    if (!source) {
        if (!allocated_name(poly) && guest_.kernel_name && guest_.kernel_name(poly) &&
            (disposition == disp::copy_send || disposition == disp::move_send))
            return name == poly ? kern::success : kern::invalid_value;
        return kern::invalid_name;
    }
    const auto port = source->port;
    if (!port) return kern::invalid_right;
    const auto kind = received_type(disposition);
    if (const auto* target = find(name)) {
        if (kind == disp::port_send_once || target->port != port || target->dead || target->set) return kern::name_exists;
    } else if (!allocated_name(name) && guest_.kernel_name && guest_.kernel_name(name)) {
        return kern::name_exists;
    } else if (kind != disp::port_send_once && port->send_name && port->send_name != name) {
        return kern::right_exists;
    }
    Right moved;
    if (const auto error = copyin(poly, disposition, nullptr, moved); error) return error;
    if (!moved.port || !moved.port->active) {
        // A dead right: the name becomes a dead name.
        entries_[name].dead += 1;
        touch();
        return kern::success;
    }
    auto& entry = entries_[name];
    entry.port = port;
    switch (kind) {
    case disp::port_receive:
        entry.receive = true;
        port->receiver = port->send_name = name;
        break;
    case disp::port_send:
        if (entry.send) drop_send(port);
        entry.send = std::min(entry.send + 1, urefs_max);
        port->send_name = name;
        break;
    default:
        entry.send_once = true;
        port->send_once_names.insert(name);
        break;
    }
    touch();
    return kern::success;
}

std::uint32_t MachIpc::extract_right(Name name, std::uint8_t disposition, Name& out, std::uint8_t& kind) {
    std::lock_guard lock(mutex_);
    out = 0;
    kind = 0;
    if (!valid_disposition(disposition)) return kern::invalid_value;
    Right moved;
    if (const auto error = copyin(name, disposition, nullptr, moved); error) return error;
    if (!moved.port && !moved.dead) return kern::invalid_name;
    kind = moved.type;
    out = copyout(std::move(moved));
    touch();
    return kern::success;
}

std::uint32_t MachIpc::move_member(Name member, Name after) {
    std::lock_guard lock(mutex_);
    auto* entry = find(member);
    if (!entry) return kern::invalid_name;
    if (!entry->receive) return kern::invalid_right;
    std::shared_ptr<PortSet> set;
    if (after != port_null) {
        auto* target = find(after);
        if (!target) return kern::invalid_name;
        if (!target->set) return kern::invalid_right;
        set = target->set;
    }
    leave_sets(*entry->port);
    if (set) {
        set->members.push_back(entry->port);
        entry->port->sets.push_back(set.get());
    }
    touch();
    return kern::success;
}

std::uint32_t MachIpc::insert_member(Name member, Name set_name) {
    std::lock_guard lock(mutex_);
    auto* entry = find(member);
    auto* target = find(set_name);
    if (!entry || !target) return kern::invalid_name;
    if (!entry->receive || !target->set) return kern::invalid_right;
    const auto& port = entry->port;
    if (std::find(port->sets.begin(), port->sets.end(), target->set.get()) != port->sets.end()) return kern::already_in_set;
    target->set->members.push_back(port);
    port->sets.push_back(target->set.get());
    touch();
    return kern::success;
}

std::uint32_t MachIpc::extract_member(Name member, Name set_name) {
    std::lock_guard lock(mutex_);
    auto* entry = find(member);
    auto* target = find(set_name);
    if (!entry || !target) return kern::invalid_name;
    if (!entry->receive || !target->set) return kern::invalid_right;
    const auto& port = entry->port;
    const auto found = std::find(port->sets.begin(), port->sets.end(), target->set.get());
    if (found == port->sets.end()) return kern::not_in_set;
    port->sets.erase(found);
    std::erase(target->set->members, port);
    touch();
    return kern::success;
}

std::uint32_t MachIpc::set_status(Name set_name, std::vector<Name>& members) {
    std::lock_guard lock(mutex_);
    members.clear();
    const auto* entry = find(set_name);
    if (!entry) return kern::invalid_name;
    if (!entry->set) return kern::invalid_right;
    for (const auto& port : entry->set->members) members.push_back(port->receiver);
    return kern::success;
}

std::uint32_t MachIpc::names(std::vector<std::pair<Name, std::uint32_t>>& out) {
    out.clear();
    std::vector<Name> list;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [name, entry] : entries_) {
            static_cast<void>(entry);
            list.push_back(name);
        }
    }
    for (const auto name : list) {
        std::uint32_t bits{};
        if (type(name, bits) == kern::success) out.emplace_back(name, bits);
    }
    return kern::success;
}

std::uint32_t MachIpc::get_attributes(Name name, std::uint32_t flavor, std::uint32_t capacity, std::vector<std::uint32_t>& out) {
    std::lock_guard lock(mutex_);
    out.clear();
    const auto* entry = find(name);
    if (!entry) return kern::invalid_name;
    if (!entry->receive) return kern::invalid_right;
    const auto& port = *entry->port;
    const auto status = [&] {
        std::vector<std::uint32_t> words{
            port.sets.empty() ? 0U : port.sets.front()->name, port.seqno, port.mscount, port.qlimit,
            static_cast<std::uint32_t>(port.queue.size()), port.sorights, port.srights || !port.holders.empty() ? 1U : 0U,
            port.pdrequest ? 1U : 0U, port.nsrequest ? 1U : 0U,
            port.status_flags | (port.guarded ? status_guarded : 0U) | (port.strict ? status_strict_guard : 0U) |
                (port.immovable ? status_guard_immovable_receive : 0U)};
        return words;
    };
    switch (flavor) {
    case 1:   // MACH_PORT_LIMITS_INFO
        if (capacity < 1) return kern::failure;
        out = {port.qlimit};
        return kern::success;
    case 2:   // MACH_PORT_RECEIVE_STATUS
        if (capacity < 10) return kern::failure;
        out = status();
        return kern::success;
    case 3:   // MACH_PORT_DNREQUESTS_SIZE
        if (capacity < 1) return kern::failure;
        out = {0};
        return kern::success;
    case 7:   // MACH_PORT_INFO_EXT: status, mpie_boost_cnt, reserved[6]
        if (capacity < 17) return kern::failure;
        out = status();
        out.resize(17, 0);
        return kern::success;
    case 8:   // MACH_PORT_GUARD_INFO: uint64_t mpgi_guard
        if (capacity < 2) return kern::failure;
        out = {static_cast<std::uint32_t>(port.guarded ? port.context : 0), static_cast<std::uint32_t>((port.guarded ? port.context : 0) >> 32)};
        return kern::success;
    default:
        return kern::invalid_argument;
    }
}

std::uint32_t MachIpc::set_attributes(Name name, std::uint32_t flavor, const std::vector<std::uint32_t>& info) {
    std::lock_guard lock(mutex_);
    auto* entry = find(name);
    if (!entry) return kern::invalid_name;
    if (!entry->receive) return kern::invalid_right;
    auto& port = *entry->port;
    switch (flavor) {
    case 1:   // MACH_PORT_LIMITS_INFO
        if (info.empty()) return kern::failure;
        if (info[0] > qlimit_max) return kern::invalid_value;
        port.qlimit = info[0];
        if (port.queue.size() < port.qlimit) send_possible(port);
        touch();
        return kern::success;
    case 3:   // MACH_PORT_DNREQUESTS_SIZE: a table-size hint
        return kern::success;
    // Importance donation and App Nap are scheduling hints Alchemy does not
    // model; the flags are recorded and reported in the status.
    case 4: port.status_flags |= status_tempowner; return kern::success;      // MACH_PORT_TEMPOWNER
    case 5: case 6: port.status_flags |= status_imp_donation; return kern::success;  // IMPORTANCE_RECEIVER, DENAP_RECEIVER
    default:
        return kern::invalid_argument;
    }
}

std::uint32_t MachIpc::request_notification(Name name, std::int32_t id, std::uint32_t sync, Name notify_name,
                                            std::uint8_t disposition, Name& previous) {
    std::lock_guard lock(mutex_);
    previous = 0;
    if (notify_name != port_null && disposition != disp::make_send_once && disposition != disp::move_send_once)
        return kern::invalid_value;
    auto* entry = find(name);
    if (!entry) return name == port_null || name == port_dead ? kern::invalid_argument : kern::invalid_name;
    std::optional<Right>* slot = nullptr;
    bool immediate = false;
    std::uint32_t value = 0;
    std::int32_t immediate_id = id;
    switch (id) {
    case notify::dead_name:
        if (entry->dead && !entry->send && !entry->send_once) {
            if (!sync) return kern::invalid_argument;
            immediate = true;
            value = name;
        } else if (!entry->send && !entry->send_once && !entry->receive) {
            return kern::invalid_right;
        }
        slot = &entry->dnrequest;
        break;
    case notify::send_possible:
        if (entry->dead && !entry->send && !entry->send_once) {
            if (!sync) return kern::invalid_argument;
            immediate = true;
            immediate_id = notify::dead_name;
            value = name;
        } else if (!entry->send && !entry->send_once) {
            return kern::invalid_right;
        } else if (sync && entry->port->queue.size() < entry->port->qlimit) {
            immediate = true;
            value = name;
        }
        slot = &entry->sprequest;
        break;
    case notify::no_senders:
        if (!entry->receive) return kern::invalid_right;
        slot = &entry->port->nsrequest;
        if (!entry->port->srights && entry->port->holders.empty() && entry->port->mscount >= sync) {
            immediate = true;
            value = entry->port->mscount;
        }
        break;
    case notify::port_destroyed:
        if (!entry->receive) return kern::invalid_right;
        if (sync) return kern::invalid_value;
        slot = &entry->port->pdrequest;
        break;
    default:
        return kern::invalid_value;
    }
    const auto port = entry->port;
    Right target;
    if (notify_name != port_null) {
        if (copyin(notify_name, disposition, nullptr, target) != kern::success || (!target.port && !target.dead))
            return kern::invalid_capability;
    }
    // The entry may have moved in the map: look the slot up again.
    entry = find(name);
    if (!entry) { release(target); return kern::invalid_name; }
    if (id == notify::dead_name) slot = &entry->dnrequest;
    else if (id == notify::send_possible) slot = &entry->sprequest;
    else if (id == notify::no_senders) slot = &port->nsrequest;
    else slot = &port->pdrequest;
    if (*slot) previous = copyout(*std::exchange(*slot, std::nullopt));
    if (id == notify::no_senders) port->nssync = sync;
    if (!target.port) { touch(); return kern::success; }
    if (immediate) {
        if (immediate_id == notify::dead_name) {
            if (auto* dead = find(name)) ++dead->dead;
        }
        notify(target, immediate_id, value);
    } else {
        *slot = target;
    }
    touch();
    return kern::success;
}

std::uint32_t MachIpc::guard(Name name, std::uint64_t value, bool strict) {
    std::lock_guard lock(mutex_);
    auto* entry = find(name);
    if (!entry) return kern::invalid_name;
    if (!entry->receive) return kern::invalid_right;
    if (entry->port->guarded) return kern::invalid_argument;
    entry->port->guarded = true;
    entry->port->strict = strict;
    entry->port->context = value;
    return kern::success;
}

std::uint32_t MachIpc::unguard(Name name, std::uint64_t value) {
    std::lock_guard lock(mutex_);
    auto* entry = find(name);
    if (!entry) return kern::invalid_name;
    if (!entry->receive) return kern::invalid_right;
    if (!entry->port->guarded) return kern::invalid_argument;
    if (entry->port->context != value) guard_violation("mach_port_unguard", name);
    entry->port->guarded = entry->port->strict = false;
    entry->port->context = 0;
    return kern::success;
}

std::uint32_t MachIpc::swap_guard(Name name, std::uint64_t old_value, std::uint64_t new_value) {
    std::lock_guard lock(mutex_);
    auto* entry = find(name);
    if (!entry) return kern::invalid_name;
    if (!entry->receive) return kern::invalid_right;
    if (!entry->port->guarded) return kern::invalid_argument;
    if (entry->port->context != old_value) guard_violation("mach_port_swap_guard", name);
    entry->port->context = new_value;
    return kern::success;
}

std::uint32_t MachIpc::get_context(Name name, std::uint64_t& context) {
    std::lock_guard lock(mutex_);
    context = 0;
    auto* entry = find(name);
    if (!entry) return kern::invalid_name;
    if (!entry->receive) return kern::invalid_right;
    // A strictly guarded port's guard is not readable as its context.
    context = entry->port->strict ? 0 : entry->port->context;
    return kern::success;
}

std::uint32_t MachIpc::set_context(Name name, std::uint64_t context) {
    std::lock_guard lock(mutex_);
    auto* entry = find(name);
    if (!entry) return kern::invalid_name;
    if (!entry->receive) return kern::invalid_right;
    if (entry->port->guarded) return kern::invalid_argument;
    entry->port->context = context;
    return kern::success;
}

std::uint32_t MachIpc::set_mscount(Name name, std::uint32_t mscount) {
    std::lock_guard lock(mutex_);
    auto* entry = find(name);
    if (!entry) return kern::invalid_name;
    if (!entry->receive) return kern::invalid_right;
    entry->port->mscount = mscount;
    return kern::success;
}

std::uint32_t MachIpc::set_seqno(Name name, std::uint32_t seqno) {
    std::lock_guard lock(mutex_);
    auto* entry = find(name);
    if (!entry) return kern::invalid_name;
    if (!entry->receive) return kern::invalid_right;
    entry->port->seqno = seqno;
    return kern::success;
}

std::uint32_t MachIpc::peek(Name name, std::uint32_t& seqno, PeekResult& result) {
    std::lock_guard lock(mutex_);
    auto* entry = find(name);
    if (!entry) return kern::invalid_name;
    if (!entry->receive) return kern::invalid_right;
    const auto& port = *entry->port;
    const auto wanted = seqno ? seqno : port.seqno;
    if (wanted < port.seqno || wanted - port.seqno >= port.queue.size()) return kern::failure;
    const auto& message = *port.queue[wanted - port.seqno];
    result.seqno = seqno = wanted;
    result.size = static_cast<std::uint32_t>(message.bytes.size());
    result.id = static_cast<std::int32_t>(get32(message.bytes, 20));
    return kern::success;
}

// ---- held rights ------------------------------------------------------------------------

std::uint32_t MachIpc::take(Name name, std::uint8_t disposition, Right& out) {
    std::lock_guard lock(mutex_);
    const auto error = copyin(name, disposition, nullptr, out);
    touch();
    return error;
}

Name MachIpc::give(Right right) {
    std::lock_guard lock(mutex_);
    const auto name = copyout(std::move(right));
    touch();
    return name;
}

Right MachIpc::copy_send(const Right& right) {
    std::lock_guard lock(mutex_);
    if (!right.port || right.type != disp::port_send) return Right{nullptr, disp::port_send, right.dead};
    ++right.port->srights;
    return Right{right.port, disp::port_send};
}

void MachIpc::drop(Right right) {
    std::lock_guard lock(mutex_);
    release(std::move(right));
    touch();
}

bool MachIpc::alive(const Right& right) {
    std::lock_guard lock(mutex_);
    return right.port && right.port->active;
}

std::uint32_t MachIpc::post(const Right& destination, std::int32_t id) {
    std::lock_guard lock(mutex_);
    const auto port = destination.port;
    if (!port || !port->active || port->kobject || port->kernel_name) return mr::send_invalid_dest;
    if (port->queue.size() >= port->qlimit) return mr::send_timed_out;
    auto message = std::make_shared<Message>();
    message->kernel = true;
    message->bytes.assign(24, 0);
    put32(message->bytes, 4, 24);
    put32(message->bytes, 20, static_cast<std::uint32_t>(id));
    ++port->srights;
    message->destination = Right{port, disp::port_send};
    enqueue(port, std::move(message));
    return kern::success;
}

Name MachIpc::make_kernel_port(std::shared_ptr<KernelObject> object) {
    std::lock_guard lock(mutex_);
    const auto port = new_port();
    port->kobject = std::move(object);
    port->srights = 1;
    port->mscount = 1;
    const auto name = new_name();
    auto& entry = entries_[name];
    entry.port = port;
    entry.send = 1;
    port->send_name = name;
    return name;
}

std::shared_ptr<KernelObject> MachIpc::kernel_object(Name name) {
    std::lock_guard lock(mutex_);
    const auto* entry = find(name);
    if (!entry || !entry->send || !entry->port || !entry->port->active) return nullptr;
    return entry->port->kobject;
}

// ---- messages -----------------------------------------------------------------------------

namespace {
struct ParsedDescriptor {
    std::uint32_t offset{};
    std::uint8_t type{}, disposition{};
    bool deallocate{};
    Address address{};
    std::uint32_t count{};
    std::vector<Name> names;
    std::vector<std::uint8_t> data;
};
} // namespace

std::uint32_t MachIpc::send(transmute::GuestMemory& memory, Address message, std::uint32_t options, std::uint32_t size,
                            std::uint32_t timeout_ms) {
    if (size < 24 || (size & 3)) return mr::send_msg_too_small;
    if (size > message_limit) return mr::send_too_large;
    std::vector<std::uint8_t> bytes(size);
    try { memory.read_bytes(message, bytes); } catch (const std::exception&) { return mr::send_invalid_data; }
    put32(bytes, 4, size);
    const auto bits = get32(bytes, 0);
    const auto remote = get32(bytes, 8), local = get32(bytes, 12), voucher = get32(bytes, 16);
    const auto remote_disposition = static_cast<std::uint8_t>(bits & 0x1f);
    const auto local_disposition = static_cast<std::uint8_t>((bits >> 8) & 0x1f);
    // The body: descriptors and the out-of-line data they name (read before
    // anything is copied in).
    std::vector<ParsedDescriptor> parsed;
    if (bits & complex_bit) {
        if (size < 28) return mr::send_msg_too_small;
        const auto count = get32(bytes, 24);
        std::size_t cursor = 28;
        for (std::uint32_t index = 0; index < count; ++index) {
            if (cursor + 12 > size) return mr::send_msg_too_small;
            ParsedDescriptor descriptor;
            descriptor.offset = static_cast<std::uint32_t>(cursor);
            descriptor.type = bytes[cursor + 11];
            if (descriptor.type == port_descriptor) {
                descriptor.names = {get32(bytes, cursor)};
                descriptor.disposition = bytes[cursor + 10];
                cursor += 12;
            } else if (descriptor.type == ool_descriptor || descriptor.type == ool_volatile_descriptor ||
                       descriptor.type == ool_ports_descriptor) {
                if (cursor + 16 > size) return mr::send_msg_too_small;
                descriptor.address = get64(bytes, cursor);
                descriptor.deallocate = bytes[cursor + 8] != 0;
                descriptor.disposition = bytes[cursor + 10];
                descriptor.count = get32(bytes, cursor + 12);
                if (descriptor.type == ool_ports_descriptor) {
                    if (descriptor.count > message_limit / 4) return mr::send_too_large;
                    std::vector<std::uint8_t> raw(descriptor.count * 4ULL);
                    if (!raw.empty()) {
                        try { memory.read_bytes(descriptor.address, raw); } catch (const std::exception&) { return mr::send_invalid_memory; }
                    }
                    for (std::uint32_t at = 0; at < descriptor.count; ++at) descriptor.names.push_back(get32(raw, at * 4ULL));
                } else {
                    if (descriptor.count > message_limit) return mr::send_too_large;
                    descriptor.data.resize(descriptor.count);
                    if (!descriptor.data.empty()) {
                        try { memory.read_bytes(descriptor.address, descriptor.data); } catch (const std::exception&) { return mr::send_invalid_memory; }
                    }
                }
                cursor += 16;
            } else {
                // MACH_MSG_GUARDED_PORT_DESCRIPTOR (whose received size depends
                // on MACH_RCV_GUARDED_DESC) is not supported; unknown types are invalid.
                return mr::send_invalid_type;
            }
            parsed.push_back(std::move(descriptor));
        }
    }
    if (!send_disposition(remote_disposition)) return mr::send_invalid_header;
    if (local != port_null && local != port_dead && !send_disposition(local_disposition)) return mr::send_invalid_header;
    if (voucher != port_null && voucher != port_dead) return mr::send_invalid_voucher;   // no vouchers exist
    for (const auto& descriptor : parsed)
        if (descriptor.type != ool_descriptor && descriptor.type != ool_volatile_descriptor && !valid_disposition(descriptor.disposition))
            return mr::send_invalid_type;
    const bool once = remote_disposition == disp::move_send_once || remote_disposition == disp::make_send_once;
    const auto deadline = (options & option::send_timeout) ? std::optional(Clock::now() + std::chrono::milliseconds(timeout_ms))
                                                           : std::nullopt;
    Lock lock(mutex_);
    // Wait for room in the destination queue. Send-once messages and
    // MACH_SEND_ALWAYS ignore the queue limit (up to the kernel limit).
    for (;;) {
        std::shared_ptr<Port> port;
        if (remote != port_null && remote != port_dead) {
            if (const auto* entry = find(remote)) {
                const bool held = remote_disposition == disp::make_send || remote_disposition == disp::make_send_once ? entry->receive
                    : remote_disposition == disp::move_send_once ? entry->send_once : entry->send != 0;
                if (held) port = entry->port;
            }
        }
        if (!port || !port->active || port->kobject || port->kernel_name) return mr::send_invalid_dest;
        if (port->host) break;   // another process's port: its queue is not limited for remote senders
        if (once || (options & option::send_always)) {
            if (port->queue.size() >= qlimit_kernel) return mr::send_no_buffer;
            break;
        }
        if (port->queue.size() < port->qlimit) break;
        if (deadline && Clock::now() >= *deadline) return mr::send_timed_out;
        if (guest_.interrupted && guest_.interrupted()) return mr::send_interrupted;
        const auto limit = Clock::now() + std::chrono::milliseconds(100);
        changed_.wait_until(lock, deadline ? std::min(*deadline, limit) : limit);
    }
    Transaction transaction;
    const auto rollback = [&](std::uint32_t code) {
        for (auto& [name, entry] : transaction.entries) {
            if (entry) entries_[name] = *entry;
            else entries_.erase(name);
        }
        for (auto& [port, state] : transaction.ports) {
            port->receiver = state.receiver;
            port->send_name = state.send_name;
            port->send_once_names = state.send_once_names;
            port->srights = state.srights;
            port->sorights = state.sorights;
            port->mscount = state.mscount;
            port->sets = state.sets;
        }
        for (auto& [set, members] : transaction.sets) set->members = members;
        return code;
    };
    auto envelope = std::make_shared<Message>();
    if (copyin(remote, remote_disposition, &transaction, envelope->destination) != kern::success || !envelope->destination.port ||
        !envelope->destination.port->active)
        return rollback(mr::send_invalid_dest);
    const auto destination = envelope->destination.port;
    if (local != port_null && copyin(local, local_disposition, &transaction, envelope->reply) != kern::success)
        return rollback(mr::send_invalid_reply);
    for (auto& descriptor : parsed) {
        Descriptor carried;
        carried.offset = descriptor.offset;
        carried.type = descriptor.type;
        carried.count = descriptor.count;
        if (descriptor.type == ool_descriptor || descriptor.type == ool_volatile_descriptor) {
            carried.data = std::move(descriptor.data);
        } else {
            carried.received = received_type(descriptor.disposition);
            for (const auto name : descriptor.names) {
                Right moved;
                if (copyin(name, descriptor.disposition, &transaction, moved) != kern::success) return rollback(mr::send_invalid_right);
                // A port's own receive right cannot travel in a message to it.
                if (moved.port == destination && moved.type == disp::port_receive) return rollback(mr::send_invalid_right);
                carried.rights.push_back(std::move(moved));
            }
        }
        envelope->descriptors.push_back(std::move(carried));
    }
    envelope->bytes = std::move(bytes);
    for (auto& [name, entry] : transaction.vanished) {
        entry_vanished(name, entry);
        if (allocated_name(name) && !entries_.contains(name)) free_indices_.emplace_back(name >> 8, static_cast<std::uint8_t>(name & 0xff));
    }
    if (destination->host) {
        lock.unlock();
        transmit(std::move(envelope), destination);
    } else {
        enqueue(destination, std::move(envelope));
        lock.unlock();
    }
    // Out-of-line memory the sender gave up (deallocate = TRUE).
    for (const auto& descriptor : parsed) {
        if (!descriptor.deallocate || !descriptor.address) continue;
        const auto length = descriptor.type == ool_ports_descriptor ? descriptor.count * 4ULL : descriptor.count;
        if (length && guest_.deallocate) guest_.deallocate(descriptor.address, length);
    }
    return kern::success;
}

// The port of `name` (receive right or port set) whose queue has the oldest
// message, or nullptr; `error` is set when `name` cannot receive.
std::shared_ptr<Port> MachIpc::ready_port(Name name, std::uint32_t& error) {
    error = 0;
    const auto* entry = find(name);
    if (!entry || (!entry->receive && !entry->set)) { error = mr::rcv_invalid_name; return nullptr; }
    if (entry->receive) return entry->port->queue.empty() ? nullptr : entry->port;
    std::shared_ptr<Port> best;
    for (const auto& member : entry->set->members) {
        if (member->queue.empty()) continue;
        if (!best || member->queue.front()->stamp < best->queue.front()->stamp) best = member;
    }
    return best;
}

std::uint32_t MachIpc::receive(transmute::GuestMemory& memory, Address buffer, std::uint32_t options, std::uint32_t size,
                               Name name, std::uint32_t timeout_ms, std::uint32_t* received) {
    Lock lock(mutex_);
    std::optional<Clock::time_point> deadline;
    if (options & option::rcv_timeout) deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    return receive_locked(lock, memory, buffer, options, size, name, deadline, received);
}

std::uint32_t MachIpc::receive_locked(Lock& lock, transmute::GuestMemory& memory, Address buffer, std::uint32_t options,
                                      std::uint32_t size, Name name, std::optional<Clock::time_point> deadline,
                                      std::uint32_t* received) {
    if (received) *received = 0;
    const auto trailer_type = (options >> 28) & 0xf, elements = (options >> 24) & 0xf;
    if (trailer_type != 0 || elements >= trailer_sizes.size()) return mr::rcv_invalid_trailer;
    const auto trailer_size = trailer_sizes[elements];
    // What `name` denoted when the receive began, to tell a destroyed port
    // from a moved receive right.
    const auto* initial = find(name);
    const std::shared_ptr<Port> initial_port = initial && initial->receive ? initial->port : nullptr;
    const std::shared_ptr<PortSet> initial_set = initial ? initial->set : nullptr;
    std::shared_ptr<Port> port;
    for (;;) {
        std::uint32_t error{};
        port = ready_port(name, error);
        if (error) {
            if (initial_port && !initial_port->active) return mr::rcv_port_died;
            if (initial_port) return mr::rcv_port_changed;
            if (initial_set) return mr::rcv_port_died;
            return error;
        }
        const auto* entry = find(name);
        if ((initial_port && entry->port != initial_port) || (initial_set && entry->set != initial_set)) return mr::rcv_port_changed;
        if (port) break;
        if (deadline && Clock::now() >= *deadline) return mr::rcv_timed_out;
        if (guest_.interrupted && guest_.interrupted()) return mr::rcv_interrupted;
        const auto limit = Clock::now() + std::chrono::milliseconds(100);
        changed_.wait_until(lock, deadline ? std::min(*deadline, limit) : limit);
    }
    auto message = port->queue.front();
    const auto message_size = static_cast<std::uint32_t>(message->bytes.size());
    const auto needed = message_size + trailer_size;
    if (needed > size) {
        if (options & option::rcv_large) {
            // The message stays queued; the caller learns its size (and,
            // with MACH_RCV_LARGE_IDENTITY, which port holds it).
            lock.unlock();
            try {
                if (size >= 8) memory.write32(buffer + 4, needed);
                if ((options & option::rcv_large_identity) && size >= 16) memory.write32(buffer + 12, port->receiver);
            } catch (const std::exception&) { return mr::rcv_invalid_data; }
            return mr::rcv_too_large;
        }
        port->queue.pop_front();
        ++port->seqno;
        discard(*message);
        if (port->queue.size() < port->qlimit) send_possible(*port);
        touch();
        return mr::rcv_too_large;
    }
    port->queue.pop_front();
    const auto seqno = port->seqno++;
    const auto destination_type = message->destination.type;
    if (destination_type == disp::port_send_once) drop_send_once(port);
    else drop_send(port);
    message->destination = {};
    const auto reply_type = message->reply.port || message->reply.dead ? message->reply.type : std::uint8_t{0};
    const auto reply_name = copyout(std::exchange(message->reply, {}));
    std::vector<std::vector<Name>> descriptor_names;
    // Kernel objects from another process become local ports once unlocked.
    std::vector<std::tuple<std::size_t, std::size_t, std::shared_ptr<WireObject>>> objects;
    for (std::size_t index = 0; index < message->descriptors.size(); ++index) {
        auto& descriptor = message->descriptors[index];
        std::vector<Name> names;
        for (auto& carried : descriptor.rights) {
            if (carried.object) objects.emplace_back(index, names.size(), std::exchange(carried.object, nullptr));
            names.push_back(copyout(std::exchange(carried, {})));
        }
        descriptor_names.push_back(std::move(names));
    }
    const auto local_name = port->receiver;
    const auto context = port->context;
    if (port->queue.size() < port->qlimit) send_possible(*port);
    touch();
    lock.unlock();
    for (auto& [index, position, data] : objects) {
        // Taken: deserialize_object owns (and on failure closes) what it names.
        const auto object = guest_.deserialize_object ? guest_.deserialize_object(std::exchange(data->bytes, {})) : nullptr;
        descriptor_names[index][position] = object ? make_kernel_port(object) : port_dead;
    }

    auto& bytes = message->bytes;
    const auto complex = get32(bytes, 0) & complex_bit;
    put32(bytes, 0, complex | reply_type | (static_cast<std::uint32_t>(destination_type) << 8));
    put32(bytes, 4, message_size);
    put32(bytes, 8, reply_name);
    put32(bytes, 12, local_name);
    put32(bytes, 16, 0);
    std::uint32_t result = kern::success;
    for (std::size_t index = 0; index < message->descriptors.size(); ++index) {
        const auto& descriptor = message->descriptors[index];
        const auto at = descriptor.offset;
        if (descriptor.type == port_descriptor) {
            put32(bytes, at, descriptor_names[index].empty() ? 0 : descriptor_names[index][0]);
            put32(bytes, at + 4, 0);
            bytes[at + 8] = bytes[at + 9] = 0;
            bytes[at + 10] = descriptor.received;
        } else if (descriptor.type == ool_ports_descriptor) {
            const auto& names = descriptor_names[index];
            Address array = 0;
            if (!names.empty()) {
                array = guest_.allocate ? guest_.allocate(names.size() * 4ULL) : 0;
                if (!array) result = mr::rcv_body_error;
                else {
                    std::vector<std::uint8_t> raw(names.size() * 4);
                    for (std::size_t at_name = 0; at_name < names.size(); ++at_name) put32(raw, at_name * 4, names[at_name]);
                    memory.write_bytes(array, raw);
                }
            }
            put64(bytes, at, array);
            bytes[at + 8] = 0;
            bytes[at + 9] = 1;   // MACH_MSG_VIRTUAL_COPY
            bytes[at + 10] = descriptor.received;
            put32(bytes, at + 12, static_cast<std::uint32_t>(names.size()));
        } else {
            Address region = 0;
            std::uint64_t length = descriptor.data.size();
            if (descriptor.ool) {
                // Out-of-line memory in a section from another process.
                length = descriptor.count;
                region = guest_.receive_ool ? guest_.receive_ool(std::exchange(descriptor.ool->bytes, {}), length) : 0;
                if (!region) result = mr::rcv_body_error;
            } else if (!descriptor.data.empty()) {
                region = guest_.allocate ? guest_.allocate(descriptor.data.size()) : 0;
                if (!region) result = mr::rcv_body_error;
                else memory.write_bytes(region, descriptor.data);
            }
            put64(bytes, at, region);
            bytes[at + 8] = 0;
            bytes[at + 9] = 1;   // MACH_MSG_VIRTUAL_COPY
            put32(bytes, at + 12, region ? static_cast<std::uint32_t>(length) : 0);
        }
    }
    // The format-0 trailer: seqno, security token, audit token, context, AV fields.
    std::vector<std::uint8_t> trailer(68, 0);
    put32(trailer, 0, 0);
    put32(trailer, 4, trailer_size);
    put32(trailer, 8, seqno);
    if (!message->kernel && (message->audit || guest_.audit_token)) {
        const auto token = message->audit ? *message->audit : guest_.audit_token();
        put32(trailer, 12, token[1]);   // msgh_sender: {euid, egid}
        put32(trailer, 16, token[2]);
        for (std::size_t word = 0; word < token.size(); ++word) put32(trailer, 20 + word * 4, token[word]);
    }
    put64(trailer, 52, context);
    bytes.insert(bytes.end(), trailer.begin(), trailer.begin() + trailer_size);
    try { memory.write_bytes(buffer, bytes); } catch (const std::exception&) { return mr::rcv_invalid_data; }
    if (received) *received = needed;
    return result;
}

void MachIpc::destroy_message(transmute::GuestMemory& memory, Address message) {
    if (!message) return;
    const auto bits = memory.read32(message);
    const auto destroy_name = [&](Name name, std::uint8_t kind) {
        if (name == port_null || name == port_dead) return;
        switch (kind) {
        case disp::move_send: case disp::move_send_once: static_cast<void>(deallocate(name)); break;
        case disp::move_receive: static_cast<void>(mod_refs(name, right::receive, -1)); break;
        case disp::make_send:
            if (insert_right(name, name, disp::make_send) == kern::success) static_cast<void>(deallocate(name));
            break;
        case disp::make_send_once: {
            Name once{};
            std::uint8_t type_out{};
            if (extract_right(name, disp::make_send_once, once, type_out) == kern::success) static_cast<void>(deallocate(once));
            break;
        }
        default: break;   // COPY_SEND and the dispose types hold nothing
        }
    };
    // msgh_local_port holds no right: the receive consumed the destination.
    destroy_name(memory.read32(message + 8), static_cast<std::uint8_t>(bits & 0x1f));
    destroy_name(memory.read32(message + 16), static_cast<std::uint8_t>((bits >> 16) & 0x1f));
    if (!(bits & complex_bit)) return;
    const auto count = memory.read32(message + 24);
    auto cursor = message + 28;
    for (std::uint32_t index = 0; index < count && index < 65536; ++index) {
        const auto descriptor_type = memory.read8(cursor + 11);
        if (descriptor_type == port_descriptor) {
            destroy_name(memory.read32(cursor), memory.read8(cursor + 10));
            cursor += 12;
            continue;
        }
        const auto address = memory.read64(cursor);
        const auto length = memory.read32(cursor + 12);
        if (descriptor_type == ool_descriptor || descriptor_type == ool_volatile_descriptor) {
            if (address && length && guest_.deallocate) guest_.deallocate(address, length);
        } else if (descriptor_type == ool_ports_descriptor) {
            const auto kind = memory.read8(cursor + 10);
            for (std::uint32_t at = 0; address && at < length; ++at) destroy_name(memory.read32(address + at * 4ULL), kind);
            if (address && length && guest_.deallocate) guest_.deallocate(address, length * 4ULL);
        } else if (descriptor_type == guarded_port_descriptor) {
            destroy_name(memory.read32(cursor + 12), memory.read8(cursor + 10));
        }
        cursor += 16;
    }
}

// ---- EVFILT_MACHPORT ------------------------------------------------------------------------------

std::uint32_t MachIpc::attach(std::uint32_t name) {
    std::lock_guard lock(mutex_);
    const auto* entry = find(name);
    if (!entry) return 2;                                   // ENOENT
    return entry->receive || entry->set ? 0 : 45;           // ENOTSUP
}

std::optional<MachPortEvents::Event> MachIpc::poll(transmute::GuestMemory& memory, std::uint32_t name, std::uint32_t fflags,
                                                   std::uint64_t ext0, std::uint64_t ext1) {
    Lock lock(mutex_);
    std::uint32_t error{};
    const auto port = ready_port(name, error);
    if (error || !port) return std::nullopt;
    if (!(fflags & option::rcv_msg)) return Event{port->receiver, 0, 0, newest(name)};
    // Direct receive into ext[0] (ext[1] bytes). A zero ext[1] asks for the
    // kevent data_out area, which Alchemy does not provide: the message stays
    // queued and MACH_RCV_TOO_LARGE is reported.
    std::uint32_t received{};
    const auto result = receive_locked(lock, memory, ext0, (fflags & ~option::rcv_timeout) | option::rcv_large,
                                       static_cast<std::uint32_t>(std::min<std::uint64_t>(ext1, UINT32_MAX)), name,
                                       Clock::now(), &received);
    if (result == mr::rcv_timed_out) return std::nullopt;
    return Event{0, result, received};
}

// The newest message stamp queued on a receive right or any member of a set.
std::uint64_t MachIpc::newest(Name name) {
    const auto* entry = find(name);
    std::uint64_t stamp = 0;
    if (!entry) return 0;
    if (entry->receive && !entry->port->queue.empty()) stamp = entry->port->queue.back()->stamp;
    if (entry->set)
        for (const auto& member : entry->set->members)
            if (!member->queue.empty()) stamp = std::max(stamp, member->queue.back()->stamp);
    return stamp;
}

bool MachIpc::newer(std::uint32_t name, std::uint64_t stamp) {
    std::lock_guard lock(mutex_);
    return newest(name) > stamp;
}

std::string MachIpc::describe_stale_queues(std::chrono::milliseconds older_than) {
    Lock lock(mutex_, std::defer_lock);
    for (int attempt = 0; attempt < 200 && !lock.try_lock(); ++attempt) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!lock.owns_lock()) return " mach: the port space stayed locked for 2 s\n";
    const auto now = std::chrono::steady_clock::now();
    std::string out;
    std::size_t ports = 0, queued = 0;
    for (const auto& [name, entry] : entries_) {
        if (!entry.receive || !entry.port || entry.port->queue.empty()) continue;
        ++ports;
        queued += entry.port->queue.size();
        const auto& front = *entry.port->queue.front();
        const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - front.queued);
        if (age < older_than) continue;
        char line[160];
        std::snprintf(line, sizeof(line), " mach: port 0x%x holds %zu message(s), the oldest (id 0x%x%s) for %lld ms", name,
                      entry.port->queue.size(), get32(front.bytes, 20), front.kernel ? ", a notification" : "",
                      static_cast<long long>(age.count()));
        out += line;
        for (const auto* set : entry.port->sets) {
            std::snprintf(line, sizeof(line), "; in set 0x%x", set->name);
            out += line;
        }
        out += '\n';
    }
    char summary[96];
    std::snprintf(summary, sizeof(summary), " mach: %zu receive right(s) with %zu queued message(s)\n", ports, queued);
    return summary + out;
}

bool MachIpc::dead_name(std::uint32_t name) {
    std::lock_guard lock(mutex_);
    const auto* entry = find(name);
    return entry && entry->dead && !entry->send && !entry->send_once && !entry->receive;
}

std::uint64_t MachIpc::epoch() {
    std::lock_guard lock(mutex_);
    return epoch_;
}

void MachIpc::wait(std::uint64_t seen, std::chrono::steady_clock::duration limit) {
    Lock lock(mutex_);
    changed_.wait_for(lock, limit, [&] { return epoch_ != seen; });
}

} // namespace alchemy::libsystem::mach
