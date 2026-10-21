#include "alchemy/darwin/metal/bridge.hpp"

#include "alchemy/engine_adapter.hpp"
#include "alchemy/gui/runloop.hpp"
#include "alchemy/host/process.hpp"
#include "alchemy/host/sections.hpp"
#include "alchemy/host/sync.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

// MTLSharedEvent across processes (owner: g3, Chrome stage 4):
// -[MTLSharedEvent newSharedEventHandle], -[MTLDevice
// newSharedEventWithHandle:] and MTLSharedEventHandle (label, NSSecureCoding:
// the handle travels between processes as a keyed archive, e.g. over XPC or
// in a Mach message's bytes).
//
// Design. A shared event is a Vulkan timeline semaphore of its process's
// device (commands.hpp); one semaphore cannot be shared between Alchemy
// processes without OS handles, so an event that has a handle also gets a
// cell: a section named after the process tree and the event's ID (unique in
// the tree, like IOSurfaceIDs) holding the event's value as every process
// sees it and the processes that bridge it. Each process keeps its bridged
// events' semaphores and their cells equal, in both directions: a value this
// process reached (CPU setSignaledValue:, GPU encodeSignalEvent:value:) is
// published to the cell, and a value another process published is signalled
// on the local semaphore from the host (GPU encodeWaitForEvent:value: and
// waitUntilSignaledValue:timeoutMS: then proceed, and listeners waiting for
// it are notified). Two bridge threads block instead of polling: one in
// vkWaitSemaphores on the local semaphores (any of them passing its current
// value, or a control semaphore when the set changes), one on this process's
// named wake semaphore, which a process releases for every other subscriber
// of a cell it raised. Both also wake every 250 ms to notice a process stop.
// A signal crosses processes in about a thread wake-up (Metal promises no
// latency).
//
// Limits: values only increase (Metal's shared events are monotonic too); a
// process that has a GPU signal of the event pending must not see another
// process signal a higher value first (Vulkan forbids a host signal past a
// pending one; producer/consumer use, where one process signals and the
// other waits, never does). Within one process, newSharedEventWithHandle:
// answers the event the handle was made from while it lives. Events of a
// second VkDevice (Alchemy has one system device) are synchronized only on
// the 250 ms wake.
namespace alchemy::metal::bridge {
namespace {

constexpr auto bridge_wake = std::chrono::milliseconds(250);

// The process tree a section name is scoped to (the tree's Mach broker), as
// IOSurface's names are.
std::string tree_name() {
    return host::process::environment("ALCHEMY_MACH_BROKER").value_or(std::to_string(host::process::current_pid()));
}

// A cell's (or the ID counter's) 4 KiB named section, zero-filled:
//   +0   uint64 value;
//   +64  uint32 subscribers[64] (pids of the processes that bridge the event).
// Without host sections, process-local storage.
struct SharedWord {
    static constexpr std::size_t subscriber_count = 64;
    struct Layout {
        std::atomic<std::uint64_t> value{0};
        std::uint64_t reserved[7]{};
        std::atomic<std::uint32_t> subscribers[subscriber_count]{};
    };
    static_assert(sizeof(Layout) <= 4096 && offsetof(Layout, subscribers) == 64);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free && std::atomic<std::uint32_t>::is_always_lock_free);
    host::Handle handle{};
    std::uintptr_t view{};
    Layout local;
    Layout* layout{&local};
    std::atomic<std::uint64_t>* value{&local.value};
    explicit SharedWord(const std::string& name) {
        if (const auto named = host::sections::open_named(name, 4096)) {
            if (const auto mapped = host::sections::map(named->handle, 0, 4096, true)) {
                handle = named->handle;
                view = *mapped;
                layout = reinterpret_cast<Layout*>(*mapped);
                value = &layout->value;
            } else {
                host::close(named->handle);
            }
        }
    }
    SharedWord(const SharedWord&) = delete;
    SharedWord& operator=(const SharedWord&) = delete;
    ~SharedWord() {
        if (view) static_cast<void>(host::sections::unmap(view));
        if (handle) host::close(handle);
    }
    // True when the value grew.
    bool raise(std::uint64_t to) {
        auto current = value->load();
        while (current < to)
            if (value->compare_exchange_weak(current, to)) return true;
        return false;
    }
    void subscribe(std::uint32_t pid) {
        for (auto& slot : layout->subscribers)
            if (slot.load() == pid) return;
        for (auto& slot : layout->subscribers) {
            std::uint32_t free = 0;
            if (slot.compare_exchange_strong(free, pid)) return;
        }
        // Full: that process is still served by its 250 ms wake.
    }
};

std::uint32_t next_event_id() {
    static SharedWord counter("Alchemy.MTLSharedEventIDs." + tree_name());
    for (;;)
        if (const auto id = static_cast<std::uint32_t>(counter.value->fetch_add(1) + 1)) return id;   // 0 is no event
}

struct Cell {
    std::uint32_t id{};
    SharedWord word;
    explicit Cell(std::uint32_t event_id) : id(event_id), word("Alchemy.MTLSharedEvent." + tree_name() + "." + std::to_string(event_id)) {}
};

struct Bridged {
    std::weak_ptr<Event> event;
    Address object{};   // the guest MTLSharedEvent (not retained), for its listeners
    std::shared_ptr<Cell> cell;
};

std::string wake_name(std::uint32_t pid) { return "Alchemy.MTLSharedEventWake." + tree_name() + "." + std::to_string(pid); }

struct BridgeState {
    std::mutex mutex;
    std::vector<Bridged> events;
    bool started{};
    std::shared_ptr<Event> control;    // signalled when `events` changes (wakes the local waiter)
    host::Handle wake{};               // this process's named wake semaphore
    std::map<std::uint32_t, host::Handle> peers;   // other processes' wake semaphores
    std::mutex sync_mutex;             // one synchronize() at a time (host signals must grow)
    std::shared_ptr<gui::CompletionThread> local_thread, remote_thread;   // run for the process's life
};
BridgeState& bridge_state() {
    static BridgeState value;
    return value;
}

// Notifies the listeners of `object` when it still is the event `event`.
// On a bridge thread (a guest-capable thread); no host lock held.
void notify(Address object, const std::shared_ptr<Event>& event) {
    auto& r = runtime();
    if (!r.process.objects().try_retain(object)) return;
    try {
        if (optional_state<Held>(object) && event_of(object) == event) check_listeners(object);
    } catch (const std::exception&) {}   // the address names another object now
    r.release(object);
}

// Wakes every other process that bridges `cell`; forgets processes gone.
void wake_peers(Cell& cell) {
    auto& state = bridge_state();
    const auto self = host::process::current_pid();
    for (auto& slot : cell.word.layout->subscribers) {
        const auto pid = slot.load();
        if (!pid || pid == self) continue;
        host::Handle peer{};
        {
            std::lock_guard lock(state.mutex);
            if (const auto found = state.peers.find(pid); found != state.peers.end()) peer = found->second;
        }
        if (!peer) {
            const auto opened = host::sync::open_semaphore(wake_name(pid));
            if (!opened) {   // that process ended
                auto stale = pid;
                slot.compare_exchange_strong(stale, 0);
                continue;
            }
            std::lock_guard lock(state.mutex);
            auto [entry, inserted] = state.peers.emplace(pid, *opened);
            if (!inserted) host::close(*opened);
            peer = entry->second;
        }
        static_cast<void>(host::sync::release_semaphore(peer, 1));   // at its maximum: it is awake anyway
    }
}

// One pass over the bridged events: publishes local values, signals remote
// ones, then wakes peers and notifies listeners (outside the locks).
void synchronize() {
    auto& state = bridge_state();
    std::vector<Bridged> events;
    {
        std::lock_guard lock(state.mutex);
        std::erase_if(state.events, [](const Bridged& b) { return b.event.expired(); });
        events = state.events;
    }
    std::vector<std::shared_ptr<Cell>> raised;
    std::vector<std::pair<Address, std::shared_ptr<Event>>> arrived;
    {
        std::lock_guard sync(state.sync_mutex);
        for (const auto& b : events) {
            const auto event = b.event.lock();
            if (!event) continue;
            const auto local = event->signaled_value();
            const auto shared = b.cell->word.value->load();
            if (local > shared) {
                if (b.cell->word.raise(local)) raised.push_back(b.cell);
            } else if (shared > local) {
                event->signal(shared);
                arrived.emplace_back(b.object, event);
            }
        }
    }
    for (const auto& cell : raised) wake_peers(*cell);
    for (const auto& [object, event] : arrived) notify(object, event);
}

// The local waiter: blocks until a bridged semaphore passes its current value
// or the set changes (the control semaphore), at most bridge_wake.
void wait_local() {
    auto& state = bridge_state();
    std::shared_ptr<Event> control;
    std::vector<std::shared_ptr<Event>> events;
    {
        std::lock_guard lock(state.mutex);
        control = state.control;
        for (const auto& b : state.events)
            if (auto event = b.event.lock(); event && event->device == control->device) events.push_back(std::move(event));
    }
    std::vector<VkSemaphore> semaphores{control->semaphore};
    std::vector<std::uint64_t> values{control->signaled_value() + 1};
    for (const auto& event : events) {
        semaphores.push_back(event->semaphore);
        values.push_back(event->signaled_value() + 1);
    }
    VkSemaphoreWaitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    info.flags = VK_SEMAPHORE_WAIT_ANY_BIT;
    info.semaphoreCount = static_cast<std::uint32_t>(semaphores.size());
    info.pSemaphores = semaphores.data();
    info.pValues = values.data();
    const auto& device = *control->device;
    const auto result = device.vk.vkWaitSemaphores(device.device, &info,
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(bridge_wake).count()));
    if (result != VK_SUCCESS && result != VK_TIMEOUT) check(result, "vkWaitSemaphores (MTLSharedEvent bridge)");
}

