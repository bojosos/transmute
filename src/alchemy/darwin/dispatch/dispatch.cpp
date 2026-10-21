#include "alchemy/darwin_dispatch.hpp"
#include "alchemy/host/process.hpp"
#include "alchemy/host/system.hpp"
#include "alchemy/host/time.hpp"
#include "alchemy/darwin_threads.hpp"
#include "alchemy/engine_adapter.hpp"
#include "alchemy/mach_port_events.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>


namespace alchemy {
namespace {
constexpr std::string_view system_library = "/usr/lib/libSystem.B.dylib";
constexpr std::uint64_t forever = UINT64_MAX;
constexpr std::uint64_t wall_now = UINT64_MAX - 1;
constexpr std::uint64_t wall_bit = 1ULL << 63;
constexpr std::uint64_t maximum_time = wall_bit - 3;
constexpr std::uint64_t queue_marker = 0x414c434851554555ULL;
constexpr std::uint64_t group_marker = 0x414c434847525550ULL;
constexpr std::uint64_t source_marker = 0x414c434853524345ULL;
constexpr std::uint64_t semaphore_marker = 0x414c4348454d5953ULL;
constexpr auto poll_period = std::chrono::milliseconds(10);
// Idle dispatch workers and the timer thread wait this long for new work
// before their guest pthread ends; bursts reuse them instead of starting one
// guest thread per idle-to-busy transition.
constexpr auto idle_period = std::chrono::seconds(1);
constexpr std::uint64_t timer_slice = 50'000'000;   // ns; bounds wall-clock drift and exit latency

std::uint64_t now(bool wall) {
    if (wall) return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    const auto value = host::time::monotonic_ns();
    if (value > maximum_time) throw std::runtime_error("dispatch monotonic clock overflow");
    return value;
}

std::uint64_t deadline(std::uint64_t base, std::int64_t delta, bool wall) {
    if (base > maximum_time) return forever;
    std::uint64_t value;
    if (delta >= 0) {
        const auto amount = static_cast<std::uint64_t>(delta);
        if (amount > maximum_time - base) return forever;
        value = base + amount;
    } else {
        const auto amount = static_cast<std::uint64_t>(-(delta + 1)) + 1;
        value = amount >= base ? 1 : base - amount;
    }
    return std::max<std::uint64_t>(1, value) | (wall ? wall_bit : 0);
}

// Nanoseconds until an encoded dispatch_time_t deadline; zero when due.
std::uint64_t remaining(std::uint64_t when) {
    if (when == 0) return 0;
    if (when == forever) return forever;
    const bool wall = (when & wall_bit) != 0;
    const auto at = when & ~wall_bit, current = now(wall);
    return current >= at ? 0 : at - current;
}

struct Dispatch;
// install_darwin_dispatch's instance per process, for the GUI run loop's
// main-queue service (drain_dispatch_main_queue).
struct Registry {
    std::mutex mutex;
    std::map<const ProcessContext*, std::weak_ptr<Dispatch>> processes;
};
Registry& registry() { static Registry value; return value; }

struct Dispatch : std::enable_shared_from_this<Dispatch> {
    struct Waiter { std::condition_variable changed; bool signaled{}; };
    struct Semaphore {
        std::mutex gate;
        std::deque<std::shared_ptr<Waiter>> waiters;
        std::uint64_t permits{}, references{1};
        bool retired{};
    };
    struct Group;
    // dispatch_sync / dispatch_barrier_sync onto a private concurrent queue
    // that must first wait for a barrier: a marker item a drainer grants, after
    // which the caller runs its work inline.
    struct Gate { bool granted{}; };
    // A queued function call. Block work owns one _Block_copy reference.
    struct Work {
        std::uint64_t function{}, context{}, block{};
        std::shared_ptr<bool> done;     // set when a waiting dispatch_sync completes
        std::shared_ptr<Group> group;   // left after the work ran (dispatch_group_async)
        std::function<void()> action;   // host work (source handlers) instead of function/context
        std::shared_ptr<Gate> gate;     // sync marker, see Gate
        bool barrier{};
    };
    enum class Kind { serial, concurrent, main };
    struct Queue {
        Kind kind{};
        std::uint64_t label{};
        std::deque<Work> items;
        unsigned active{};        // drainer activations attached to this queue
        unsigned executing{};     // concurrent: drainers running an item
        unsigned running{};       // concurrent: items running (drainers and inline syncs)
        bool barrier_running{};
        std::uint64_t owner{};    // serial: identity of the thread running an item
        std::uint64_t suspended{}, context{};
        std::uint64_t references{1};
        bool immortal{}, released{};
        std::condition_variable changed;
        // dispatch_queue_set_specific: key -> {context, destructor}.
        std::map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>> specifics;
    };
    struct Group {
        std::uint64_t address{}, count{}, references{1};
        std::vector<std::pair<std::uint64_t, Work>> notifications;   // target queue (retained), work
        bool released{};
    };
    enum class SourceType { timer, data_add, data_or, data_replace, mach_recv, mach_send, memory_pressure, proc, read, write, vnode, unsupported };
    struct Handler { std::uint64_t function{}, block{}; explicit operator bool() const { return function || block; } };
    struct Source {
        std::uint64_t address{}, handle{}, mask{}, queue{}, context{};
        SourceType type{};
        std::string type_name;
        Handler event, cancel, registration;
        std::uint64_t pending{}, data{};
        std::uint64_t suspended{}, references{1};
        unsigned invoking{};             // handlers running now
        std::vector<Handler> retired;    // released when invoking drops to zero
        std::uint64_t interval{}, generation{};
        bool inactive{true}, event_queued{}, canceled{}, cancel_delivered{}, registered{}, disposed{};
        bool dead_reported{};            // MACH_SEND: DISPATCH_MACH_SEND_DEAD was delivered; PROC: EXIT was
        bool known_child{};              // PROC: the pid was a child of this process at creation
    };
    struct TimerTask { std::uint64_t when{}; std::function<void()> fire, drop; };
    struct Task { std::uint64_t id{}; std::function<void()> run; };

    explicit Dispatch(ProcessContext& context) : process(context) {}
    ProcessContext& process;
    std::mutex metadata;
    // Objective-C classes of dispatch objects (set_dispatch_object_classes);
    // zero until a provider registers them (the isa word then holds a marker).
    enum class ObjectClass : unsigned { serial, concurrent, main, global, attribute, group, source, semaphore, count };
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(ObjectClass::count)> classes{};
    // add_dispatch_reference_handler, oldest first; asked newest first.
    std::vector<DispatchReferenceHandler> reference_handlers;
    void retain_or_release(std::uint64_t address, bool retain) {
        std::vector<DispatchReferenceHandler> handlers;
        { std::lock_guard lock(metadata); handlers = reference_handlers; }
        for (auto handler = handlers.rbegin(); handler != handlers.rend(); ++handler)
            if ((*handler)(address, retain)) return;
        reference(address, retain);
    }
    // Queue-specific destructors of released queues, run outside `metadata`.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> pending_destructors;
    std::map<std::uint64_t, std::shared_ptr<Semaphore>> semaphores;
    std::map<std::uint64_t, std::shared_ptr<Queue>> queues;
    std::map<std::uint64_t, std::shared_ptr<Group>> groups;
    std::map<std::uint64_t, std::shared_ptr<Source>> sources;
    std::map<std::uint64_t, std::pair<SourceType, std::string>> source_types;
    std::condition_variable group_changed;
    std::map<std::uint64_t, std::uint64_t> draining; // thread identity -> queue being drained
    std::array<std::uint64_t, 3> global{};
    std::uint64_t main_queue{}, concurrent_attribute{}, serial_attribute{};
    std::uint64_t worker_entry{}, timer_entry{};
    // Worker pool: idle guest workers take host tasks (queue drains, apply
    // helpers); a new guest pthread starts only when no idle worker can.
    std::deque<Task> tasks;
    unsigned waiting_workers{};
    std::uint64_t next_task{1};
    std::condition_variable pool_changed;
    // One timer guest thread serves dispatch_after and timer sources.
    std::map<std::uint64_t, TimerTask> timers;
    std::uint64_t next_timer{1};
    bool timer_running{};
    bool mach_watching{};            // the Mach source watcher task runs
    bool polling{};                  // the memory-pressure / process poll timer is armed
    std::int64_t kqueue{-1};         // READ/WRITE/VNODE sources: their kqueue (udata = source)
    bool kqueue_watching{};          // the kqueue watcher task runs
    std::uint64_t pressure_level{};  // the last DISPATCH_MEMORYPRESSURE_* level seen (0: not sampled)
    std::condition_variable timer_changed;

