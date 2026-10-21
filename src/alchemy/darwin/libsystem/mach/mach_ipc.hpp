#pragma once

// In-process Mach IPC for alchemy-fx (docs/ALCHEMY_CHROME_AUDIT.md, stage 1
// item (e)): one IPC space (the task's), its port names and rights, port
// sets, notifications and mach_msg message transfer. Independently written
// from the public <mach/*.h> interfaces and their documented semantics; no
// Apple code. Guest exports are in mach_exports.cpp.
//
// Model:
// - A Port is a kernel port. Its receive right lives in the space (under
//   `receiver`), in a message, or with the kernel (kernel-object ports:
//   memory entries, the bootstrap port, identity tokens).
// - The space maps names to Entries. As in Mach, one name denotes all send
//   and receive rights of one port; every send-once right has its own name;
//   a dead name remains when a port dies.
// - Names Alchemy hands out elsewhere (mach_task_self 0xa1c1, the host and
//   clock ports, thread ports = thread identities, semaphores) are "kernel
//   names": immortal send rights outside the allocated range (index >=
//   0x1000, generation a non-zero multiple of 4).
// - Rights in a message are counted like Mach's (srights, sorights, the
//   make-send count), so no-senders, send-once and dead-name notifications
//   arrive as the kernel sends them.
// - One lock serializes the space; waits (blocking receive, full queues,
//   kevent) share one condition variable and an epoch counter.

#include "alchemy/darwin/libsystem/mach/transport.hpp"
#include "alchemy/mach_port_events.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace transmute { class GuestMemory; }