void run_bridge(void (*wait)()) {
    auto& process = runtime().process;
    for (;;) {
        try {
            wait();
            synchronize();
        } catch (const std::exception& failure) {
            std::fprintf(stderr, "[alchemy metal] MTLSharedEvent bridge: %s\n", failure.what());
            std::this_thread::sleep_for(bridge_wake);
        }
        if (engine_stop_requested(process.calling_engine())) return;
    }
}

// Publishes `event` (guest `object`) under `cell` and subscribes this
// process to it, starting the bridge threads on first use. On a guest thread
// inside a synthetic call.
void bridge(Address object, const std::shared_ptr<Event>& event, std::shared_ptr<Cell> cell) {
    auto& state = bridge_state();
    bool start = false;
    std::shared_ptr<Event> control;
    cell->word.subscribe(host::process::current_pid());
    {
        std::lock_guard lock(state.mutex);
        for (const auto& b : state.events)
            if (b.event.lock() == event && b.cell->id == cell->id) return;
        state.events.push_back({event, object, std::move(cell)});
        if (!state.started) {
            state.started = start = true;
            state.control = Event::create(event->device, false);
            if (const auto wake = host::sync::create_semaphore(wake_name(host::process::current_pid()), 0, 1U << 20)) state.wake = *wake;
        }
        control = state.control;
    }
    if (!start) {
        control->signal(control->signaled_value() + 1);   // the local waiter waits on the new event too
        return;
    }
    auto& process = runtime().process;
    auto local = gui::start_completion_thread(process, "MTLSharedEvent bridge (local)");
    auto remote = gui::start_completion_thread(process, "MTLSharedEvent bridge (remote)");
    {
        std::lock_guard lock(state.mutex);
        state.local_thread = local;
        state.remote_thread = remote;
    }
    local->post([] { run_bridge(wait_local); });
    remote->post([] {
        run_bridge([] {
            const auto wake = bridge_state().wake;
            if (!wake) { std::this_thread::sleep_for(bridge_wake); return; }   // no named semaphore: the timer only
            const auto handles = std::array<host::Handle, 1>{wake};
            static_cast<void>(host::sync::wait_any(handles,
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(bridge_wake).count())));
        });
    });
}