    std::uint64_t symbol(std::string_view name) const {
        const auto value = process.resolve(system_library, name);
        if (!value) throw std::runtime_error("Alchemy dispatch requires " + std::string(name));
        return value->address;
    }
    std::uint64_t call(std::string_view name, std::initializer_list<std::uint64_t> arguments) {
        return EngineAdapter(process.calling_engine()).call(symbol(name), std::span(arguments.begin(), arguments.size()));
    }
    std::uint64_t call_address(std::uint64_t address, std::initializer_list<std::uint64_t> arguments) {
        return EngineAdapter(process.calling_engine()).call(address, std::span(arguments.begin(), arguments.size()));
    }
    std::uint64_t identity() { return process.current_thread()->identity; }
    bool exiting() const { return process.requested_exit().has_value(); }
    // Blocked dispatch waits end when the process exits: workers unwind, the
    // main thread reports the missing engine stop.
    [[noreturn]] void stop(std::string_view operation) {
        if (guest_worker_must_stop(process)) throw GuestThreadStop{};
        EngineAdapter::require("cooperative guest stop during " + std::string(operation));
    }
    std::uint64_t isa(ObjectClass kind, std::uint64_t marker) const {
        const auto value = classes[static_cast<std::size_t>(kind)].load(std::memory_order_acquire);
        return value ? value : marker;
    }
    // The first word is the Objective-C isa once the classes are registered
    // (dispatch objects are OS_dispatch_* objects under OS_OBJECT_USE_OBJC).
    std::uint64_t marked_object(std::uint64_t marker, ObjectClass kind, std::uint64_t size = 16) {
        const auto address = process.allocate_guest(size);
        try {
            // Guest object storage contains no Windows pointers or runtime objects.
            process.calling_engine().fill_bytes(address, 0, size);
            process.calling_engine().write64(address, isa(kind, marker));
        } catch (...) { process.release_guest(address); throw; }
        return address;
    }
    static ObjectClass queue_class(const Queue& value) {
        if (value.kind == Kind::main) return ObjectClass::main;
        if (value.kind == Kind::serial) return ObjectClass::serial;
        return value.immortal ? ObjectClass::global : ObjectClass::concurrent;
    }
    void set_classes(const DispatchObjectClasses& value) {
        const std::array<std::uint64_t, static_cast<std::size_t>(ObjectClass::count)> list{
            value.serial_queue, value.concurrent_queue, value.main_queue, value.global_queue,
            value.queue_attribute, value.group, value.source, value.semaphore};
        for (std::size_t index = 0; index < list.size(); ++index) classes[index].store(list[index], std::memory_order_release);
        auto& memory = process.calling_engine();
        std::lock_guard lock(metadata);
        for (const auto& [address, queue] : queues) memory.write64(address, isa(queue_class(*queue), queue_marker));
        for (const auto& [address, group] : groups) { static_cast<void>(group); memory.write64(address, isa(ObjectClass::group, group_marker)); }
        for (const auto& [address, source] : sources) { static_cast<void>(source); memory.write64(address, isa(ObjectClass::source, source_marker)); }
        for (const auto& [address, semaphore] : semaphores) { static_cast<void>(semaphore); memory.write64(address, isa(ObjectClass::semaphore, semaphore_marker)); }
        for (const auto attribute : {concurrent_attribute, serial_attribute})
            if (attribute) memory.write64(attribute, isa(ObjectClass::attribute, 0));
    }
    std::optional<DispatchObjectInfo> info(std::uint64_t address) {
        std::lock_guard lock(metadata);
        if (const auto found = queues.find(address); found != queues.end() && !found->second->released) {
            const auto& value = *found->second;
            DispatchObjectInfo result;
            result.kind = value.kind == Kind::main ? "queue-main" : value.kind == Kind::serial ? "queue-serial" :
                          value.immortal ? "queue-global" : "queue-concurrent";
            if (value.label) {
                const auto bytes = process.calling_engine().read_string(value.label, 1, 4096);
                result.label.assign(bytes.begin(), bytes.end());
            }
            result.references = value.references;
            result.immortal = value.immortal;
            return result;
        }
        if (const auto found = groups.find(address); found != groups.end() && !found->second->released)
            return DispatchObjectInfo{"group", {}, found->second->references, false};
        if (const auto found = sources.find(address); found != sources.end())
            return DispatchObjectInfo{"source", found->second->type_name, found->second->references, false};
        if (const auto found = semaphores.find(address); found != semaphores.end())
            return DispatchObjectInfo{"semaphore", {}, found->second->references, false};
        if (address && (address == concurrent_attribute || address == serial_attribute))
            return DispatchObjectInfo{"queue-attr", address == concurrent_attribute ? "concurrent" : "serial", 1, true};
        return std::nullopt;
    }

    // ---- queue-specific data ------------------------------------------------------
    // The queue whose work runs on this thread: the queue being drained or
    // synchronously entered, else the main queue on the main thread.
    std::uint64_t current_queue() {
        const auto self = identity();
        std::lock_guard lock(metadata);
        if (const auto found = draining.find(self); found != draining.end()) return found->second;
        return &process.calling_engine() == &process.main_engine() ? main_queue : 0;
    }
    // Marks `address` as this thread's current queue while synchronous work
    // runs inline (dispatch_sync), restoring the previous one afterwards.
    struct Entered {
        Dispatch& owner;
        std::uint64_t self{}, previous{};
        bool had{};
        Entered(Dispatch& dispatch, std::uint64_t address) : owner(dispatch), self(dispatch.identity()) {
            std::lock_guard lock(owner.metadata);
            if (const auto found = owner.draining.find(self); found != owner.draining.end()) { had = true; previous = found->second; }
            owner.draining[self] = address;
        }
        ~Entered() {
            std::lock_guard lock(owner.metadata);
            if (had) owner.draining[self] = previous; else owner.draining.erase(self);
        }
        Entered(const Entered&) = delete;
        Entered& operator=(const Entered&) = delete;
    };
    void set_specific(std::uint64_t address, std::uint64_t key, std::uint64_t context, std::uint64_t destructor) {
        if (!key) return;   // NULL keys are ignored, as by libdispatch
        std::pair<std::uint64_t, std::uint64_t> previous{};
        {
            std::lock_guard lock(metadata);
            const auto found = queues.find(address);
            if (found == queues.end() || found->second->released) throw std::runtime_error("dispatch_queue_set_specific of an invalid queue");
            auto& table = found->second->specifics;
            if (const auto old = table.find(key); old != table.end()) { previous = old->second; table.erase(old); }
            if (context) table.emplace(key, std::pair{context, destructor});
        }
        // The destructor of a replaced value runs with its context.
        if (previous.first && previous.second) static_cast<void>(call_address(previous.second, {previous.first}));
    }
    std::uint64_t get_specific(std::uint64_t address, std::uint64_t key) {
        if (!key || !address) return 0;
        std::lock_guard lock(metadata);
        const auto found = queues.find(address);
        if (found == queues.end() || found->second->released) return 0;
        const auto value = found->second->specifics.find(key);
        return value == found->second->specifics.end() ? 0 : value->second.first;
    }
    void run_destructors() {
        std::vector<std::pair<std::uint64_t, std::uint64_t>> pending;
        { std::lock_guard lock(metadata); pending.swap(pending_destructors); }
        for (const auto& [context, destructor] : pending) static_cast<void>(call_address(destructor, {context}));
    }

    std::shared_ptr<Semaphore> lookup(std::uint64_t address) {
        std::lock_guard lock(metadata);
        const auto found = semaphores.find(address);
        if (found == semaphores.end()) throw std::runtime_error("invalid Alchemy dispatch semaphore handle");
        return found->second;
    }

    std::uint64_t create(std::int64_t count) {
        if (count < 0) return 0;
        auto semaphore = std::make_shared<Semaphore>();
        semaphore->permits = static_cast<std::uint64_t>(count);
        const auto address = marked_object(semaphore_marker, ObjectClass::semaphore);
        try { std::lock_guard lock(metadata); semaphores.emplace(address, std::move(semaphore)); }
        catch (...) { process.release_guest(address); throw; }
        return address;
    }

    void reference(std::uint64_t address, bool retain) {
        bool destroy{};
        std::shared_ptr<Source> disposed;
        {
            std::lock_guard lock(metadata);
            if (const auto queue = queues.find(address); queue != queues.end()) {
                auto& value = *queue->second;
                if (value.immortal) return; // Global and main queues ignore retain/release.
                if (retain) {
                    if (value.references == UINT64_MAX) throw std::runtime_error("dispatch reference count overflow");
                    ++value.references;
                } else {
                    if (value.references == 0) throw std::runtime_error("over-released Alchemy dispatch queue");
                    if (--value.references == 0) {
                        if (value.suspended) throw std::runtime_error("dispatch queue released while suspended");
                        value.released = true; retire(address, value);
                    }
                }
                return;
            }
            if (const auto group = groups.find(address); group != groups.end()) {
                auto& value = *group->second;
                if (retain) {
                    if (value.references == UINT64_MAX) throw std::runtime_error("dispatch reference count overflow");
                    ++value.references;
                } else if (--value.references == 0) { value.released = true; retire_group(value); }
                return;
            }
            if (const auto source = sources.find(address); source != sources.end()) {
                auto& value = *source->second;
                if (retain) {
                    if (value.references == UINT64_MAX) throw std::runtime_error("dispatch reference count overflow");
                    ++value.references;
                } else if (--value.references == 0) {
                    if (value.inactive || value.suspended)
                        throw std::runtime_error("dispatch source released while inactive or suspended");
                    // The last reference ends an uncancelled source: its timer
                    // stops and no further handlers are submitted.
                    value.disposed = value.canceled = true; ++value.generation;
                    disposed = source->second;
                    sources.erase(source);
                }
                if (!disposed) return;
            } else {
                const auto found = semaphores.find(address);
                if (found == semaphores.end()) throw std::runtime_error("invalid Alchemy dispatch object handle");
                const auto selected = found->second;
                auto& value = *selected;
                std::lock_guard semaphore_lock(value.gate);
                if (retain) {
                    if (value.references == UINT64_MAX) throw std::runtime_error("dispatch reference count overflow");
                    ++value.references;
                } else {
                    if (value.references == 1 && !value.waiters.empty())
                        throw std::runtime_error("dispatch semaphore released with active waiters");
                    if (--value.references == 0) { value.retired = true; semaphores.erase(found); destroy = true; }
                }
            }
        }
        run_destructors();
        if (disposed) {
            // Handlers that are not queued are released now; a queued cancel
            // handler releases them itself after it ran.
            if (!disposed->cancel_delivered) release_handlers(*disposed);
            reference(disposed->queue, false);
            process.release_guest(address);
            return;
        }
        if (destroy) process.release_guest(address);
    }

    std::uint64_t signal(std::uint64_t address) {
        const auto value = lookup(address);
        std::shared_ptr<Waiter> selected;
        {
            std::lock_guard lock(value->gate);
            if (value->retired) throw std::runtime_error("released Alchemy dispatch semaphore handle");
            if (!value->waiters.empty()) {
                selected = value->waiters.front();
                value->waiters.pop_front();
                selected->signaled = true;
            } else {
                if (value->permits == static_cast<std::uint64_t>(INT64_MAX))
                    throw std::runtime_error("dispatch semaphore count overflow");
                ++value->permits;
            }
        }
        if (selected) selected->changed.notify_one();
        return selected ? 1 : 0;
    }

    std::uint64_t wait(std::uint64_t address, std::uint64_t timeout) {
        const auto value = lookup(address);
        auto waiter = std::make_shared<Waiter>();
        std::unique_lock lock(value->gate);
        if (value->retired) throw std::runtime_error("released Alchemy dispatch semaphore handle");
        if (value->permits) { --value->permits; return 0; }
        if (timeout == 0) return 1;
        value->waiters.push_back(waiter);
        const auto remove = [&] { std::erase(value->waiters, waiter); };
        try {
            while (!waiter->signaled) {
                if (exiting()) { remove(); stop("dispatch semaphore wait"); }
                std::uint64_t interval = 10'000'000;
                if (timeout != forever) {
                    const auto left = remaining(timeout);
                    if (!left) { remove(); return 1; }
                    interval = std::min(interval, left);
                }
                waiter->changed.wait_for(lock, std::chrono::nanoseconds(interval));
            }
        } catch (...) { remove(); throw; }
        return 0;
    }