namespace alchemy::libsystem::mach {

using Name = std::uint32_t;
using Address = std::uint64_t;

inline constexpr Name port_null = 0, port_dead = 0xffffffffU;

// <mach/kern_return.h>
namespace kern {
inline constexpr std::uint32_t success = 0, invalid_address = 1, protection_failure = 2, no_space = 3, invalid_argument = 4,
    failure = 5, resource_shortage = 6, already_in_set = 11, not_in_set = 12, name_exists = 13, aborted = 14,
    invalid_name = 15, invalid_task = 16, invalid_right = 17, invalid_value = 18, urefs_overflow = 19,
    invalid_capability = 20, right_exists = 21, not_supported = 46;
}
// <mach/message.h> mach_msg_return_t
namespace mr {
inline constexpr std::uint32_t send_invalid_data = 0x10000002, send_invalid_dest = 0x10000003, send_timed_out = 0x10000004,
    send_invalid_voucher = 0x10000005, send_interrupted = 0x10000007, send_msg_too_small = 0x10000008,
    send_invalid_reply = 0x10000009, send_invalid_right = 0x1000000a, send_invalid_memory = 0x1000000c,
    send_no_buffer = 0x1000000d, send_too_large = 0x1000000e, send_invalid_type = 0x1000000f,
    send_invalid_header = 0x10000010, rcv_invalid_name = 0x10004002, rcv_timed_out = 0x10004003,
    rcv_too_large = 0x10004004, rcv_interrupted = 0x10004005, rcv_port_changed = 0x10004006,
    rcv_invalid_data = 0x10004008, rcv_port_died = 0x10004009, rcv_body_error = 0x1000400c,
    rcv_invalid_trailer = 0x1000400f;
}
// mach_msg_type_name_t dispositions and received types.
namespace disp {
inline constexpr std::uint8_t move_receive = 16, move_send = 17, move_send_once = 18, copy_send = 19, make_send = 20,
    make_send_once = 21, port_receive = 16, port_send = 17, port_send_once = 18;
}
// mach_port_right_t
namespace right {
inline constexpr std::uint32_t send = 0, receive = 1, send_once = 2, port_set = 3, dead_name = 4;
}
// mach_msg_option_t
namespace option {
inline constexpr std::uint32_t send_msg = 0x1, rcv_msg = 0x2, rcv_large = 0x4, rcv_large_identity = 0x8,
    send_timeout = 0x10, rcv_timeout = 0x100, send_always = 0x10000;
}
// Notification message ids (<mach/notify.h>).
namespace notify {
inline constexpr std::int32_t port_deleted = 0101, send_possible = 0102, port_destroyed = 0105, no_senders = 0106,
    send_once = 0107, dead_name = 0110;
}

struct Port;
struct PortSet;

// A port's identity across processes: the process that first exported it and
// its id there. Frames name ports by key (docs/ALCHEMY_PROCESS.md).
struct Key {
    std::uint32_t pid{};
    std::uint64_t id{};
    auto operator<=>(const Key&) const = default;
};

// Bytes from another process that stand for a host resource this process
// now owns (a section or file handle duplicated into it): closed with
// `discard` unless taken (`bytes` moved out) or forwarded.
struct WireObject {
    std::vector<std::uint8_t> bytes;
    std::function<void(const std::vector<std::uint8_t>&)> discard;
    WireObject() = default;
    WireObject(const WireObject&) = delete;
    WireObject& operator=(const WireObject&) = delete;
    ~WireObject() {
        if (discard && !bytes.empty()) {
            try { discard(bytes); } catch (...) {}
        }
    }
};

// A right outside the space: in a message, a registry or a notification
// request. `port` null is MACH_PORT_NULL, or MACH_PORT_DEAD with `dead`.
struct Right {
    std::shared_ptr<Port> port;
    std::uint8_t type{};   // disp::port_receive / port_send / port_send_once
    bool dead{};
    // A kernel object received from another process (a memory entry by
    // value), turned into a local kernel port when the message is received.
    std::shared_ptr<WireObject> object;
};

struct KernelObject { virtual ~KernelObject() = default; };
// A task port received from another process: names that task (pid_for_task).
struct RemoteTask final : KernelObject {
    explicit RemoteTask(std::uint32_t value) : pid(value) {}
    std::uint32_t pid{};
};

struct Descriptor {
    std::uint32_t offset{};        // in the message image
    std::uint8_t type{};           // MACH_MSG_*_DESCRIPTOR
    std::uint8_t received{};       // the received right type (port / OOL ports)
    std::uint32_t count{};         // OOL size or port count
    std::vector<Right> rights;
    std::vector<std::uint8_t> data;
    std::shared_ptr<WireObject> ool;   // out-of-line memory in a section from another process
};

struct Message {
    std::vector<std::uint8_t> bytes;   // the message image as sent (header first)
    Right destination;                 // the send or send-once right it was sent with
    Right reply;
    std::vector<Descriptor> descriptors;
    std::uint64_t stamp{};
    bool kernel{};                     // sent by the kernel (notifications)
    std::optional<std::array<std::uint32_t, 8>> audit;   // the sending task's audit token (another process)
};

struct Port {
    std::uint64_t id{};
    bool active{true};
    Name receiver{};                   // receive right's name; 0 in transit or kernel-held
    Name send_name{};                  // the entry holding send/receive rights, if any
    std::set<Name> send_once_names;
    Name kernel_name{};                // fixed name of a kernel-named port (task, thread, host)
    std::shared_ptr<KernelObject> kobject;
    std::deque<std::shared_ptr<Message>> queue;
    std::uint32_t qlimit{5}, seqno{}, mscount{}, srights{}, sorights{};
    std::uint64_t context{};
    bool guarded{}, strict{}, immovable{};
    std::uint32_t status_flags{};      // tempowner / importance hints (reported only)
    std::vector<PortSet*> sets;
    std::optional<Right> nsrequest, pdrequest;
    std::uint32_t nssync{};
    // Across processes (mach_remote.cpp). A port hosted here gets a key when a
    // right to it first leaves the process; `holders`/`once_holders` count the
    // send and send-once units other processes hold on it. A proxy stands for
    // a port hosted by process `host`: its local rights are counted as usual,
    // and `remote_refs`/`remote_once` are the units this process holds there.
    // A port whose receive right moved away becomes a proxy that forwards.
    Key key{};
    std::uint32_t host{};
    std::uint64_t remote_refs{}, remote_once{};
    std::map<std::uint32_t, std::uint64_t> holders, once_holders;
    bool migrating{};
};

struct PortSet {
    Name name{};
    std::vector<std::shared_ptr<Port>> members;
};

struct Entry {
    std::shared_ptr<Port> port;        // send, receive, send-once or dead name
    std::shared_ptr<PortSet> set;
    bool receive{}, send_once{};
    std::uint32_t send{}, dead{};      // user references
    std::optional<Right> dnrequest, sprequest;
    bool empty() const { return !receive && !send_once && !send && !dead && !set; }
};

class MachIpc final : public MachPortEvents, public Transport::Handler, public std::enable_shared_from_this<MachIpc> {
public:
    // Guest services the space needs (set once by the provider).
    struct Guest {
        std::function<Address(std::uint64_t size)> allocate;             // vm_allocate anywhere; 0 when refused
        std::function<void(Address, std::uint64_t)> deallocate;          // vm_deallocate (or munmap)
        std::function<bool(Name)> kernel_name;                           // a name handed out outside the space
        std::function<bool()> interrupted;                               // process exit: true, or throws GuestThreadStop
        std::function<std::array<std::uint32_t, 8>()> audit_token;       // this task's audit_token_t
        std::uint32_t task_self_name{};                                  // mach_task_self()
        // A kernel object as bytes for another process (empty: it cannot
        // travel), and the local object made from such bytes. Called on guest
        // threads without the space lock.
        std::function<std::optional<std::vector<std::uint8_t>>(const std::shared_ptr<KernelObject>&, std::uint32_t recipient)> serialize_object;
        std::function<std::shared_ptr<KernelObject>(const std::vector<std::uint8_t>&)> deserialize_object;
        // Host-only (any thread, no guest calls): an object received from
        // another process passed on to `recipient` (its handles moved there),
        // or closed.
        std::function<std::optional<std::vector<std::uint8_t>>(const std::vector<std::uint8_t>&, std::uint32_t recipient)> forward_object;
        std::function<void(const std::vector<std::uint8_t>&)> discard_object;
        // Out-of-line memory of at least `ool_section_minimum` bytes crosses
        // processes in a host section: export_ool (host-only) makes the
        // object for `recipient`; receive_ool (guest thread) copies it into
        // new memory, 0 on failure. Such objects forward and discard as above.
        std::uint64_t ool_section_minimum{};
        std::function<std::optional<std::vector<std::uint8_t>>(const std::vector<std::uint8_t>& data, std::uint32_t recipient)> export_ool;
        std::function<Address(const std::vector<std::uint8_t>& object, std::uint64_t size)> receive_ool;
        // Host-only (transport threads): a routine on this task called through
        // another process's task port (task_info, task_policy_*), its answer.
        std::function<std::vector<std::uint8_t>(std::uint32_t from, const std::vector<std::uint8_t>& call)> task_call;
    };
    explicit MachIpc(Guest guest) : guest_(std::move(guest)) {}