// The cell of an event that already has one (its first handle made it).
std::shared_ptr<Cell> cell_of(const std::shared_ptr<Event>& event) {
    auto& state = bridge_state();
    std::lock_guard lock(state.mutex);
    for (const auto& b : state.events)
        if (b.event.lock() == event) return b.cell;
    return nullptr;
}

// An event of this process by ID: the guest object (+1) when it lives.
Address local_event(std::uint32_t id) {
    auto& r = runtime();
    std::vector<Bridged> events;
    {
        auto& state = bridge_state();
        std::lock_guard lock(state.mutex);
        events = state.events;
    }
    for (const auto& b : events) {
        const auto event = b.event.lock();
        if (!event || b.cell->id != id || !r.process.objects().try_retain(b.object)) continue;
        try {
            if (event_of(b.object) == event) return b.object;
        } catch (const std::exception&) {}
        r.release(b.object);
    }
    return 0;
}

struct HandleState final : Held { std::uint32_t id{}; };

constexpr std::string_view id_key = "AlchemyMTLSharedEventID";   // the handle's keyed-archive key

Address new_handle(Address cls, std::uint32_t id, Address label) {
    auto& r = runtime();
    auto value = std::make_shared<HandleState>();
    value->id = id;
    if (label) value->label = r.objc.send(label, "copy").x0;
    return r.create(cls, std::move(value));
}

} // namespace