    // ---- worker pool ----------------------------------------------------------
    // Starts a detached guest pthread at a private synthetic entry. False when
    // the process is already exiting, which drops the pending work like Darwin.
    bool spawn(std::uint64_t entry, std::uint64_t argument) {
        const auto cell = process.allocate_guest(8);
        struct Cell { ProcessContext& process; std::uint64_t address; ~Cell() { process.release_guest(address); } } owned{process, cell};
        process.calling_engine().write64(cell, 0);
        const auto result = static_cast<std::uint32_t>(call("_pthread_create", {cell, 0, entry, argument}));
        if (result == 35 && exiting()) return false;
        if (result) throw std::runtime_error("dispatch could not start a worker thread (errno " + std::to_string(result) + ")");
        static_cast<void>(call("_pthread_detach", {process.calling_engine().read64(cell)}));
        return true;
    }
    unsigned concurrency() const { return std::max(1U, std::min(64U, std::thread::hardware_concurrency())); }
    bool withdraw(std::uint64_t id) {
        std::lock_guard lock(metadata);
        const auto found = std::find_if(tasks.begin(), tasks.end(), [id](const Task& task) { return task.id == id; });
        if (found == tasks.end()) return false;
        tasks.erase(found);
        return true;
    }
    // Hands `run` to an idle worker or a new one. False when the task will not
    // run (the process is exiting).
    bool start_task(std::function<void()> run) {
        std::uint64_t id;
        bool spawn_worker;
        {
            std::lock_guard lock(metadata);
            id = next_task++;
            tasks.push_back({id, std::move(run)});
            spawn_worker = tasks.size() > waiting_workers;
        }
        if (!spawn_worker) { pool_changed.notify_one(); return true; }
        bool started{};
        try { started = spawn(worker_entry, 0); }
        catch (...) { if (withdraw(id)) throw; return true; }
        return started || !withdraw(id);
    }
    void worker() {
        std::unique_lock lock(metadata);
        for (;;) {
            if (!tasks.empty()) {
                auto task = std::move(tasks.front());
                tasks.pop_front();
                lock.unlock();
                task.run();
                lock.lock();
                continue;
            }
            if (exiting()) return;
            ++waiting_workers;
            const auto until = std::chrono::steady_clock::now() + idle_period;
            while (tasks.empty() && !exiting() && std::chrono::steady_clock::now() < until)
                pool_changed.wait_for(lock, poll_period);
            --waiting_workers;
            if (tasks.empty()) return;
        }
    }

    // ---- timer thread -----------------------------------------------------------
    void schedule_timer(std::uint64_t when, std::function<void()> fire, std::function<void()> drop) {
        bool start{};
        {
            std::lock_guard lock(metadata);
            timers.emplace(next_timer++, TimerTask{when, std::move(fire), std::move(drop)});
            if (!timer_running) start = timer_running = true;
        }
        timer_changed.notify_all();
        if (!start) return;
        bool started{};
        try { started = spawn(timer_entry, 0); }
        catch (...) { std::lock_guard lock(metadata); timer_running = false; throw; }
        if (!started) { std::lock_guard lock(metadata); timer_running = false; }
    }
    void timer_service() {
        std::unique_lock lock(metadata);
        auto idle_since = std::chrono::steady_clock::now();
        for (;;) {
            if (exiting()) {
                auto pending = std::move(timers);
                timers.clear();
                timer_running = false;
                lock.unlock();
                for (auto& [id, task] : pending) { static_cast<void>(id); if (task.drop) task.drop(); }
                return;
            }
            std::vector<TimerTask> due;
            std::uint64_t wait = timer_slice;
            for (auto entry = timers.begin(); entry != timers.end();) {
                const auto left = remaining(entry->second.when);
                if (!left) { due.push_back(std::move(entry->second)); entry = timers.erase(entry); }
                else { wait = std::min(wait, left); ++entry; }
            }
            if (!due.empty()) {
                lock.unlock();
                try { for (auto& task : due) task.fire(); }
                catch (...) { lock.lock(); timer_running = false; throw; }
                lock.lock();
                idle_since = std::chrono::steady_clock::now();
                continue;
            }
            if (!timers.empty()) idle_since = std::chrono::steady_clock::now();
            else if (std::chrono::steady_clock::now() - idle_since >= idle_period) { timer_running = false; return; }
            timer_changed.wait_for(lock, std::chrono::nanoseconds(wait));
        }
    }

    // ---- queues -------------------------------------------------------------
    std::uint64_t new_queue(Kind kind, std::string_view label, bool immortal) {
        auto queue = std::make_shared<Queue>();
        queue->kind = kind; queue->immortal = immortal;
        const auto address = marked_object(queue_marker, queue_class(*queue), 128);
        try {
            transmute::GuestMemory& engine = process.calling_engine();
            queue->label = process.allocate_guest(label.size() + 1);
            engine.write_bytes(queue->label, {reinterpret_cast<const std::uint8_t*>(label.data()), label.size()});
            engine.fill_bytes(queue->label + label.size(), 0, 1);
            std::lock_guard lock(metadata);
            queues.emplace(address, std::move(queue));
        } catch (...) {
            if (queue->label) process.release_guest(queue->label);
            process.release_guest(address); throw;
        }
        return address;
    }
    std::shared_ptr<Queue> queue(std::uint64_t address) {
        std::lock_guard lock(metadata);
        const auto found = queues.find(address);
        if (found == queues.end() || found->second->released) throw std::runtime_error("invalid Alchemy dispatch queue handle");
        return found->second;
    }
    bool is_queue(std::uint64_t address) {
        std::lock_guard lock(metadata);
        const auto found = queues.find(address);
        return found != queues.end() && !found->second->released;
    }
    // Caller holds metadata. Frees a released queue once nothing references it.
    void retire(std::uint64_t address, Queue& value) {
        if (!value.released || !value.items.empty() || value.active || value.owner || value.running) return;
        for (const auto& [key, entry] : value.specifics) {
            static_cast<void>(key);
            if (entry.second) pending_destructors.push_back(entry);
        }
        value.specifics.clear();
        const auto label = value.label;
        queues.erase(address);
        process.release_guest(label);
        process.release_guest(address);
    }
    Work block_work(std::uint64_t block) {
        if (!block) throw std::runtime_error("dispatch received a null block");
        const auto copy = call("__Block_copy", {block});
        Work work;
        work.function = process.calling_engine().read64(copy + 16);
        work.context = work.block = copy;
        return work;
    }
    static Work function_work(std::uint64_t context, std::uint64_t function, std::string_view operation) {
        if (!function) throw std::runtime_error(std::string(operation) + " received a null function");
        Work work;
        work.function = function; work.context = context;
        return work;
    }
    void discard(const Work& work) { if (work.block) static_cast<void>(call("__Block_release", {work.block})); }
    void execute(const Work& work) {
        if (work.action) work.action();
        else static_cast<void>(call_address(work.function, {work.context}));
        discard(work);
        if (work.group) leave(work.group);
    }

    // Caller holds metadata. The drainer activations `value` needs now; they
    // are counted as active, the caller starts them with activate().
    unsigned wanted(Queue& value) {
        if (value.kind == Kind::main || value.suspended || value.items.empty()) return 0;
        if (value.kind == Kind::serial) {
            if (value.active || value.owner) return 0;
            ++value.active;
            return 1;
        }
        if (value.barrier_running) return 0;
        std::size_t runnable = 0;
        for (const auto& item : value.items) {
            if (item.barrier) { if (runnable == 0 && value.running == 0) runnable = 1; break; }
            if (++runnable >= concurrency()) break;
        }
        const auto idle = value.active - value.executing;
        if (runnable <= idle || value.active >= concurrency()) return 0;
        const auto count = std::min(static_cast<unsigned>(runnable - idle), concurrency() - value.active);
        value.active += count;
        return count;
    }
    void activate(std::uint64_t address, unsigned count) {
        for (unsigned index = 0; index < count; ++index) {
            bool started{};
            try { started = start_task([self = shared_from_this(), address] { self->drain(address); self->run_destructors(); }); }
            catch (...) {
                std::lock_guard lock(metadata);
                if (const auto found = queues.find(address); found != queues.end()) --found->second->active;
                throw;
            }
            if (!started) {
                std::lock_guard lock(metadata);
                if (const auto found = queues.find(address); found != queues.end()) {
                    --found->second->active; retire(address, *found->second);
                }
            }
        }
    }

    void async(std::uint64_t address, Work work) {
        const auto value = queue(address);
        unsigned start{};
        {
            std::lock_guard lock(metadata);
            // Barriers apply to private concurrent queues only; on serial and
            // global queues dispatch_barrier_async is dispatch_async.
            if (value->kind != Kind::concurrent || value->immortal) work.barrier = false;
            value->items.push_back(std::move(work));
            start = wanted(*value);
        }
        value->changed.notify_all();
        // Main-queue work runs when the main thread drains; a GUI event pump
        // blocked in its wait returns to do so.
        if (value->kind == Kind::main) process.wake_main_thread();
        activate(address, start);
    }

    // Caller holds metadata. Bookkeeping after a concurrent item ran;
    // returns the activations to start.
    unsigned finish_concurrent(Queue& value, bool barrier, bool worker) {
        --value.running;
        if (worker) --value.executing;
        if (barrier) value.barrier_running = false;
        return wanted(value);
    }

    // Worker body: runs queued work until the queue has nothing runnable.
    void drain(std::uint64_t address) {
        std::shared_ptr<Queue> value;
        { std::lock_guard lock(metadata); const auto found = queues.find(address);
          if (found == queues.end()) throw std::runtime_error("dispatch worker lost its queue"); value = found->second; }
        const auto self = identity();
        const auto leave = [&] { --value->active; draining.erase(self); retire(address, *value); value->changed.notify_all(); };
        const bool concurrent = value->kind == Kind::concurrent;
        for (;;) {
            Work work;
            {
                std::lock_guard lock(metadata);
                if (value->items.empty() || value->suspended || exiting()) { leave(); return; }
                if (concurrent) {
                    if (value->barrier_running || (value->items.front().barrier && value->running)) { leave(); return; }
                    work = std::move(value->items.front()); value->items.pop_front();
                    if (work.barrier) value->barrier_running = true;
                    ++value->running;
                    if (work.gate) {
                        // The waiting dispatch_sync runs the work itself.
                        work.gate->granted = true;
                        value->changed.notify_all();
                        continue;
                    }
                    ++value->executing;
                } else {
                    work = std::move(value->items.front()); value->items.pop_front();
                    value->owner = self;
                }
                draining[self] = address;
            }
            try { execute(work); }
            catch (...) {
                std::lock_guard lock(metadata);
                if (concurrent) static_cast<void>(finish_concurrent(*value, work.barrier, true));
                value->owner = 0; leave();
                throw;
            }
            unsigned more{};
            {
                std::lock_guard lock(metadata);
                if (concurrent) more = finish_concurrent(*value, work.barrier, true);
                value->owner = 0;
                if (work.done) *work.done = true;
                value->changed.notify_all();
            }
            activate(address, more);
        }
    }

