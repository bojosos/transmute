#include "alchemy/darwin/kit/completion/runtime/common.hpp"

#include <map>

// NSNotificationQueue (Foundation) and NSProcessInfo's automatic-termination
// calls.
//
// A notification queue belongs to the thread that created it (+defaultQueue:
// one per thread, for the default center) and posts through its center:
//   NSPostNow     coalesces (removes matching queued notifications), then posts at once;
//   NSPostASAP    at the end of the current run loop iteration (before timers,
//                 before sources, before waiting, or at exit of the thread's run loop);
//   NSPostWhenIdle when the thread's run loop is about to wait.
// Coalescing (NSNotificationCoalescingOnName / OnSender): a new ASAP or idle
// notification matching a queued one is dropped. A run loop observer on the
// queue's thread's run loop (common modes) drives posting; notifications
// queued for other modes wait until the run loop runs in one of them.
//
// -[NSProcessInfo disableAutomaticTermination:] / -enableAutomaticTermination:
// keep the documented counter (and -automaticTerminationSupportEnabled).
// Alchemy never terminates an application on its own, so the counter is the
// whole effect, as on macOS while the counter is positive.
namespace alchemy::gui::completion_kit {
namespace {

using namespace rt;

constexpr std::uint64_t post_when_idle = 1, post_asap = 2, post_now = 3;
constexpr std::uint64_t coalesce_name = 1, coalesce_sender = 2;
constexpr std::uint64_t before_timers = 2, before_sources = 4, before_waiting = 32, loop_exit = 128;

struct Entry { Address notification{}, name{}, object{}, modes{}; std::uint64_t style{}; };   // notification, modes: +1

struct QueueState final : ObjectPayload {
    std::mutex mutex;
    Address center{};                 // +1
    std::vector<Entry> entries;
    Address observer{}, loop{};       // the run loop observer (+1) and its run loop
};

struct Queues {
    explicit Queues(Support& value) : support(value) {}
    Support& support;
    std::mutex mutex;
    std::map<std::uint64_t, Address> defaults;   // thread identity -> default queue (kept for the thread's life)
    Address queue_class{}, callout{};

