#include "alchemy/darwin/kit/completion/completion_kit.hpp"

#include "alchemy/host/process.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <map>
#include <optional>
#include <utility>
#include <string>
#include <vector>

// Foundation classes QLab's bundled frameworks use at launch that the
// Foundation providers lack.
//
// NSDistributedNotificationCenter (Syphon announces and discovers servers
// through it): there is no system-wide notification server under Alchemy, so
// the default center delivers its notifications to this process's observers
// only, synchronously, like NSNotificationCenter. Observers registered here
// are separate from the default NSNotificationCenter's. Objects are sender
// names (NSString) and match by string equality.
namespace alchemy::gui::completion_kit {
namespace {

struct Observer { Address observer{}, selector{}, name{}, object{}; };

struct DistributedCenter {
    std::mutex mutex;
    std::vector<Observer> observers;
    Address object{};
    bool suspended{};
};

// NSKeyedUnarchiver/NSKeyedArchiver class-name translation tables (the
// class methods apply to every archiver; per-instance tables are not kept).
struct ClassNames {
    std::mutex mutex;
    std::map<std::string, Address> decode;    // archived name -> class
    std::map<Address, std::string> encode;    // class -> archived name
};

void install_keyed_archiver(Support& support) {
    auto names = std::make_shared<ClassNames>();
    const auto coder = support.objc.find_class("NSCoder") ? "NSCoder" : "NSObject";
    const auto unarchiver = support.declare_class("NSKeyedUnarchiver", coder, foundation_library);
    const auto archiver = support.declare_class("NSKeyedArchiver", coder, foundation_library);
    const auto string_of = [&support](Address value) {
        if (!value) fail("NSKeyedArchiver class name is nil");
        const auto utf8 = support.send_id(value, "UTF8String");
        if (!utf8) fail("NSKeyedArchiver class name is not a string");
        std::string text;
        for (auto at = utf8;; ++at) { const auto c = support.memory().read8(at); if (!c) break; text.push_back(static_cast<char>(c)); }
        return text;
    };
    for (const bool class_method : {true, false}) {
        support.method(unarchiver, "setClass:forClassName:", [names, string_of](CpuState& s, GuestMemory&) {
            const auto name = string_of(s.x[3]);
            std::lock_guard lock(names->mutex);
            if (s.x[2]) names->decode[name] = s.x[2]; else names->decode.erase(name);
        }, "v@:#@", class_method);
        support.method(unarchiver, "classForClassName:", [names, string_of](CpuState& s, GuestMemory&) {
            const auto name = string_of(s.x[2]);
            std::lock_guard lock(names->mutex);
            const auto found = names->decode.find(name);
            s.x[0] = found == names->decode.end() ? 0 : found->second;
        }, "#@:@", class_method);
        support.method(archiver, "setClassName:forClass:", [names, string_of](CpuState& s, GuestMemory&) {
            std::lock_guard lock(names->mutex);
            if (!s.x[3]) return;
            if (s.x[2]) names->encode[s.x[3]] = string_of(s.x[2]); else names->encode.erase(s.x[3]);
        }, "v@:@#", class_method);
        support.method(archiver, "classNameForClass:", [&support, names](CpuState& s, GuestMemory&) {
            std::string name;
            { std::lock_guard lock(names->mutex); const auto found = names->encode.find(s.x[2]); if (found != names->encode.end()) name = found->second; }
            s.x[0] = name.empty() ? 0 : support.cf.constant_string(name);
        }, "@@:#", class_method);
    }
}

// NSComparisonMethods (Foundation's scripting comparison category on
// NSObject): equality through -isEqual:, ordering through -compare:.
void install_comparison_methods(Support& support) {
    const auto object = support.objc.find_class("NSObject");
    if (!object) fail("NSObject is not provided");
    const auto equal = [&support](Address left, Address right) {
        CallRegs registers;
        registers.x[2] = right;
        return (support.send(left, "isEqual:", registers).x0 & 0xff) != 0;
    };
    support.method(object, "isEqualTo:", [equal](CpuState& s, GuestMemory&) { s.x[0] = equal(s.x[0], s.x[2]); }, "B@:@");
    support.method(object, "isNotEqualTo:", [equal](CpuState& s, GuestMemory&) { s.x[0] = !equal(s.x[0], s.x[2]); }, "B@:@");
    const auto order = [&support](Address left, Address right) {
        if (!support.objc.responds(left, support.objc.selector("compare:"))) unsupported("comparison of an object without -compare:");
        CallRegs registers;
        registers.x[2] = right;
        return static_cast<std::int64_t>(support.send(left, "compare:", registers).x0);
    };
    const std::array<std::pair<const char*, int>, 4> comparisons{{
        {"isLessThan:", 0}, {"isLessThanOrEqualTo:", 1}, {"isGreaterThan:", 2}, {"isGreaterThanOrEqualTo:", 3}}};
    for (const auto& [name, kind] : comparisons)
        support.method(object, name, [order, kind = kind](CpuState& s, GuestMemory&) {
            const auto result = s.x[2] ? order(s.x[0], s.x[2]) : 1;
            s.x[0] = kind == 0 ? result < 0 : kind == 1 ? result <= 0 : kind == 2 ? result > 0 : result >= 0;
        }, "B@:@");
}

// +[NSThread detachNewThreadSelector:toTarget:withObject:] and
// +detachNewThreadWithBlock:: a detached guest pthread sends the message (or
// runs the block) inside an autorelease pool; target and argument are
// retained until it returns.
void install_thread_detach(Support& support) {
    const auto thread_class = support.objc.find_class("NSThread");
    if (!thread_class) return;
    struct Work { Address target{}, selector{}, argument{}, block{}; };
    struct Pending { std::mutex mutex; std::map<Address, Work> work; Address next{1}; };
    auto pending = std::make_shared<Pending>();
    static constexpr std::string_view libsystem = "/usr/lib/libSystem.B.dylib";
    const auto entry = support.process.install_function("___alchemy_completion_nsthread_main", [&support, pending](CpuState& s, GuestMemory&) {
        Work work;
        {
            std::lock_guard lock(pending->mutex);
            const auto found = pending->work.find(s.x[0]);
            if (found == pending->work.end()) fail("NSThread entry without its work");
            work = found->second;
            pending->work.erase(found);
        }
        const auto pool = support.call("/usr/lib/libobjc.A.dylib", "_objc_autoreleasePoolPush", {});
        if (work.block) {
            const auto invoke = support.memory().read64(work.block + 16);
            const std::array<Address, 1> arguments{work.block};
            static_cast<void>(support.process.calling_engine().invoke_callback(invoke, arguments));
        } else {
            CallRegs registers;
            registers.x[2] = work.argument;
            static_cast<void>(support.objc.send(work.target, work.selector, registers));
        }
        static_cast<void>(support.call("/usr/lib/libobjc.A.dylib", "_objc_autoreleasePoolPop", {pool}));
        if (work.block) static_cast<void>(support.call(libsystem, "__Block_release", {work.block}));
        support.send_id(work.target, "release");
        if (work.argument) support.send_id(work.argument, "release");
        s.x[0] = 0;
    });
    const auto start = [&support, pending, entry](Work work) {
        Address token{};
        { std::lock_guard lock(pending->mutex); token = pending->next++; pending->work.emplace(token, work); }
        const auto handle = support.process.allocate_guest(8);
        const auto result = support.call(libsystem, "_pthread_create", {handle, 0, entry, token});
        const auto thread = support.memory().read64(handle);
        support.process.release_guest(handle);
        if (static_cast<std::uint32_t>(result)) fail("NSThread could not start a thread");
        static_cast<void>(support.call(libsystem, "_pthread_detach", {thread}));
    };
    support.method(thread_class, "detachNewThreadSelector:toTarget:withObject:", [&support, start](CpuState& s, GuestMemory&) {
        if (!s.x[2] || !s.x[3]) fail("detachNewThreadSelector: needs a selector and a target");
        Work work{support.send_id(s.x[3], "retain"), s.x[2], s.x[4] ? support.send_id(s.x[4], "retain") : 0, 0};
        start(work);
    }, "v@::@@", true);
    // Thread priorities are advisory: the host scheduler decides.
    struct Priority { std::mutex mutex; std::map<std::uint64_t, double> values; };
    auto priorities = std::make_shared<Priority>();
    support.method(thread_class, "setThreadPriority:", [priorities](CpuState& s, GuestMemory&) {
        std::lock_guard lock(priorities->mutex);
        priorities->values[host::process::current_thread_id()] = std::bit_cast<double>(s.v[0][0]);
        s.x[0] = 1;
    }, "B@:d", true);
    support.method(thread_class, "threadPriority", [priorities](CpuState& s, GuestMemory&) {
        std::lock_guard lock(priorities->mutex);
        const auto found = priorities->values.find(host::process::current_thread_id());
        s.v[0] = {std::bit_cast<std::uint64_t>(found == priorities->values.end() ? 0.5 : found->second), 0};
    }, "d@:", true);
    support.method(thread_class, "detachNewThreadWithBlock:", [&support, start](CpuState& s, GuestMemory&) {
        if (!s.x[2]) fail("detachNewThreadWithBlock: needs a block");
        Work work{0, 0, 0, support.call("/usr/lib/libSystem.B.dylib", "__Block_copy", {s.x[2]})};
        start(work);
    }, "v@:@?", true);
}

// NSCache: a mutable key/value store whose keys are retained (not copied)
// and compared with -isEqual:. Memory pressure never arises, so nothing is
// evicted except through the count limit (oldest first); the delegate hears
// -cache:willEvictObject: for evictions and removals.
void install_cache(Support& support) {
    const auto cache_class = support.declare_class("NSCache", "NSObject", foundation_library);
    struct Entry { Address key{}, value{}; std::uint64_t cost{}; };
    struct CacheState final : ObjectPayload {
        std::mutex mutex;
        std::vector<Entry> entries;
        Address name{}, delegate{};
        std::uint64_t count_limit{}, cost_limit{};
        bool evicts = true;
    };
    const auto state_of = [&support](Address object) { return support.objc.host_state<CacheState>(object); };
    const auto equal = [&support](Address left, Address right) {
        if (left == right) return true;
        CallRegs registers;
        registers.x[2] = right;
        return (support.send(left, "isEqual:", registers).x0 & 0xff) != 0;
    };
    const auto find = [equal](CacheState& state, Address key) -> std::optional<std::size_t> {
        std::vector<Entry> snapshot;
        { std::lock_guard lock(state.mutex); snapshot = state.entries; }
        for (std::size_t index = 0; index < snapshot.size(); ++index) if (equal(snapshot[index].key, key)) return index;
        return std::nullopt;
    };
    const auto evict = [&support](CacheState& state, Address cache, Entry entry) {
        if (state.delegate && support.objc.responds(state.delegate, support.objc.selector("cache:willEvictObject:")))
            static_cast<void>(support.send_id(state.delegate, "cache:willEvictObject:", {cache, entry.value}));
        support.send_id(entry.key, "release");
        support.send_id(entry.value, "release");
    };
    support.method(cache_class, "objectForKey:", [state_of, find](CpuState& s, GuestMemory&) {
        const auto state = state_of(s.x[0]);
        const auto index = s.x[2] ? find(*state, s.x[2]) : std::nullopt;
        std::lock_guard lock(state->mutex);
        s.x[0] = index && *index < state->entries.size() ? state->entries[*index].value : 0;
    }, "@@:@");
    const auto store = [&support, state_of, find, evict](Address cache, Address value, Address key, std::uint64_t cost) {
        if (!key) fail("-[NSCache setObject:forKey:] with a nil key");
        const auto state = state_of(cache);
        if (!value) fail("-[NSCache setObject:forKey:] with a nil object");
        const auto retained_value = support.send_id(value, "retain");
        std::vector<Entry> evicted;
        if (const auto index = find(*state, key)) {
            std::lock_guard lock(state->mutex);
            if (*index < state->entries.size()) {
                evicted.push_back({0, state->entries[*index].value, 0});
                state->entries[*index].value = retained_value;
                state->entries[*index].cost = cost;
            }
        } else {
            const auto retained_key = support.send_id(key, "retain");
            std::lock_guard lock(state->mutex);
            state->entries.push_back({retained_key, retained_value, cost});
            while (state->count_limit && state->entries.size() > state->count_limit) {
                evicted.push_back(state->entries.front());
                state->entries.erase(state->entries.begin());
            }
        }
        for (const auto& entry : evicted) {
            if (entry.key) evict(*state, cache, entry);
            else support.send_id(entry.value, "release");
        }
    };
    support.method(cache_class, "setObject:forKey:", [store](CpuState& s, GuestMemory&) { store(s.x[0], s.x[2], s.x[3], 0); }, "v@:@@");
    support.method(cache_class, "setObject:forKey:cost:", [store](CpuState& s, GuestMemory&) { store(s.x[0], s.x[2], s.x[3], s.x[4]); }, "v@:@@Q");
    support.method(cache_class, "removeObjectForKey:", [state_of, find, evict](CpuState& s, GuestMemory&) {
        const auto state = state_of(s.x[0]);
        const auto index = s.x[2] ? find(*state, s.x[2]) : std::nullopt;
        if (!index) return;
        Entry entry;
        {
            std::lock_guard lock(state->mutex);
            if (*index >= state->entries.size()) return;
            entry = state->entries[*index];
            state->entries.erase(state->entries.begin() + static_cast<std::ptrdiff_t>(*index));
        }
        evict(*state, s.x[0], entry);
    }, "v@:@");
    support.method(cache_class, "removeAllObjects", [state_of, evict](CpuState& s, GuestMemory&) {
        const auto state = state_of(s.x[0]);
        std::vector<Entry> entries;
        { std::lock_guard lock(state->mutex); entries.swap(state->entries); }
        for (const auto& entry : entries) evict(*state, s.x[0], entry);
    }, "v@:");
    support.method(cache_class, "name", [state_of](CpuState& s, GuestMemory&) { s.x[0] = state_of(s.x[0])->name; }, "@@:");
    support.method(cache_class, "setName:", [&support, state_of](CpuState& s, GuestMemory&) {
        const auto state = state_of(s.x[0]);
        const auto copy = s.x[2] ? support.send_id(s.x[2], "copy") : 0;
        support.send_id(std::exchange(state->name, copy), "release");
    }, "v@:@");
    support.method(cache_class, "delegate", [state_of](CpuState& s, GuestMemory&) { s.x[0] = state_of(s.x[0])->delegate; }, "@@:");
    support.method(cache_class, "setDelegate:", [state_of](CpuState& s, GuestMemory&) { state_of(s.x[0])->delegate = s.x[2]; }, "v@:@");
    support.method(cache_class, "countLimit", [state_of](CpuState& s, GuestMemory&) { s.x[0] = state_of(s.x[0])->count_limit; }, "Q@:");
    support.method(cache_class, "setCountLimit:", [state_of](CpuState& s, GuestMemory&) { state_of(s.x[0])->count_limit = s.x[2]; }, "v@:Q");
    support.method(cache_class, "totalCostLimit", [state_of](CpuState& s, GuestMemory&) { s.x[0] = state_of(s.x[0])->cost_limit; }, "Q@:");
    support.method(cache_class, "setTotalCostLimit:", [state_of](CpuState& s, GuestMemory&) { state_of(s.x[0])->cost_limit = s.x[2]; }, "v@:Q");
    support.method(cache_class, "evictsObjectsWithDiscardedContent", [state_of](CpuState& s, GuestMemory&) { s.x[0] = state_of(s.x[0])->evicts; }, "B@:");
    support.method(cache_class, "setEvictsObjectsWithDiscardedContent:", [state_of](CpuState& s, GuestMemory&) { state_of(s.x[0])->evicts = (s.x[2] & 0xff) != 0; }, "v@:B");
    support.method(cache_class, "dealloc", [&support, state_of, cache_class](CpuState& s, GuestMemory&) {
        if (const auto state = support.objc.find_host_state<CacheState>(s.x[0])) {
            std::vector<Entry> entries;
            { std::lock_guard lock(state->mutex); entries.swap(state->entries); }
            for (const auto& entry : entries) { support.send_id(entry.key, "release"); support.send_id(entry.value, "release"); }
            support.send_id(std::exchange(state->name, 0), "release");
        }
        static_cast<void>(support.objc.send_super(s.x[0], cache_class, support.objc.selector("dealloc")));
    }, "v@:");
}

} // namespace

void install_foundation(Support& support) {
    install_cache(support);
    install_keyed_archiver(support);
    install_comparison_methods(support);
    install_thread_detach(support);
    const auto center_class = support.declare_class("NSDistributedNotificationCenter", "NSNotificationCenter", foundation_library);
    auto center = std::make_shared<DistributedCenter>();
    const auto same = [&support](Address left, Address right) {
        if (left == right) return true;
        if (!left || !right) return false;
        return support.cf.equal(left, right);
    };
    const auto default_center = [&support, center, center_class]() {
        std::lock_guard lock(center->mutex);
        if (!center->object) center->object = support.create(center_class, nullptr);
        return center->object;
    };
    for (const auto* name : {"defaultCenter", "notificationCenterForType:"})
        support.method(center_class, name, [default_center](CpuState& s, GuestMemory&) { s.x[0] = default_center(); }, "@@:@", true);
    const auto add = [&support, center](CpuState& s) {
        if (!s.x[2] || !s.x[3]) fail("distributed notification observer and selector are required");
        const auto name = s.x[4] ? support.cf.copy(s.x[4]) : 0;
        const auto object = s.x[5] ? support.cf.copy(s.x[5]) : 0;
        std::lock_guard lock(center->mutex);
        center->observers.push_back({s.x[2], s.x[3], name, object});
    };
    support.method(center_class, "addObserver:selector:name:object:", [add](CpuState& s, GuestMemory&) { add(s); }, "v@:@:@@");
    // The suspension behavior only matters while the application is suspended,
    // which never happens: delivery is always immediate.
    support.method(center_class, "addObserver:selector:name:object:suspensionBehavior:",
        [add](CpuState& s, GuestMemory&) { add(s); }, "v@:@:@@Q");
    const auto remove = [&support, center, same](Address observer, Address name, Address object) {
        std::vector<Address> released;
        {
            std::lock_guard lock(center->mutex);
            auto& list = center->observers;
            for (auto it = list.begin(); it != list.end();) {
                if (it->observer == observer && (!name || same(it->name, name)) && (!object || same(it->object, object))) {
                    released.push_back(it->name); released.push_back(it->object);
                    it = list.erase(it);
                } else ++it;
            }
        }
        for (const auto value : released) support.release(value);
    };
    support.method(center_class, "removeObserver:", [remove](CpuState& s, GuestMemory&) { remove(s.x[2], 0, 0); }, "v@:@");
    support.method(center_class, "removeObserver:name:object:", [remove](CpuState& s, GuestMemory&) { remove(s.x[2], s.x[3], s.x[4]); }, "v@:@@@");
    const auto deliver = [&support, center, same](Address notification) {
        const auto name = support.send_id(notification, "name"), object = support.send_id(notification, "object");
        std::vector<Observer> targets;
        {
            std::lock_guard lock(center->mutex);
            for (const auto& entry : center->observers)
                if ((!entry.name || same(entry.name, name)) && (!entry.object || same(entry.object, object))) targets.push_back(entry);
        }
        for (const auto& target : targets) {
            bool live{};
            {
                std::lock_guard lock(center->mutex);
                live = std::any_of(center->observers.begin(), center->observers.end(), [&](const Observer& entry) {
                    return entry.observer == target.observer && entry.selector == target.selector && entry.name == target.name;
                });
            }
            if (!live) continue; // Removed by an earlier observer during this post.
            CallRegs registers;
            registers.x[2] = notification;
            static_cast<void>(support.objc.send(target.observer, target.selector, registers));
        }
    };
    const auto post = [&support, deliver](Address name, Address object, Address user_info) {
        if (!name) fail("distributed notification requires a name");
        const auto notification_class = support.objc.find_class("NSNotification");
        if (!notification_class) fail("NSNotification is not provided");
        const auto notification = support.send_id(notification_class, "notificationWithName:object:userInfo:", {name, object, user_info});
        deliver(notification);
    };
    support.method(center_class, "postNotification:", [deliver](CpuState& s, GuestMemory&) { if (s.x[2]) deliver(s.x[2]); }, "v@:@");
    support.method(center_class, "postNotificationName:object:", [post](CpuState& s, GuestMemory&) { post(s.x[2], s.x[3], 0); }, "v@:@@");
    support.method(center_class, "postNotificationName:object:userInfo:", [post](CpuState& s, GuestMemory&) { post(s.x[2], s.x[3], s.x[4]); }, "v@:@@@");
    support.method(center_class, "postNotificationName:object:userInfo:deliverImmediately:",
        [post](CpuState& s, GuestMemory&) { post(s.x[2], s.x[3], s.x[4]); }, "v@:@@@B");
    support.method(center_class, "postNotificationName:object:userInfo:options:",
        [post](CpuState& s, GuestMemory&) { post(s.x[2], s.x[3], s.x[4]); }, "v@:@@@Q");
    support.method(center_class, "setSuspended:", [center](CpuState& s, GuestMemory&) {
        std::lock_guard lock(center->mutex); center->suspended = (s.x[2] & 0xff) != 0;
    }, "v@:B");
    support.method(center_class, "suspended", [center](CpuState& s, GuestMemory&) {
        std::lock_guard lock(center->mutex); s.x[0] = center->suspended;
    }, "B@:");
}

} // namespace alchemy::gui::completion_kit