    void sync(std::uint64_t address, Work work, bool barrier = false) {
        const auto value = queue(address);
        const auto self = identity();
        if (value->kind == Kind::concurrent && value->immortal) { Entered entered(*this, address); execute(work); return; }
        if (value->kind == Kind::concurrent) { concurrent_sync(address, value, std::move(work), barrier); return; }
        if (value->kind == Kind::main) {
            if (&process.calling_engine() == &process.main_engine())
                throw std::runtime_error("dispatch_sync onto the main queue from the main thread deadlocks");
            // The main queue runs its work on the main thread when it drains.
            work.done = std::make_shared<bool>(false);
            const auto done = work.done;
            std::unique_lock lock(metadata);
            value->items.push_back(std::move(work));
            value->changed.notify_all();
            lock.unlock();
            process.wake_main_thread();
            lock.lock();
            while (!*done) {
                value->changed.wait_for(lock, poll_period);
                if (exiting()) { lock.unlock(); stop("dispatch_sync onto the main queue"); }
            }
            return;
        }
        {
            std::unique_lock lock(metadata);
            if (value->owner == self) throw std::runtime_error("dispatch_sync onto the current serial queue deadlocks");
            while (value->owner || value->active || !value->items.empty() || value->suspended) {
                value->changed.wait_for(lock, poll_period);
                if (exiting()) { lock.unlock(); stop("dispatch_sync"); }
            }
            value->owner = self;
        }
        const auto finish = [&] {
            std::lock_guard lock(metadata);
            value->owner = 0;
            const auto start = wanted(*value);
            value->changed.notify_all();
            return start;
        };
        try { Entered entered(*this, address); execute(work); }
        catch (...) { std::lock_guard lock(metadata); value->owner = 0; value->changed.notify_all(); throw; }
        activate(address, finish());
    }
    // dispatch_sync / dispatch_barrier_sync onto a private concurrent queue:
    // the work runs inline once earlier barriers (and, for a barrier, every
    // earlier item) finished.
    void concurrent_sync(std::uint64_t address, const std::shared_ptr<Queue>& value, Work work, bool barrier) {
        auto gate = std::make_shared<Gate>();
        unsigned start{};
        {
            std::unique_lock lock(metadata);
            const bool queued_barrier = std::any_of(value->items.begin(), value->items.end(), [](const Work& item) { return item.barrier; });
            const bool immediate = !value->suspended && !value->barrier_running &&
                (barrier ? value->items.empty() && value->running == 0 : !queued_barrier);
            if (immediate) {
                ++value->running;
                if (barrier) value->barrier_running = true;
                gate->granted = true;
            } else {
                Work marker;
                marker.barrier = barrier; marker.gate = gate;
                value->items.push_back(std::move(marker));
                start = wanted(*value);
            }
        }
        activate(address, start);
        {
            std::unique_lock lock(metadata);
            while (!gate->granted) {
                value->changed.wait_for(lock, poll_period);
                if (exiting()) { lock.unlock(); stop("dispatch_sync onto a concurrent queue"); }
            }
        }
        const auto finish = [&] {
            unsigned more;
            { std::lock_guard lock(metadata); more = finish_concurrent(*value, barrier, false); value->changed.notify_all(); }
            activate(address, more);
        };
        try { Entered entered(*this, address); execute(work); } catch (...) { finish(); throw; }
        finish();
    }

    // Runs the main queue's pending work on the calling (main) thread, at most
    // `limit` items; returns the number run.
    std::size_t drain_main(std::size_t limit = std::numeric_limits<std::size_t>::max()) {
        if (&process.calling_engine() != &process.main_engine())
            throw std::runtime_error("the dispatch main queue drains only on the main thread");
        const auto value = queue(main_queue);
        const auto self = identity();
        std::size_t count{};
        while (count < limit) {
            Work work;
            {
                std::lock_guard lock(metadata);
                if (value->items.empty() || value->suspended) break;
                work = std::move(value->items.front()); value->items.pop_front();
                draining[self] = main_queue;
            }
            try { execute(work); }
            catch (...) { std::lock_guard lock(metadata); draining.erase(self); throw; }
            ++count;
            std::lock_guard lock(metadata);
            if (work.done) *work.done = true;
            value->changed.notify_all();
        }
        std::lock_guard lock(metadata);
        draining.erase(self);
        return count;
    }
    std::size_t main_length() {
        std::lock_guard lock(metadata);
        const auto found = queues.find(main_queue);
        return found == queues.end() || found->second->suspended ? 0 : found->second->items.size();
    }

    void after(std::uint64_t when, std::uint64_t address, Work work) {
        if (when == forever) { discard(work); return; } // DISPATCH_TIME_FOREVER never fires.
        static_cast<void>(queue(address));
        reference(address, true); // Pending timers keep their queue.
        const auto self = shared_from_this();
        try {
            schedule_timer(when,
                [self, address, work] { self->async(address, work); self->reference(address, false); },
                [self, address, work] { self->discard(work); self->reference(address, false); });
        } catch (...) { reference(address, false); throw; }
    }

    // ---- suspension and context -------------------------------------------------
    void suspend(std::uint64_t address) {
        std::lock_guard lock(metadata);
        if (const auto found = queues.find(address); found != queues.end() && !found->second->released) {
            if (found->second->immortal) return; // Global and main queues ignore suspension.
            ++found->second->suspended;
            return;
        }
        if (const auto found = sources.find(address); found != sources.end()) { ++found->second->suspended; return; }
        throw std::runtime_error("dispatch_suspend of an object that cannot be suspended");
    }
    void resume(std::uint64_t address) {
        std::shared_ptr<Source> source;
        unsigned start{};
        {
            std::lock_guard lock(metadata);
            if (const auto found = queues.find(address); found != queues.end() && !found->second->released) {
                auto& value = *found->second;
                if (value.immortal) return;
                if (!value.suspended) throw std::runtime_error("dispatch_resume of a queue that is not suspended");
                if (--value.suspended == 0) start = wanted(value);
                value.changed.notify_all();
            } else if (const auto entry = sources.find(address); entry != sources.end()) {
                source = entry->second;
                if (source->inactive) source->inactive = false;
                else if (!source->suspended) throw std::runtime_error("dispatch_resume of a source that is not suspended");
                else --source->suspended;
            } else throw std::runtime_error("dispatch_resume of an object that cannot be resumed");
        }
        if (source) { source_ready(source); return; }
        activate(address, start);
    }
    void activate_object(std::uint64_t address) {
        std::shared_ptr<Source> source;
        {
            std::lock_guard lock(metadata);
            if (const auto entry = sources.find(address); entry != sources.end()) {
                source = entry->second;
                if (!source->inactive) return;
                source->inactive = false;
            } else if (queues.contains(address)) return; // Queues here are created active.
            else throw std::runtime_error("dispatch_activate of an object that cannot be activated");
        }
        source_ready(source);
    }
    std::uint64_t* context_slot(std::uint64_t address) {
        if (const auto found = queues.find(address); found != queues.end() && !found->second->released) return &found->second->context;
        if (const auto found = sources.find(address); found != sources.end()) return &found->second->context;
        throw std::runtime_error("dispatch context of an object without one");
    }
    void set_target(std::uint64_t address, std::uint64_t target) {
        std::lock_guard lock(metadata);
        const auto global_target = target == 0 || std::find(global.begin(), global.end(), target) != global.end();
        if (const auto found = sources.find(address); found != sources.end()) {
            const auto selected = queues.find(target ? target : global[1]);
            if (selected == queues.end() || selected->second->released) throw std::runtime_error("invalid dispatch target queue");
            auto& source = *found->second;
            if (source.queue != selected->first) {
                // Same reference bookkeeping as dispatch_retain/release; global
                // queues are immortal, so only private targets count.
                if (!selected->second->immortal) ++selected->second->references;
                const auto old = queues.find(source.queue);
                if (old != queues.end() && !old->second->immortal && --old->second->references == 0) {
                    old->second->released = true; retire(old->first, *old->second);
                }
                source.queue = selected->first;
            }
            return;
        }
        if (const auto found = queues.find(address); found != queues.end() && !found->second->released) {
            // A global target only sets a QoS (a host scheduling hint here);
            // queue hierarchies are not modelled.
            if (global_target && !found->second->immortal) return;
            throw std::runtime_error("unsupported dispatch_set_target_queue: queue hierarchies are not implemented");
        }
        throw std::runtime_error("dispatch_set_target_queue of an object without a target");
    }

    // ---- groups -----------------------------------------------------------------
    std::uint64_t new_group() {
        auto group = std::make_shared<Group>();
        group->address = marked_object(group_marker, ObjectClass::group);
        try { std::lock_guard lock(metadata); groups.emplace(group->address, group); }
        catch (...) { process.release_guest(group->address); throw; }
        return group->address;
    }
    std::shared_ptr<Group> group(std::uint64_t address) {
        std::lock_guard lock(metadata);
        const auto found = groups.find(address);
        if (found == groups.end() || found->second->released) throw std::runtime_error("invalid Alchemy dispatch group handle");
        return found->second;
    }
    // Caller holds metadata. A released group keeps its storage while work or
    // notifications still refer to it.
    void retire_group(Group& value) {
        if (!value.released || value.count || !value.notifications.empty()) return;
        const auto address = value.address;
        groups.erase(address);
        process.release_guest(address);
    }
    void enter(const std::shared_ptr<Group>& value) {
        std::lock_guard lock(metadata);
        if (value->count == UINT64_MAX) throw std::runtime_error("dispatch group count overflow");
        ++value->count;
    }
    void leave(const std::shared_ptr<Group>& value) {
        std::vector<std::pair<std::uint64_t, Work>> ready;
        {
            std::lock_guard lock(metadata);
            if (value->count == 0) throw std::runtime_error("unbalanced dispatch_group_leave");
            if (--value->count == 0) { ready.swap(value->notifications); retire_group(*value); }
        }
        group_changed.notify_all();
        for (auto& [target, work] : ready) { async(target, std::move(work)); reference(target, false); }
    }
    std::uint64_t group_wait(const std::shared_ptr<Group>& value, std::uint64_t timeout) {
        std::unique_lock lock(metadata);
        while (value->count) {
            if (exiting()) { lock.unlock(); stop("dispatch_group_wait"); }
            std::uint64_t interval = 10'000'000;
            if (timeout != forever) {
                const auto left = remaining(timeout);
                if (!left) return 1;
                interval = std::min(interval, left);
            }
            group_changed.wait_for(lock, std::chrono::nanoseconds(interval));
        }
        return 0;
    }
    void group_notify(const std::shared_ptr<Group>& value, std::uint64_t target, Work work) {
        static_cast<void>(queue(target));
        reference(target, true);
        {
            std::lock_guard lock(metadata);
            if (value->count) { value->notifications.emplace_back(target, std::move(work)); return; }
        }
        async(target, std::move(work));
        reference(target, false);
    }
    void group_async(const std::shared_ptr<Group>& value, std::uint64_t target, Work work) {
        static_cast<void>(queue(target));
        enter(value);
        work.group = value;
        try { async(target, std::move(work)); } catch (...) { leave(value); throw; }
    }