void install_shared_events(Runtime& r) {
    const auto device = r.objc.find_class("AlchemyMTLDevice");
    const auto shared_event = r.objc.find_class("AlchemyMTLSharedEvent");
    if (!device || !shared_event) throw std::runtime_error("Alchemy Metal: install_shared_events before the device and event classes");
    const auto handle = r.define_class("MTLSharedEventHandle");
    r.objc.add_native_protocol(handle, "NSSecureCoding");
    r.objc.add_native_protocol(handle, "NSCoding");
    install_label(handle);
    r.install_dealloc(handle);

    // -[MTLSharedEvent newSharedEventHandle] (+1): the event gets its cell
    // (and so its ID) with its first handle; later handles name the same.
    r.method(shared_event, "newSharedEventHandle", [handle](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto event = event_of(self);
        auto cell = cell_of(event);
        if (!cell) {
            cell = std::make_shared<Cell>(next_event_id());
            cell->word.raise(event->signaled_value());
            bridge(self, event, cell);
        }
        s.x[0] = new_handle(handle, cell->id, runtime().objc.send(self, "label").x0);
    }, false, "@@:");
    // -[MTLDevice newSharedEventWithHandle:] (+1): the handle's event of this
    // process while it lives, else a new event of this device bridged to the
    // handle's cell (starting at its current value). nil for other objects.
    r.method(device, "newSharedEventWithHandle:", [](CpuState& s, GuestMemory&) {
        auto& rt = runtime();
        const auto value = optional_state<HandleState>(s.x[2]);
        if (!value || !value->id) { s.x[0] = 0; return; }
        if (const auto existing = local_event(value->id)) { s.x[0] = existing; return; }
        const auto object = rt.objc.send(s.x[0], "newSharedEvent").x0;
        if (!object) { s.x[0] = 0; return; }
        const auto event = event_of(object);
        auto cell = std::make_shared<Cell>(value->id);
        event->signal(cell->word.value->load());
        bridge(object, event, std::move(cell));
        if (value->label) static_cast<void>(rt.objc.send(object, "setLabel:", [&] {
            CallRegs registers;
            registers.x[2] = value->label;
            return registers;
        }()));
        s.x[0] = object;
    }, false, "@@:@");

    // NSSecureCoding: the ID and the label.
    r.method(handle, "supportsSecureCoding", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, true, "B@:");
    r.method(handle, "encodeWithCoder:", [](CpuState& s, GuestMemory&) {
        auto& rt = runtime();
        const auto value = state<HandleState>(s.x[0], "MTLSharedEventHandle");
        CallRegs id;
        id.x[2] = value->id;
        id.x[3] = rt.string(id_key);
        static_cast<void>(rt.objc.send(s.x[2], "encodeInt64:forKey:", id));
        if (value->label) {
            CallRegs label;
            label.x[2] = value->label;
            label.x[3] = rt.string("label");
            static_cast<void>(rt.objc.send(s.x[2], "encodeObject:forKey:", label));
        }
    }, false, "v@:@");
    r.method(handle, "initWithCoder:", [](CpuState& s, GuestMemory&) {
        auto& rt = runtime();
        const auto self = s.x[0], coder = s.x[2];
        CallRegs id;
        id.x[2] = rt.string(id_key);
        const auto decoded = static_cast<std::uint32_t>(rt.objc.send(coder, "decodeInt64ForKey:", id).x0);
        if (!decoded) {   // not an archived handle: init fails (nil)
            rt.release(self);
            s.x[0] = 0;
            return;
        }
        CallRegs label;
        label.x[2] = rt.string("label");
        const auto text = rt.objc.send(coder, "decodeObjectForKey:", label).x0;
        auto value = rt.objc.host_state<HandleState>(self, [] { return std::make_shared<HandleState>(); });
        value->id = decoded;
        if (text) value->label = rt.retain(text);
        s.x[0] = self;
    }, false, "@@:@");
}

} // namespace alchemy::metal::bridge