    std::shared_ptr<QueueState> state_of(Address queue) {
        auto state = support.objc.find_host_state<QueueState>(queue);
        if (!state || !state->center) fail("NSNotificationQueue is not initialized");
        return state;
    }
    bool same(Address left, Address right) {
        if (left == right) return true;
        if (!left || !right) return false;
        CallRegs r;
        r.x[2] = right;
        return (support.send(left, "isEqual:", r).x0 & 0xff) != 0;
    }
    bool matches(const Entry& entry, Address name, Address object, std::uint64_t mask) {
        if (!mask) return false;
        if ((mask & coalesce_name) && !same(entry.name, name)) return false;
        if ((mask & coalesce_sender) && entry.object != object) return false;
        return true;
    }
    void release(const Entry& entry) {
        objc_release(support, entry.notification);
        objc_release(support, entry.modes);
    }
    // The run loop observer of the queue's thread: created with the queue.
    void attach_observer(Address queue, QueueState& state) {
        GuestBlock context(support, 40);
        support.memory().write64(context + 8, queue);   // info: the queue (not retained: the queue owns the observer)
        const auto loop = support.call(core_foundation, "_CFRunLoopGetCurrent", {});
        const auto observer = support.call(core_foundation, "_CFRunLoopObserverCreate",
                                           {0, before_timers | before_sources | before_waiting | loop_exit, 1, 0, callout, context});
        const auto common = support.process.resolve(core_foundation, "_kCFRunLoopCommonModes");
        if (!common) fail("NSNotificationQueue needs kCFRunLoopCommonModes");
        static_cast<void>(support.call(core_foundation, "_CFRunLoopAddObserver", {loop, observer, support.memory().read64(common->address)}));
        state.observer = observer;
        state.loop = loop;
    }
    bool in_mode(const Entry& entry, Address loop) {
        const auto mode = support.call(core_foundation, "_CFRunLoopCopyCurrentMode", {loop});
        const std::string current = mode ? support.cf.utf8(mode) : std::string("kCFRunLoopDefaultMode");
        if (mode) support.release(mode);
        if (!entry.modes) return current == "kCFRunLoopDefaultMode";
        for (const auto value : support.cf.array_values(entry.modes)) {
            const auto name = support.cf.utf8(value);
            if (name == current || name == "kCFRunLoopCommonModes") return true;
        }
        return false;
    }
    void flush(Address queue, std::uint64_t activity) {
        const auto state = state_of(queue);
        std::vector<Entry> due;
        {
            std::lock_guard lock(state->mutex);
            for (auto it = state->entries.begin(); it != state->entries.end();) {
                const bool time = it->style == post_asap || (it->style == post_when_idle && activity == before_waiting);
                if (time) { due.push_back(*it); it = state->entries.erase(it); } else ++it;
            }
        }
        std::vector<Entry> later;
        for (const auto& entry : due) {
            if (!in_mode(entry, state->loop)) { later.push_back(entry); continue; }
            static_cast<void>(support.send_id(state->center, "postNotification:", {entry.notification}));
            release(entry);
        }
        if (later.empty()) return;
        std::lock_guard lock(state->mutex);
        state->entries.insert(state->entries.begin(), later.begin(), later.end());
    }
    void enqueue(Address queue, Address notification, std::uint64_t style, std::uint64_t mask, Address modes) {
        if (!notification) fail("NSInvalidArgumentException: -[NSNotificationQueue enqueueNotification:...] with nil");
        if (style < post_when_idle || style > post_now) fail("NSInvalidArgumentException: invalid NSPostingStyle " + std::to_string(style));
        const auto state = state_of(queue);
        const auto name = support.send_id(notification, "name"), object = support.send_id(notification, "object");
        std::vector<Entry> removed;
        {
            std::lock_guard lock(state->mutex);
            if (style == post_now) {
                for (auto it = state->entries.begin(); it != state->entries.end();) {
                    if (matches(*it, name, object, mask)) { removed.push_back(*it); it = state->entries.erase(it); } else ++it;
                }
            } else {
                for (const auto& entry : state->entries) if (matches(entry, name, object, mask)) return;
                state->entries.push_back({objc_retain(support, notification), name, object, modes ? support.send_id(modes, "copy") : 0, style});
            }
        }
        for (const auto& entry : removed) release(entry);
        if (style == post_now) static_cast<void>(support.send_id(state->center, "postNotification:", {notification}));
    }
    void dequeue(Address queue, Address notification, std::uint64_t mask) {
        const auto state = state_of(queue);
        const auto name = support.send_id(notification, "name"), object = support.send_id(notification, "object");
        std::vector<Entry> removed;
        {
            std::lock_guard lock(state->mutex);
            for (auto it = state->entries.begin(); it != state->entries.end();) {
                if (matches(*it, name, object, mask ? mask : coalesce_name | coalesce_sender)) { removed.push_back(*it); it = state->entries.erase(it); }
                else ++it;
            }
        }
        for (const auto& entry : removed) release(entry);
    }
};

} // namespace

void install_notification_queue(Support& support) {
    auto self = std::make_shared<Queues>(support);
    const auto cls = self->queue_class = support.declare_class("NSNotificationQueue", "NSObject", foundation_library);
    self->callout = support.process.install_function("___alchemy_completion_notification_queue_observer", [self](CpuState& s, GuestMemory&) {
        // void callout(CFRunLoopObserverRef, CFRunLoopActivity, void* info)
        if (s.x[2]) self->flush(s.x[2], s.x[1]);
        s.x[0] = 0;
    }, "alchemy.internal");
    support.method(cls, "initWithNotificationCenter:", [self](CpuState& s, GuestMemory&) {
        if (!s.x[2]) fail("NSInvalidArgumentException: -[NSNotificationQueue initWithNotificationCenter:] with nil");
        const auto state = self->support.objc.host_state<QueueState>(s.x[0]);
        state->center = objc_retain(self->support, s.x[2]);
        self->attach_observer(s.x[0], *state);
    }, "@@:@");
    support.method(cls, "init", [self](CpuState& s, GuestMemory&) {
        const auto center = self->support.send_id(class_named(self->support, "NSNotificationCenter"), "defaultCenter");
        s.x[0] = self->support.send_id(s.x[0], "initWithNotificationCenter:", {center});
    }, "@@:");
    support.method(cls, "defaultQueue", [self, cls](CpuState& s, GuestMemory&) {
        const auto thread = self->support.process.current_thread()->identity;
        {
            std::lock_guard lock(self->mutex);
            if (const auto found = self->defaults.find(thread); found != self->defaults.end()) { s.x[0] = found->second; return; }
        }
        const auto queue = self->support.send_id(self->support.send_id(cls, "alloc"), "init");
        std::lock_guard lock(self->mutex);
        self->defaults[thread] = queue;
        s.x[0] = queue;
    }, "@@:", true);
    support.method(cls, "dealloc", [self, cls](CpuState& s, GuestMemory&) {
        if (const auto state = self->support.objc.find_host_state<QueueState>(s.x[0])) {
            std::vector<Entry> entries;
            Address observer, center;
            {
                std::lock_guard lock(state->mutex);
                entries.swap(state->entries);
                observer = std::exchange(state->observer, 0);
                center = std::exchange(state->center, 0);
            }
            for (const auto& entry : entries) self->release(entry);
            if (observer) {
                static_cast<void>(self->support.call(core_foundation, "_CFRunLoopObserverInvalidate", {observer}));
                self->support.release(observer);
            }
            objc_release(self->support, center);
        }
        static_cast<void>(self->support.objc.send_super(s.x[0], cls, self->support.objc.selector("dealloc")));
    }, "v@:");
    support.method(cls, "enqueueNotification:postingStyle:", [self](CpuState& s, GuestMemory&) {
        self->enqueue(s.x[0], s.x[2], s.x[3], coalesce_name | coalesce_sender, 0);
    }, "v@:@Q");
    support.method(cls, "enqueueNotification:postingStyle:coalesceMask:forModes:", [self](CpuState& s, GuestMemory&) {
        self->enqueue(s.x[0], s.x[2], s.x[3], s.x[4], s.x[5]);
    }, "v@:@QQ@");
    support.method(cls, "dequeueNotificationsMatching:coalesceMask:", [self](CpuState& s, GuestMemory&) {
        if (s.x[2]) self->dequeue(s.x[0], s.x[2], s.x[3]);
    }, "v@:@Q");

    // ---- NSProcessInfo automatic termination ----
    const auto info = class_named(support, "NSProcessInfo");
    struct Termination { std::mutex mutex; std::int64_t disabled{}; bool supported{}; };
    auto termination = std::make_shared<Termination>();
    support.method(info, "disableAutomaticTermination:", [termination](CpuState&, GuestMemory&) {
        std::lock_guard lock(termination->mutex);
        ++termination->disabled;
    }, "v@:@");
    support.method(info, "enableAutomaticTermination:", [termination](CpuState&, GuestMemory&) {
        std::lock_guard lock(termination->mutex);
        --termination->disabled;
    }, "v@:@");
    support.method(info, "automaticTerminationSupportEnabled", [termination](CpuState& s, GuestMemory&) {
        std::lock_guard lock(termination->mutex);
        s.x[0] = termination->supported;
    }, "B@:");
    support.method(info, "setAutomaticTerminationSupportEnabled:", [termination](CpuState& s, GuestMemory&) {
        std::lock_guard lock(termination->mutex);
        termination->supported = (s.x[2] & 0xff) != 0;
    }, "v@:B");
}

} // namespace alchemy::gui::completion_kit