    // ---- dispatch_apply ---------------------------------------------------------
    void apply(std::uint64_t iterations, std::uint64_t address, std::uint64_t function, std::uint64_t context,
               std::uint64_t block) {
        if (!iterations) return;
        if (!function && !block) throw std::runtime_error("dispatch_apply received a null function");
        const auto invoke = [this, function, context, block](std::uint64_t index) {
            if (block) static_cast<void>(call_address(process.calling_engine().read64(block + 16), {block, index}));
            else static_cast<void>(call_address(function, {context, index}));
        };
        // DISPATCH_APPLY_AUTO (NULL) and concurrent queues run iterations in
        // parallel on workers and the caller; serial queues run them in order.
        const bool parallel = address == 0 || queue(address)->kind == Kind::concurrent;
        const auto helpers = parallel ? std::min<std::uint64_t>(concurrency() - 1, iterations - 1) : 0;
        if (!helpers) { for (std::uint64_t index = 0; index < iterations; ++index) invoke(index); return; }
        struct Apply {
            std::atomic<std::uint64_t> next{0};
            std::atomic<unsigned> running{0};
            std::uint64_t total{};
            std::mutex gate;
            std::condition_variable done;
            std::exception_ptr error;
        };
        auto state = std::make_shared<Apply>();
        state->total = iterations;
        const auto run = [state, invoke] {
            ++state->running;
            for (;;) {
                const auto index = state->next.fetch_add(1);
                if (index >= state->total) break;
                try { invoke(index); }
                catch (...) {
                    std::lock_guard lock(state->gate);
                    if (!state->error) state->error = std::current_exception();
                    state->next = state->total; // No further iterations start.
                }
            }
            std::lock_guard lock(state->gate);
            --state->running;
            state->done.notify_all();
        };
        for (std::uint64_t index = 0; index < helpers; ++index) static_cast<void>(start_task(run));
        run();
        std::unique_lock lock(state->gate);
        // Helpers that start after this returns see no iteration left and never
        // touch the guest function.
        while (state->running) {
            state->done.wait_for(lock, poll_period);
            if (exiting()) { lock.unlock(); stop("dispatch_apply"); }
        }
        if (state->error) std::rethrow_exception(state->error);
    }