    static bool allocated_name(Name name) { return (name >> 8) >= first_index && (name & 3) == 0 && (name & 0xff) != 0; }

    // ---- names and rights (kern_return_t) ----------------------------------
    std::uint32_t allocate(std::uint32_t right, Name& name);
    std::uint32_t allocate_name(std::uint32_t right, Name name);
    struct Options { std::uint32_t flags{}, qlimit{}; };
    std::uint32_t construct(const Options& options, std::uint64_t context, Name& name);
    std::uint32_t destruct(Name name, std::int32_t srdelta, std::uint64_t guard);
    std::uint32_t deallocate(Name name);
    std::uint32_t destroy(Name name);
    std::uint32_t mod_refs(Name name, std::uint32_t right, std::int32_t delta);
    std::uint32_t get_refs(Name name, std::uint32_t right, std::uint32_t& refs);
    std::uint32_t type(Name name, std::uint32_t& type);
    std::uint32_t insert_right(Name name, Name poly, std::uint8_t disposition);
    std::uint32_t extract_right(Name name, std::uint8_t disposition, Name& out, std::uint8_t& type);
    std::uint32_t move_member(Name member, Name after);
    std::uint32_t insert_member(Name member, Name set);
    std::uint32_t extract_member(Name member, Name set);
    std::uint32_t set_status(Name set, std::vector<Name>& members);
    std::uint32_t names(std::vector<std::pair<Name, std::uint32_t>>& out);
    std::uint32_t get_attributes(Name name, std::uint32_t flavor, std::uint32_t capacity, std::vector<std::uint32_t>& out);
    std::uint32_t set_attributes(Name name, std::uint32_t flavor, const std::vector<std::uint32_t>& info);
    std::uint32_t request_notification(Name name, std::int32_t id, std::uint32_t sync, Name notify, std::uint8_t disposition,
                                       Name& previous);
    std::uint32_t guard(Name name, std::uint64_t guard, bool strict);
    std::uint32_t unguard(Name name, std::uint64_t guard);
    std::uint32_t swap_guard(Name name, std::uint64_t old_guard, std::uint64_t new_guard);
    std::uint32_t get_context(Name name, std::uint64_t& context);
    std::uint32_t set_context(Name name, std::uint64_t context);
    std::uint32_t set_mscount(Name name, std::uint32_t mscount);
    std::uint32_t set_seqno(Name name, std::uint32_t seqno);
    struct PeekResult { std::uint32_t seqno{}, size{}; std::int32_t id{}; };
    std::uint32_t peek(Name name, std::uint32_t& seqno, PeekResult& result);