    // ---- sources ----------------------------------------------------------------
    std::uint64_t new_source(std::uint64_t type, std::uint64_t handle, std::uint64_t mask, std::uint64_t target) {
        SourceType kind;
        std::string name;
        {
            std::lock_guard lock(metadata);
            const auto found = source_types.find(type);
            if (found == source_types.end()) return 0; // Not a DISPATCH_SOURCE_TYPE_*: NULL as on Darwin.
            kind = found->second.first; name = found->second.second;
        }
        const bool mach = kind == SourceType::mach_recv || kind == SourceType::mach_send;
        if (kind == SourceType::unsupported || (mach && !mach_port_events(process)))
            throw std::runtime_error("unsupported dispatch source type DISPATCH_SOURCE_TYPE_" + name);
        bool canceled_at_creation = false, known_child = false;
        if (mach) {
            // A receive right (MACH_RECV, no mask) or a send right watched for
            // DISPATCH_MACH_SEND_DEAD (1); other masks are invalid (NULL).
            if (!handle || handle > UINT32_MAX) return 0;
            if (kind == SourceType::mach_recv ? mask != 0 : (mask & ~1ULL) != 0) return 0;
        } else if (kind == SourceType::memory_pressure) {
            // NORMAL 0x1, WARN 0x2, CRITICAL 0x4; the private process-limit
            // (0x10, 0x20) and MSL bits are accepted and never fire.
            if (handle || (mask & ~(0x7ULL | 0x30ULL | 0xf0000000ULL))) return 0;
        } else if (kind == SourceType::read || kind == SourceType::write || kind == SourceType::vnode) {
            // A descriptor; READ and WRITE take no mask, VNODE the DISPATCH_VNODE_*
            // bits (NOTE_DELETE 0x1 ... NOTE_FUNLOCK 0x100).
            if (handle > INT32_MAX || (kind == SourceType::vnode ? (mask & ~0x1ffULL) != 0 : mask != 0)) return 0;
        } else if (kind == SourceType::proc) {
            // EXIT 0x80000000, FORK 0x40000000, EXEC 0x20000000, SIGNAL 0x08000000.
            if (!handle || handle > INT32_MAX || (mask & ~0xe8000000ULL)) return 0;
            // Only children of this process (and this process) can be watched;
            // another pid's source is cancelled at once (as libdispatch does when
            // the kernel refuses the registration).
            const auto state = process.child_exited ? process.child_exited(static_cast<std::uint32_t>(handle)) : std::nullopt;
            known_child = state.has_value();
            canceled_at_creation = !known_child && handle != static_cast<std::uint64_t>(call("_getpid", {}));
        } else if (handle) return 0; // Timer and data sources take no handle.
        if (!target) target = global[1];
        static_cast<void>(queue(target));
        auto source = std::make_shared<Source>();
        source->type = kind; source->type_name = name; source->handle = handle; source->mask = mask; source->queue = target;
        source->address = marked_object(source_marker, ObjectClass::source);
        source->known_child = known_child;
        source->canceled = canceled_at_creation;
        reference(target, true);
        try { std::lock_guard lock(metadata); sources.emplace(source->address, source); }
        catch (...) { reference(target, false); process.release_guest(source->address); throw; }
        if (mach) watch_mach();
        if (kind == SourceType::memory_pressure || kind == SourceType::proc) start_polling();
        if (kind == SourceType::read || kind == SourceType::write || kind == SourceType::vnode) {
            // The kernel refuses the descriptor: the source is cancelled (libdispatch).
            if (!kevent_change(*source, 1 | (kind == SourceType::vnode ? 0x20 : 0x80))) {
                std::lock_guard lock(metadata);
                source->canceled = true;
            } else watch_kqueue();
        }
        return source->address;
    }
    std::shared_ptr<Source> source(std::uint64_t address) {
        std::lock_guard lock(metadata);
        const auto found = sources.find(address);
        if (found == sources.end()) throw std::runtime_error("invalid Alchemy dispatch source handle");
        return found->second;
    }
    Handler make_handler(std::uint64_t function, std::uint64_t block) {
        if (block) return {0, call("__Block_copy", {block})};
        return {function, 0};
    }
    // Replaced and cancelled handler blocks are released once no handler of
    // the source is running (a handler may replace or outlive itself).
    void flush_retired(Source& value) {
        std::vector<Handler> retired;
        {
            std::lock_guard lock(metadata);
            if (value.invoking) return;
            retired.swap(value.retired);
        }
        for (const auto& handler : retired) if (handler.block) static_cast<void>(call("__Block_release", {handler.block}));
    }
    void release_handlers(Source& value) {
        {
            std::lock_guard lock(metadata);
            for (auto* slot : {&value.event, &value.cancel, &value.registration})
                if (*slot) value.retired.push_back(std::exchange(*slot, {}));
        }
        flush_retired(value);
    }
    void set_handler(std::uint64_t address, Handler Source::*slot, std::uint64_t function, std::uint64_t block) {
        const auto value = source(address);
        auto handler = make_handler(function, block);
        {
            std::lock_guard lock(metadata);
            std::swap(value.get()->*slot, handler);
            if (handler) value->retired.push_back(handler);
        }
        flush_retired(*value);
        if (slot == &Source::event) source_ready(value);
    }
    void invoke_handler(Source& value, const Handler& handler, std::uint64_t context) {
        { std::lock_guard lock(metadata); ++value.invoking; }
        struct Done {
            Dispatch& self; Source& value;
            ~Done() { { std::lock_guard lock(self.metadata); --value.invoking; } try { self.flush_retired(value); } catch (...) {} }
        } done{*this, value};
        if (handler.block) static_cast<void>(call_address(process.calling_engine().read64(handler.block + 16), {handler.block}));
        else if (handler.function) static_cast<void>(call_address(handler.function, {context}));
    }
    // Submits the registration, event or cancel handler that is due.
    void source_ready(const std::shared_ptr<Source>& value) {
        std::vector<Work> submissions;
        std::uint64_t target{};
        {
            std::lock_guard lock(metadata);
            if (value->inactive || value->suspended || value->disposed) return;
            target = value->queue;
            const auto self = shared_from_this();
            if (!value->registered) {
                value->registered = true;
                if (value->registration) {
                    Work work;
                    work.action = [self, value] {
                        Handler handler; std::uint64_t context;
                        { std::lock_guard lock(self->metadata); handler = value->registration; context = value->context; }
                        self->invoke_handler(*value, handler, context);
                    };
                    submissions.push_back(std::move(work));
                }
            }
            if (value->canceled) {
                if (!value->cancel_delivered) {
                    value->cancel_delivered = true;
                    Work work;
                    work.action = [self, value] { self->source_cancelled(value); };
                    submissions.push_back(std::move(work));
                }
            } else if (!value->event_queued && value->pending && value->event) {
                value->event_queued = true;
                Work work;
                work.action = [self, value] { self->source_event(value); };
                submissions.push_back(std::move(work));
            }
        }
        for (auto& work : submissions) async(target, std::move(work));
    }
    void source_event(const std::shared_ptr<Source>& value) {
        Handler handler;
        std::uint64_t context{};
        {
            std::lock_guard lock(metadata);
            value->event_queued = false;
            if (value->canceled) return;
            value->data = std::exchange(value->pending, 0);
            if (value->type == SourceType::mach_recv) value->data = 0;   // MACH_RECV has no data
            handler = value->event; context = value->context;
        }
        invoke_handler(*value, handler, context);
        // A receive right with messages left (the handler took none or some)
        // stays pending, as libdispatch's level-triggered MACH_RECV.
        if (value->type == SourceType::mach_recv) check_mach(value, mach_port_events(process));
        // READ/WRITE are registered with EV_DISPATCH: re-enabled after the handler.
        if (value->type == SourceType::read || value->type == SourceType::write) {
            bool live;
            { std::lock_guard lock(metadata); live = !value->canceled; }
            if (live) static_cast<void>(kevent_change(*value, 0x4));   // EV_ENABLE
        }
        source_ready(value); // Events merged while the handler ran.
    }
    // ---- Mach sources -------------------------------------------------------------
    // MACH_RECV is pending while a message is queued on its receive right (or
    // a member of its port set); MACH_SEND fires DISPATCH_MACH_SEND_DEAD once
    // its name becomes a dead name. A watcher task on the worker pool checks
    // them whenever the Mach IPC space changes (mach_port_events.hpp) and
    // ends when no Mach source is left.
    void check_mach(const std::shared_ptr<Source>& value, const std::shared_ptr<MachPortEvents>& events) {
        if (!events) return;
        const auto name = static_cast<std::uint32_t>(value->handle);
        const bool ready = value->type == SourceType::mach_recv
            ? events->poll(process.calling_engine(), name, 0, 0, 0).has_value()
            : (value->mask & 1) && events->dead_name(name);
        if (!ready) return;
        {
            std::lock_guard lock(metadata);
            if (value->canceled || value->disposed || value->event_queued || value->invoking || value->pending) return;
            if (value->type == SourceType::mach_send) {
                if (value->dead_reported) return;
                value->dead_reported = true;
            }
            value->pending = 1;   // DISPATCH_MACH_SEND_DEAD for MACH_SEND
        }
        source_ready(value);
    }
    // ---- memory pressure and process sources ---------------------------------------
    // DISPATCH_SOURCE_TYPE_MEMORYPRESSURE: the host's memory state as Windows
    // memory resource notifications report it: the low-memory notification is
    // CRITICAL, the high-memory one NORMAL, neither WARN. A source fires when
    // the level changes to one in its mask; its data is the new level.
    // ALCHEMY_MEMORY_PRESSURE=normal|warn|critical overrides the host state
    // (diagnostics and tests).
    // DISPATCH_SOURCE_TYPE_PROC: DISPATCH_PROC_EXIT of a child process (it
    // exited, or was reaped). Fork, exec and signal events of another process
    // are not observable here and never fire.
    static std::uint64_t memory_pressure_level() {
        const auto forced = host::process::environment("ALCHEMY_MEMORY_PRESSURE").value_or(std::string());
        {
            const std::string_view level(forced);
            if (level == "critical") return 4;
            if (level == "warn") return 2;
            if (level == "normal") return 1;
        }
        // DISPATCH_MEMORYPRESSURE_NORMAL 1, _WARN 2, _CRITICAL 4.
        const auto pressure = host::system::memory_pressure();
        if (!pressure) return 1;
        return *pressure == host::system::Pressure::critical ? 4 : *pressure == host::system::Pressure::warning ? 2 : 1;
    }
    void start_polling() {
        {
            std::lock_guard lock(metadata);
            if (!pressure_level) pressure_level = memory_pressure_level();   // the baseline: no event for it
            if (polling) return;
            polling = true;
        }
        arm_poll();
    }
    void arm_poll() {
        const auto self = shared_from_this();
        schedule_timer(deadline(now(false), 250'000'000, false), [self] { self->poll_sources(); },
                       [self] { std::lock_guard lock(self->metadata); self->polling = false; });
    }
    void poll_sources() {
        const auto level = memory_pressure_level();
        std::vector<std::shared_ptr<Source>> ready;
        bool any = false;
        std::vector<std::shared_ptr<Source>> processes;
        {
            std::lock_guard lock(metadata);
            const bool changed = level != pressure_level;
            pressure_level = level;
            for (const auto& [address, value] : sources) {
                static_cast<void>(address);
                if (value->canceled || value->disposed) continue;
                if (value->type == SourceType::memory_pressure) {
                    any = true;
                    if (changed && (value->mask & level)) { value->pending = level; ready.push_back(value); }
                } else if (value->type == SourceType::proc) {
                    any = true;
                    if ((value->mask & 0x80000000ULL) && !value->dead_reported) processes.push_back(value);
                }
            }
            if (!any || exiting()) { polling = false; return; }
        }
        for (const auto& value : processes) {
            const auto state = process.child_exited ? process.child_exited(static_cast<std::uint32_t>(value->handle)) : std::nullopt;
            if (!value->known_child || (state && !*state)) continue;
            {
                std::lock_guard lock(metadata);
                if (value->dead_reported) continue;
                value->dead_reported = true;
                value->pending = 0x80000000ULL;   // DISPATCH_PROC_EXIT
            }
            ready.push_back(value);
        }
        for (const auto& value : ready) source_ready(value);
        arm_poll();
    }
    void watch_mach() {
        {
            std::lock_guard lock(metadata);
            if (mach_watching) return;
            mach_watching = true;
        }
        const auto self = shared_from_this();
        bool started{};
        try { started = start_task([self] { self->mach_watcher(); }); }
        catch (...) { std::lock_guard lock(metadata); mach_watching = false; throw; }
        if (!started) { std::lock_guard lock(metadata); mach_watching = false; }
    }
    void mach_watcher() {
        const auto events = mach_port_events(process);
        for (;;) {
            const auto epoch = events ? events->epoch() : 0;
            std::vector<std::shared_ptr<Source>> watched;
            {
                std::lock_guard lock(metadata);
                for (const auto& [address, value] : sources) {
                    static_cast<void>(address);
                    if ((value->type == SourceType::mach_recv || value->type == SourceType::mach_send) && !value->canceled && !value->disposed)
                        watched.push_back(value);
                }
                if (watched.empty() || exiting() || !events) { mach_watching = false; return; }
            }
            for (const auto& value : watched) check_mach(value, events);
            events->wait(epoch, std::chrono::milliseconds(50));
        }
    }
    void source_cancelled(const std::shared_ptr<Source>& value) {
        Handler handler;
        std::uint64_t context{};
        { std::lock_guard lock(metadata); handler = value->cancel; context = value->context; }
        invoke_handler(*value, handler, context);
        release_handlers(*value);
    }
    void cancel(std::uint64_t address) {
        const auto value = source(address);
        {
            std::lock_guard lock(metadata);
            if (value->canceled) return;
            value->canceled = true;
            ++value->generation;
        }
        if (value->type == SourceType::read || value->type == SourceType::write || value->type == SourceType::vnode)
            static_cast<void>(kevent_change(*value, 0x2));   // EV_DELETE
        source_ready(value);
    }
    // ---- descriptor sources: READ, WRITE, VNODE over kqueue ------------------------------
    // Each source is a knote of one private kqueue (udata = the source):
    // READ/WRITE with EV_DISPATCH (level-triggered, re-enabled after each
    // handler; data is the bytes readable or the write space hint), VNODE
    // with EV_CLEAR (the DISPATCH_VNODE_* bits accumulate as data). A worker
    // task waits in kevent64 and merges the events.
    bool kevent_change(const Source& value, std::uint16_t flags) {
        {
            std::lock_guard lock(metadata);
            if (kqueue < 0) {
                const auto created = static_cast<std::int64_t>(static_cast<std::int32_t>(call("_kqueue", {})));
                if (created < 0) return false;
                kqueue = created;
            }
        }
        transmute::GuestMemory& memory = process.calling_engine();
        const auto change = process.allocate_guest(96);
        struct Cell { ProcessContext& process; std::uint64_t address; ~Cell() { process.release_guest(address); } } owned{process, change};
        memory.fill_bytes(change, 0, 96);
        memory.write64(change, value.handle);
        const auto filter = static_cast<std::uint16_t>(value.type == SourceType::read ? -1 : value.type == SourceType::write ? -2 : -4);
        memory.write8(change + 8, static_cast<std::uint8_t>(filter));
        memory.write8(change + 9, static_cast<std::uint8_t>(filter >> 8));
        const auto receipt = static_cast<std::uint16_t>(flags | 0x40);   // EV_RECEIPT
        memory.write8(change + 10, static_cast<std::uint8_t>(receipt));
        memory.write8(change + 11, static_cast<std::uint8_t>(receipt >> 8));
        memory.write32(change + 12, value.type == SourceType::vnode ? static_cast<std::uint32_t>(value.mask) : 0);
        memory.write64(change + 24, value.address);
        const auto result = static_cast<std::int32_t>(call("_kevent64", {static_cast<std::uint64_t>(kqueue), change, 1, change + 48, 1, 0, 0}));
        return result == 1 && memory.read64(change + 48 + 16) == 0;
    }
    void watch_kqueue() {
        {
            std::lock_guard lock(metadata);
            if (kqueue_watching) return;
            kqueue_watching = true;
        }
        const auto self = shared_from_this();
        bool started{};
        try { started = start_task([self] { self->kqueue_watcher(); }); }
        catch (...) { std::lock_guard lock(metadata); kqueue_watching = false; throw; }
        if (!started) { std::lock_guard lock(metadata); kqueue_watching = false; }
    }
    void kqueue_watcher() {
        constexpr std::uint64_t capacity = 16;
        const auto events = process.allocate_guest(capacity * 48 + 16);
        struct Cell { ProcessContext& process; std::uint64_t address; ~Cell() { process.release_guest(address); } } owned{process, events};
        transmute::GuestMemory& memory = process.calling_engine();
        const auto timeout = events + capacity * 48;
        memory.write64(timeout, 0);
        memory.write64(timeout + 8, 100'000'000);   // 100 ms: notice the last source going away
        for (;;) {
            std::int64_t queue;
            {
                std::lock_guard lock(metadata);
                bool any = false;
                for (const auto& [address, value] : sources) {
                    static_cast<void>(address);
                    if ((value->type == SourceType::read || value->type == SourceType::write || value->type == SourceType::vnode) &&
                        !value->canceled && !value->disposed) { any = true; break; }
                }
                if (!any || exiting()) { kqueue_watching = false; return; }
                queue = kqueue;
            }
            const auto count = static_cast<std::int32_t>(call("_kevent64", {static_cast<std::uint64_t>(queue), 0, 0, events, capacity, 0, timeout}));
            for (std::int32_t index = 0; index < count; ++index) {
                const auto at = events + static_cast<std::uint64_t>(index) * 48;
                const auto address = memory.read64(at + 24);
                const auto data = memory.read64(at + 16);
                const auto fflags = memory.read32(at + 12);
                std::shared_ptr<Source> value;
                {
                    std::lock_guard lock(metadata);
                    const auto found = sources.find(address);
                    if (found == sources.end() || found->second->canceled) continue;
                    value = found->second;
                    if (value->type == SourceType::vnode) value->pending |= fflags;
                    else value->pending = data ? data : 1;   // a ready descriptor always submits
                }
                source_ready(value);
            }
        }
    }
    void merge(std::uint64_t address, std::uint64_t data) {
        const auto value = source(address);
        {
            std::lock_guard lock(metadata);
            switch (value->type) {
            case SourceType::data_add:
                if (!data) return; // Merging zero submits nothing.
                value->pending += data; break;
            case SourceType::data_or:
                if (!data) return;
                value->pending |= data; break;
            case SourceType::data_replace:
                value->pending = data;
                if (!data) return;
                break;
            default: throw std::runtime_error("dispatch_source_merge_data on a source that is not a data source");
            }
        }
        source_ready(value);
    }
    void set_timer(std::uint64_t address, std::uint64_t start, std::uint64_t interval) {
        const auto value = source(address);
        // DISPATCH_TIME_NOW and DISPATCH_WALLTIME_NOW start from the current time.
        if (start == 0) start = deadline(now(false), 0, false);
        else if (start == wall_now) start = deadline(now(true), 0, true);
        std::uint64_t generation;
        {
            std::lock_guard lock(metadata);
            if (value->type != SourceType::timer) throw std::runtime_error("dispatch_source_set_timer on a source that is not a timer");
            value->interval = interval;
            generation = ++value->generation;
            if (value->canceled || start == forever) return;
        }
        arm(value, start, generation);
    }
    void arm(const std::shared_ptr<Source>& value, std::uint64_t when, std::uint64_t generation) {
        const std::weak_ptr<Source> weak = value;
        const auto self = shared_from_this();
        schedule_timer(when, [self, weak, when, generation] { if (const auto value = weak.lock()) self->timer_fired(value, when, generation); }, {});
    }
    void timer_fired(const std::shared_ptr<Source>& value, std::uint64_t when, std::uint64_t generation) {
        std::uint64_t next = forever;
        {
            std::lock_guard lock(metadata);
            if (generation != value->generation || value->canceled) return;
            ++value->pending;
            // DISPATCH_TIME_FOREVER (and zero) intervals fire once. Missed
            // periods coalesce into the data count, as on Darwin.
            if (value->interval && value->interval != forever) {
                const bool wall = (when & wall_bit) != 0;
                next = deadline(when & ~wall_bit, static_cast<std::int64_t>(std::min(value->interval, maximum_time)), wall);
                const auto current = now(wall);
                if (next != forever && (next & ~wall_bit) <= current) {
                    const auto missed = (current - (next & ~wall_bit)) / value->interval + 1;
                    value->pending += missed;
                    next = deadline(next & ~wall_bit, static_cast<std::int64_t>(std::min(missed * value->interval, maximum_time)), wall);
                }
            }
        }
        if (next != forever) arm(value, next, generation);
        source_ready(value);
    }