    // ---- rights held by host code (registries, special ports) --------------
    // Copies in a right from the space as a message would (MOVE/COPY/MAKE).
    std::uint32_t take(Name name, std::uint8_t disposition, Right& out);
    // Inserts a held right into the space and returns its name.
    Name give(Right right);
    Right copy_send(const Right& right);   // another send right to the same port
    void drop(Right right);
    bool alive(const Right& right);        // the right's port still has a receiver
    // Queues a header-only message with `id` on a held send right without
    // blocking (notify(3) delivery): MACH_SEND_TIMED_OUT when the queue is full.
    std::uint32_t post(const Right& destination, std::int32_t id);
    // A new kernel-held port for `object` and a send right to it in the space.
    Name make_kernel_port(std::shared_ptr<KernelObject> object);
    // The kernel object a send right in the space names (nullptr if none).
    std::shared_ptr<KernelObject> kernel_object(Name name);

    // ---- messages (mach_msg_return_t) ---------------------------------------
    std::uint32_t send(transmute::GuestMemory& memory, Address message, std::uint32_t options, std::uint32_t size,
                       std::uint32_t timeout_ms);
    // `received` is the message plus trailer size written (0 when nothing was received).
    std::uint32_t receive(transmute::GuestMemory& memory, Address buffer, std::uint32_t options, std::uint32_t size,
                          Name name, std::uint32_t timeout_ms, std::uint32_t* received = nullptr);
    // mach_msg_destroy: releases the rights and memory a message holds.
    void destroy_message(transmute::GuestMemory& memory, Address message);

    // ---- EVFILT_MACHPORT ----------------------------------------------------
    std::uint32_t attach(std::uint32_t name) override;
    std::optional<Event> poll(transmute::GuestMemory& memory, std::uint32_t name, std::uint32_t fflags, std::uint64_t ext0,
                              std::uint64_t ext1) override;
    bool newer(std::uint32_t name, std::uint64_t stamp) override;
    bool dead_name(std::uint32_t name) override;
    std::uint64_t epoch() override;
    void wait(std::uint64_t seen, std::chrono::steady_clock::duration limit) override;

    // Test and diagnostics: the number of live (active) ports and names.
    std::size_t live_names();

    // ---- across processes (mach_remote.cpp) ----------------------------------
    // Enables the cross-process layer: `broker` hosts the bootstrap registry
    // of the process tree (this process when it is the root).
    void connect(std::shared_ptr<Transport> transport, std::uint32_t broker);
    std::uint32_t self_pid() const { return self_; }
    // Bootstrap over the tree-wide registry (bootstrap status codes).
    std::uint32_t bootstrap_check_in(const std::string& service, Name& out);
    std::uint32_t bootstrap_look_up(const std::string& service, Name& out);
    std::uint32_t bootstrap_register(const std::string& service, Name port);
    // posix_spawn: special ports the child receives (task_get_special_port).
    void spawn_starting();
    void spawn_started(std::uint32_t pid, std::vector<std::pair<std::uint32_t, Right>> specials);
    // The child side: its parent's special ports for it (once).
    std::vector<std::pair<std::uint32_t, Right>> inherited_special_ports(std::uint32_t parent);
    // A routine on the task of process `pid` (Guest::task_call there); empty
    // when that process cannot be reached.
    std::optional<std::vector<std::uint8_t>> task_call(std::uint32_t pid, const std::vector<std::uint8_t>& call);
    // Transport::Handler (transport threads).
    void frame(std::uint32_t from, std::uint32_t type, Reader& payload) override;
    Transport::Payload request(std::uint32_t from, std::uint32_t type, Reader& payload) override;
    void peer_died(std::uint32_t pid) override;

private:
    static constexpr std::uint32_t first_index = 0x1000;
    using Lock = std::unique_lock<std::mutex>;

    struct Transaction;
    Entry* find(Name name);
    Name new_name();
    void erase(Name name);
    std::shared_ptr<Port> new_port();
    std::shared_ptr<Port> kernel_port(Name name);
    void touch();
    void enqueue(const std::shared_ptr<Port>& port, std::shared_ptr<Message> message);
    std::uint32_t copyin(Name name, std::uint8_t disposition, Transaction* transaction, Right& out);
    Name copyout(Right right);
    void release(Right right);
    void drop_send(const std::shared_ptr<Port>& port);
    void drop_send_once(const std::shared_ptr<Port>& port);
    void destroy_receive(const std::shared_ptr<Port>& port);
    void destroy_set(const std::shared_ptr<PortSet>& set);
    void discard(Message& message);
    void leave_sets(Port& port);
    void entry_vanished(Name name, Entry& entry);
    void notify(Right target, std::int32_t id, std::uint32_t value, Right carried = {});
    void send_possible(Port& port);
    std::uint32_t remove_send(Name name, Entry& entry, std::uint32_t count);
    std::uint32_t receive_locked(Lock& lock, transmute::GuestMemory& memory, Address buffer, std::uint32_t options,
                                 std::uint32_t size, Name name, std::optional<std::chrono::steady_clock::time_point> deadline,
                                 std::uint32_t* received);
    std::shared_ptr<Port> ready_port(Name name, std::uint32_t& error);
    std::uint64_t newest(Name name);
    void senders_changed(const std::shared_ptr<Port>& port);
    void kill_port(std::shared_ptr<Port> port);   // by value: it may erase the caller's table entry
    // Across processes (mach_remote.cpp).
    void retire_proxy(std::shared_ptr<Port> port);
    void write_right(Writer& out, Right right, std::uint32_t recipient, std::vector<std::shared_ptr<Port>>& migrated);
    Right read_right(Reader& in, std::uint32_t from);
    void transmit(std::shared_ptr<Message> message, const std::shared_ptr<Port>& destination, bool forced = false);
    bool grant(std::uint32_t host, Key key, bool once, std::uint32_t recipient);
    void lost_send_once(const std::shared_ptr<Port>& port, std::uint32_t from);
    void post_notification(const std::shared_ptr<Port>& proxy, std::vector<std::uint8_t> bytes);

    Guest guest_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::uint64_t epoch_{}, stamp_{}, port_ids_{};
    std::map<Name, Entry> entries_;
    std::map<Name, std::shared_ptr<Port>> kernel_ports_;
    std::uint32_t next_index_{first_index};
    std::vector<std::pair<std::uint32_t, std::uint8_t>> free_indices_;   // index, last generation
    // Across processes.
    std::shared_ptr<Transport> transport_;
    std::uint32_t self_{}, broker_{};
    std::map<Key, std::shared_ptr<Port>> hosted_;                         // keys this process answers for
    std::map<std::pair<std::uint32_t, Key>, std::shared_ptr<Port>> proxies_;   // (host, key) -> proxy
    std::map<std::string, std::pair<std::uint32_t, Key>> registry_;       // the broker's services
    std::map<std::uint32_t, std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>>> specials_;   // child -> wires
    std::chrono::steady_clock::time_point last_spawn_{};
    std::condition_variable specials_changed_;
};

} // namespace alchemy::libsystem::mach