    std::uint64_t global_queue(std::int64_t identifier, std::uint64_t flags) const {
        if (flags) return 0;
        switch (identifier) {
        case 2: case 0x21: case 0x19: return global[0];
        case 0: case 0x15: return global[1];
        case -2: case 0x11: case INT16_MIN: case 0x09: return global[2];
        default: return 0;
        }
    }
    Kind attribute_kind(std::uint64_t attribute) const {
        if (!attribute || attribute == serial_attribute) return Kind::serial;
        if (attribute == concurrent_attribute) return Kind::concurrent;
        throw std::runtime_error("unsupported Darwin dispatch queue attribute");
    }

    template<class Function> void install(std::string name, Function function) {
        const auto self = shared_from_this();
        static_cast<void>(process.install_function(std::move(name), [self, function](auto& state, auto& memory) {
            state.x[0] = function(*self, state, memory);
        }));
    }
    void install_queues();
    void install_groups_and_sources();
};

void Dispatch::install_queues() {
    main_queue = new_queue(Kind::main, "com.apple.main-thread", true);
    const std::array<const char*, 3> names{"com.apple.root.user-initiated-qos", "com.apple.root.default-qos", "com.apple.root.utility-qos"};
    for (std::size_t index = 0; index < global.size(); ++index) global[index] = new_queue(Kind::concurrent, names[index], true);
    concurrent_attribute = process.allocate_guest(64);
    serial_attribute = process.allocate_guest(64);
    for (const auto attribute : {concurrent_attribute, serial_attribute}) {
        process.calling_engine().fill_bytes(attribute, 0, 64);
        process.calling_engine().write64(attribute, isa(ObjectClass::attribute, 0));
    }
    process.install_export(std::string(system_library), "__dispatch_main_q", {main_queue, ExportKind::data});
    process.install_export(std::string(system_library), "__dispatch_queue_attr_concurrent", {concurrent_attribute, ExportKind::data});
    const auto owned = shared_from_this();
    worker_entry = process.install_function("__alchemy_dispatch_worker", [owned](auto& state, auto&) {
        owned->worker(); state.x[0] = 0;
    }, "alchemy.internal");
    timer_entry = process.install_function("__alchemy_dispatch_timer", [owned](auto& state, auto&) {
        owned->timer_service(); state.x[0] = 0;
    }, "alchemy.internal");
    install("_dispatch_get_global_queue", [](auto& self, auto& s, auto&) {
        return self.global_queue(std::bit_cast<std::int64_t>(s.x[0]), s.x[1]);
    });
    install("_dispatch_queue_create", [](auto& self, auto& s, auto& m) {
        std::string label;
        if (s.x[0]) { const auto bytes = m.read_string(s.x[0], 1, 4096); label.assign(bytes.begin(), bytes.end()); }
        return self.new_queue(self.attribute_kind(s.x[1]), label, false);
    });
    install("_dispatch_queue_attr_make_with_qos_class", [](auto& self, auto& s, auto&) {
        // QoS is a scheduling hint for host threads; the attribute keeps its kind.
        return self.attribute_kind(s.x[0]) == Kind::concurrent ? self.concurrent_attribute : self.serial_attribute;
    });
    install("_dispatch_queue_get_label", [](auto& self, auto& s, auto&) {
        auto address = s.x[0];
        if (!address) {
            std::lock_guard lock(self.metadata);
            const auto found = self.draining.find(self.identity());
            address = found != self.draining.end() ? found->second :
                &self.process.calling_engine() == &self.process.main_engine() ? self.main_queue : 0;
            if (!address) return std::uint64_t{0};
        }
        return self.queue(address)->label;
    });
    install("_dispatch_async", [](auto& self, auto& s, auto&) { self.async(s.x[0], self.block_work(s.x[1])); return std::uint64_t{0}; });
    install("_dispatch_async_f", [](auto& self, auto& s, auto&) {
        self.async(s.x[0], function_work(s.x[1], s.x[2], "dispatch_async_f")); return std::uint64_t{0};
    });
    install("_dispatch_sync", [](auto& self, auto& s, auto&) { self.sync(s.x[0], self.block_work(s.x[1])); return std::uint64_t{0}; });
    install("_dispatch_sync_f", [](auto& self, auto& s, auto&) {
        self.sync(s.x[0], function_work(s.x[1], s.x[2], "dispatch_sync_f")); return std::uint64_t{0};
    });
    install("_dispatch_barrier_async", [](auto& self, auto& s, auto&) {
        auto work = self.block_work(s.x[1]); work.barrier = true; self.async(s.x[0], std::move(work)); return std::uint64_t{0};
    });
    install("_dispatch_barrier_async_f", [](auto& self, auto& s, auto&) {
        auto work = function_work(s.x[1], s.x[2], "dispatch_barrier_async_f"); work.barrier = true;
        self.async(s.x[0], std::move(work)); return std::uint64_t{0};
    });
    install("_dispatch_barrier_sync", [](auto& self, auto& s, auto&) { self.sync(s.x[0], self.block_work(s.x[1]), true); return std::uint64_t{0}; });
    install("_dispatch_barrier_sync_f", [](auto& self, auto& s, auto&) {
        self.sync(s.x[0], function_work(s.x[1], s.x[2], "dispatch_barrier_sync_f"), true); return std::uint64_t{0};
    });
    install("_dispatch_after", [](auto& self, auto& s, auto&) { self.after(s.x[0], s.x[1], self.block_work(s.x[2])); return std::uint64_t{0}; });
    install("_dispatch_after_f", [](auto& self, auto& s, auto&) {
        self.after(s.x[0], s.x[1], function_work(s.x[2], s.x[3], "dispatch_after_f")); return std::uint64_t{0};
    });
    install("_dispatch_apply", [](auto& self, auto& s, auto&) { self.apply(s.x[0], s.x[1], 0, 0, s.x[2]); return std::uint64_t{0}; });
    install("_dispatch_apply_f", [](auto& self, auto& s, auto&) { self.apply(s.x[0], s.x[1], s.x[3], s.x[2], 0); return std::uint64_t{0}; });
    install("_dispatch_suspend", [](auto& self, auto& s, auto&) { self.suspend(s.x[0]); return std::uint64_t{0}; });
    install("_dispatch_resume", [](auto& self, auto& s, auto&) { self.resume(s.x[0]); return std::uint64_t{0}; });
    install("_dispatch_activate", [](auto& self, auto& s, auto&) { self.activate_object(s.x[0]); return std::uint64_t{0}; });
    install("_dispatch_set_context", [](auto& self, auto& s, auto&) {
        std::lock_guard lock(self.metadata); *self.context_slot(s.x[0]) = s.x[1]; return std::uint64_t{0};
    });
    install("_dispatch_get_context", [](auto& self, auto& s, auto&) { std::lock_guard lock(self.metadata); return *self.context_slot(s.x[0]); });
    install("_dispatch_set_finalizer_f", [](auto&, auto& s, auto&) {
        if (s.x[1]) throw std::runtime_error("unsupported dispatch_set_finalizer_f: dispatch object finalizers are not implemented");
        return std::uint64_t{0};
    });
    install("_dispatch_queue_set_specific", [](auto& self, auto& s, auto&) {
        self.set_specific(s.x[0], s.x[1], s.x[2], s.x[3]); return std::uint64_t{0};
    });
    install("_dispatch_queue_get_specific", [](auto& self, auto& s, auto&) { return self.get_specific(s.x[0], s.x[1]); });
    install("_dispatch_get_specific", [](auto& self, auto& s, auto&) { return self.get_specific(self.current_queue(), s.x[0]); });
    install("_dispatch_set_target_queue", [](auto& self, auto& s, auto&) { self.set_target(s.x[0], s.x[1]); return std::uint64_t{0}; });
    // CoreFoundation's run loop drains the main queue through this SPI.
    install("__dispatch_main_queue_callback_4CF", [](auto& self, auto&, auto&) { static_cast<void>(self.drain_main()); return std::uint64_t{0}; });
    static_cast<void>(process.install_function("_dispatch_main", [owned](auto& state, auto&) {
        const auto value = owned->queue(owned->main_queue);
        for (;;) {
            static_cast<void>(owned->drain_main());
            if (const auto status = owned->process.requested_exit()) {
                // dispatch_main never returns: leave through _exit with the requested status.
                state.x[0] = *status; state.x[30] = owned->symbol("__exit"); return;
            }
            std::unique_lock lock(owned->metadata);
            if (value->items.empty()) value->changed.wait_for(lock, poll_period);
        }
    }));
    install_groups_and_sources();
}

void Dispatch::install_groups_and_sources() {
    install("_dispatch_group_create", [](auto& self, auto&, auto&) { return self.new_group(); });
    install("_dispatch_group_enter", [](auto& self, auto& s, auto&) { self.enter(self.group(s.x[0])); return std::uint64_t{0}; });
    install("_dispatch_group_leave", [](auto& self, auto& s, auto&) { self.leave(self.group(s.x[0])); return std::uint64_t{0}; });
    install("_dispatch_group_wait", [](auto& self, auto& s, auto&) { return self.group_wait(self.group(s.x[0]), s.x[1]); });
    install("_dispatch_group_notify", [](auto& self, auto& s, auto&) {
        const auto value = self.group(s.x[0]);
        self.group_notify(value, s.x[1], self.block_work(s.x[2])); return std::uint64_t{0};
    });
    install("_dispatch_group_notify_f", [](auto& self, auto& s, auto&) {
        const auto value = self.group(s.x[0]);
        self.group_notify(value, s.x[1], function_work(s.x[2], s.x[3], "dispatch_group_notify_f")); return std::uint64_t{0};
    });
    install("_dispatch_group_async", [](auto& self, auto& s, auto&) {
        const auto value = self.group(s.x[0]);
        self.group_async(value, s.x[1], self.block_work(s.x[2])); return std::uint64_t{0};
    });
    install("_dispatch_group_async_f", [](auto& self, auto& s, auto&) {
        const auto value = self.group(s.x[0]);
        self.group_async(value, s.x[1], function_work(s.x[2], s.x[3], "dispatch_group_async_f")); return std::uint64_t{0};
    });
    // DISPATCH_SOURCE_TYPE_* are addresses of libdispatch data symbols.
    const std::array<std::pair<const char*, SourceType>, 12> types{{
        {"timer", SourceType::timer}, {"data_add", SourceType::data_add}, {"data_or", SourceType::data_or},
        {"data_replace", SourceType::data_replace}, {"read", SourceType::read}, {"write", SourceType::write},
        {"proc", SourceType::proc}, {"signal", SourceType::unsupported}, {"vnode", SourceType::vnode},
        {"mach_send", SourceType::mach_send}, {"mach_recv", SourceType::mach_recv},
        {"memorypressure", SourceType::memory_pressure}}};
    for (const auto& [name, kind] : types) {
        // DISPATCH_SOURCE_TYPE_* are type descriptors, not objects.
        const auto address = process.allocate_guest(16);
        process.calling_engine().fill_bytes(address, 0, 16);
        process.calling_engine().write64(address, source_marker);
        std::string upper(name);
        std::transform(upper.begin(), upper.end(), upper.begin(), [](char c) { return static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c); });
        source_types.emplace(address, std::pair{kind, upper});
        process.install_export(std::string(system_library), "__dispatch_source_type_" + std::string(name), {address, ExportKind::data});
    }
    install("_dispatch_source_create", [](auto& self, auto& s, auto&) { return self.new_source(s.x[0], s.x[1], s.x[2], s.x[3]); });
    install("_dispatch_source_set_event_handler", [](auto& self, auto& s, auto&) { self.set_handler(s.x[0], &Source::event, 0, s.x[1]); return std::uint64_t{0}; });
    install("_dispatch_source_set_event_handler_f", [](auto& self, auto& s, auto&) { self.set_handler(s.x[0], &Source::event, s.x[1], 0); return std::uint64_t{0}; });
    install("_dispatch_source_set_cancel_handler", [](auto& self, auto& s, auto&) { self.set_handler(s.x[0], &Source::cancel, 0, s.x[1]); return std::uint64_t{0}; });
    install("_dispatch_source_set_cancel_handler_f", [](auto& self, auto& s, auto&) { self.set_handler(s.x[0], &Source::cancel, s.x[1], 0); return std::uint64_t{0}; });
    install("_dispatch_source_set_registration_handler", [](auto& self, auto& s, auto&) { self.set_handler(s.x[0], &Source::registration, 0, s.x[1]); return std::uint64_t{0}; });
    install("_dispatch_source_set_registration_handler_f", [](auto& self, auto& s, auto&) { self.set_handler(s.x[0], &Source::registration, s.x[1], 0); return std::uint64_t{0}; });
    install("_dispatch_source_cancel", [](auto& self, auto& s, auto&) { self.cancel(s.x[0]); return std::uint64_t{0}; });
    install("_dispatch_source_testcancel", [](auto& self, auto& s, auto&) {
        const auto value = self.source(s.x[0]); std::lock_guard lock(self.metadata); return std::uint64_t{value->canceled};
    });
    install("_dispatch_source_get_data", [](auto& self, auto& s, auto&) {
        const auto value = self.source(s.x[0]); std::lock_guard lock(self.metadata); return value->data;
    });
    install("_dispatch_source_get_handle", [](auto& self, auto& s, auto&) { return self.source(s.x[0])->handle; });
    install("_dispatch_source_get_mask", [](auto& self, auto& s, auto&) { return self.source(s.x[0])->mask; });
    install("_dispatch_source_merge_data", [](auto& self, auto& s, auto&) { self.merge(s.x[0], s.x[1]); return std::uint64_t{0}; });
    install("_dispatch_source_set_timer", [](auto& self, auto& s, auto&) { self.set_timer(s.x[0], s.x[1], s.x[2]); return std::uint64_t{0}; });
}
} // namespace

void install_darwin_dispatch(ProcessContext& context) {
    auto implementation = std::make_shared<Dispatch>(context);
    implementation->install("_dispatch_semaphore_create", [](auto& self, auto& state, auto&) {
        return self.create(std::bit_cast<std::int64_t>(state.x[0]));
    });
    implementation->install("_dispatch_semaphore_wait", [](auto& self, auto& state, auto&) {
        return self.wait(state.x[0], state.x[1]);
    });
    implementation->install("_dispatch_semaphore_signal", [](auto& self, auto& state, auto&) {
        return self.signal(state.x[0]);
    });
    implementation->install("_dispatch_retain", [](auto& self, auto& state, auto&) {
        self.retain_or_release(state.x[0], true); return std::uint64_t{0};
    });
    implementation->install("_dispatch_release", [](auto& self, auto& state, auto&) {
        self.retain_or_release(state.x[0], false); return std::uint64_t{0};
    });
    implementation->install("_dispatch_time", [](auto&, auto& state, auto&) {
        const auto when = state.x[0];
        if (when == forever) return forever;
        const bool wall = when == wall_now || (when & wall_bit) != 0;
        const auto base = when == 0 || when == wall_now ? now(wall) : when & ~wall_bit;
        return deadline(base, std::bit_cast<std::int64_t>(state.x[1]), wall);
    });
    implementation->install("_dispatch_walltime", [](auto&, auto& state, auto& memory) {
        auto base = now(true);
        if (state.x[0]) {
            const auto address = state.x[0];
            if (address > UINT64_MAX - 15) throw std::runtime_error("dispatch_walltime address overflow");
            const auto seconds = std::bit_cast<std::int64_t>(memory.read64(address));
            const auto nanos = std::bit_cast<std::int64_t>(memory.read64(address + 8));
            if (nanos < 0 || nanos >= 1'000'000'000) throw std::runtime_error("invalid Darwin dispatch_walltime timespec");
            if (seconds < 0) throw std::runtime_error("dispatch_walltime before the Unix epoch is unsupported");
            const auto count = static_cast<std::uint64_t>(seconds);
            if (count > (maximum_time - static_cast<std::uint64_t>(nanos)) / 1'000'000'000) return forever;
            base = count * 1'000'000'000 + static_cast<std::uint64_t>(nanos);
        }
        return deadline(base, std::bit_cast<std::int64_t>(state.x[1]), true);
    });
    implementation->install_queues();
    std::lock_guard lock(registry().mutex);
    registry().processes.insert_or_assign(&context, implementation);
}

namespace {
std::shared_ptr<Dispatch> dispatch_of(ProcessContext& context) {
    std::lock_guard lock(registry().mutex);
    const auto found = registry().processes.find(&context);
    return found == registry().processes.end() ? nullptr : found->second.lock();
}
} // namespace

std::size_t drain_dispatch_main_queue(ProcessContext& context, std::size_t limit) {
    const auto dispatch = dispatch_of(context);
    return dispatch ? dispatch->drain_main(limit) : 0;
}

std::size_t dispatch_main_queue_length(ProcessContext& context) {
    const auto dispatch = dispatch_of(context);
    return dispatch ? dispatch->main_length() : 0;
}

void set_dispatch_object_classes(ProcessContext& context, const DispatchObjectClasses& classes) {
    const auto dispatch = dispatch_of(context);
    if (!dispatch) throw std::runtime_error("dispatch object classes need the dispatch provider");
    dispatch->set_classes(classes);
}

std::optional<DispatchObjectInfo> dispatch_object_info(ProcessContext& context, std::uint64_t object) {
    const auto dispatch = dispatch_of(context);
    return dispatch ? dispatch->info(object) : std::nullopt;
}

void add_dispatch_reference_handler(ProcessContext& context, DispatchReferenceHandler handler) {
    const auto dispatch = dispatch_of(context);
    if (!dispatch) throw std::runtime_error("dispatch reference handlers need the dispatch provider");
    std::lock_guard lock(dispatch->metadata);
    dispatch->reference_handlers.push_back(std::move(handler));
}
} // namespace alchemy
