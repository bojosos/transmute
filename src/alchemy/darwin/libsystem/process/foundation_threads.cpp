#include "alchemy/darwin/kit/completion/runtime/common.hpp"

#include <bit>
#include <cmath>
#include <cstdio>

// NSThread additions and the Foundation locks.
//
// +[NSThread sleepForTimeInterval:] / +sleepUntilDate: sleep through the
// guest's nanosleep (so a process exit still ends the wait).
// +[NSThread callStackReturnAddresses] / +callStackSymbols walk the calling
// guest thread's frame-pointer chain (the arm64 frame record {fp, lr}); the
// symbols use Foundation's layout "<index> <image> 0x<address> <image> +
// <offset>" (the form macOS prints for images without symbol names).
//
// NSLock, NSRecursiveLock, NSCondition and NSConditionLock are built on the
// guest's pthread mutexes and condition variables (each lock owns guest
// storage for them): ownership, recursion and waiting follow libSystem's
// pthread semantics. -lockBeforeDate: polls -tryLock until the date. Added
// only for classes no provider defines.
namespace alchemy::gui::completion_kit {
namespace {

using namespace rt;

constexpr Address mutex_size = 64, condition_size = 48, attribute_size = 16;
constexpr std::uint64_t recursive_mutex = 2;   // PTHREAD_MUTEX_RECURSIVE

struct LockState final : ObjectPayload {
    std::mutex mutex;
    Address storage{};          // guest: pthread_mutex_t at 0, pthread_cond_t at 64
    Address name{};             // +1
    std::int64_t condition{};   // NSConditionLock
};

double date_seconds(Support& support, Address date) {
    if (!date) return -INFINITY;
    return support.send(date, "timeIntervalSinceReferenceDate").d(0);
}

void sleep_seconds(Support& support, double seconds) {
    if (!(seconds > 0)) return;
    seconds = std::min(seconds, 86400.0);
    GuestBlock spec(support, 16);
    const auto whole = std::floor(seconds);
    support.memory().write64(spec, static_cast<std::uint64_t>(whole));
    support.memory().write64(spec + 8, static_cast<std::uint64_t>((seconds - whole) * 1e9));
    static_cast<void>(support.call(libsystem, "_nanosleep", {spec, 0}));
}

} // namespace

std::vector<Address> guest_backtrace(Support& support, const CpuState& state, std::size_t limit) {
    std::vector<Address> frames;
    if (state.x[30]) frames.push_back(state.x[30]);
    auto frame = state.x[29];
    auto& memory = support.memory();
    while (frame && (frame & 7) == 0 && frames.size() < limit) {
        Address next{}, address{};
        try {
            next = memory.read64(frame);
            address = memory.read64(frame + 8);
        } catch (const std::exception&) { break; }
        if (!address) break;
        frames.push_back(address);
        if (next <= frame) break;   // frame records go up the stack
        frame = next;
    }
    return frames;
}

std::vector<std::string> symbolize(Support& support, const std::vector<Address>& frames) {
    const auto images = support.process.images();
    std::vector<std::string> lines;
    for (std::size_t index = 0; index < frames.size(); ++index) {
        const auto address = frames[index];
        std::string image = "???";
        Address base{};
        for (const auto& value : images) {
            if (address < value.begin || address >= value.end) continue;
            image = value.name.substr(value.name.find_last_of('/') + 1);
            base = value.header;
            break;
        }
        char line[512];
        if (base) std::snprintf(line, sizeof(line), "%-4zu%-35s 0x%016llx %s + %llu", index, image.c_str(),
                                static_cast<unsigned long long>(address), image.c_str(), static_cast<unsigned long long>(address - base));
        else std::snprintf(line, sizeof(line), "%-4zu%-35s 0x%016llx 0x%llx + 0", index, image.c_str(),
                           static_cast<unsigned long long>(address), static_cast<unsigned long long>(address));
        lines.emplace_back(line);
    }
    return lines;
}

void install_threads(Support& support) {
    auto thread = support.objc.find_class("NSThread");
    if (!thread) thread = support.declare_class("NSThread", "NSObject", foundation_library);
    support.method(thread, "sleepForTimeInterval:", [&support](CpuState& s, GuestMemory&) {
        sleep_seconds(support, std::bit_cast<double>(s.v[0][0]));
    }, "v@:d", true);
    support.method(thread, "sleepUntilDate:", [&support](CpuState& s, GuestMemory&) {
        sleep_seconds(support, date_seconds(support, s.x[2]) - CoreFoundation::absolute_time_now());
    }, "v@:@", true);
    support.method(thread, "callStackReturnAddresses", [&support](CpuState& s, GuestMemory&) {
        std::vector<Address> values;
        for (const auto address : guest_backtrace(support, s, 256)) values.push_back(support.cf.integer(static_cast<std::int64_t>(address), 4));
        s.x[0] = array(support, values);
        for (const auto value : values) support.release(value);
    }, "@@:", true);
    support.method(thread, "callStackSymbols", [&support](CpuState& s, GuestMemory&) {
        std::vector<Address> values;
        for (const auto& line : symbolize(support, guest_backtrace(support, s, 256))) values.push_back(support.cf.string(line));
        s.x[0] = array(support, values);
        for (const auto value : values) support.release(value);
    }, "@@:", true);
}

void install_locks(Support& support) {
    const auto object = class_named(support, "NSObject");
    static_cast<void>(object);
    const auto pthread = [&support](std::string_view symbol, std::initializer_list<Address> arguments) {
        return static_cast<std::int32_t>(support.call(libsystem, symbol, arguments));
    };
    // Guest storage for the mutex (recursive or not) and the condition.
    const auto create = [&support, pthread](Address self, bool recursive, bool condition) {
        auto state = support.objc.host_state<LockState>(self);
        std::lock_guard lock(state->mutex);
        if (state->storage) return state;
        const auto storage = support.process.allocate_guest(mutex_size + condition_size, 16);
        support.memory().fill_bytes(storage, 0, mutex_size + condition_size);
        GuestBlock attribute(support, attribute_size);
        if (pthread("_pthread_mutexattr_init", {attribute}) != 0) fail("pthread_mutexattr_init failed");
        if (recursive && pthread("_pthread_mutexattr_settype", {attribute, recursive_mutex}) != 0) fail("pthread_mutexattr_settype failed");
        if (pthread("_pthread_mutex_init", {storage, attribute}) != 0) fail("pthread_mutex_init failed");
        static_cast<void>(pthread("_pthread_mutexattr_destroy", {attribute}));
        if (condition && pthread("_pthread_cond_init", {storage + mutex_size, 0}) != 0) fail("pthread_cond_init failed");
        state->storage = storage;
        return state;
    };
    const auto storage_of = [&support](Address self) {
        const auto state = support.objc.find_host_state<LockState>(self);
        if (!state || !state->storage) fail("lock object is not initialized (send -init first)");
        return state->storage;
    };
    const auto destroy = [&support, pthread](Address self, bool condition) {
        const auto state = support.objc.find_host_state<LockState>(self);
        if (!state) return;
        Address storage, name;
        { std::lock_guard lock(state->mutex); storage = std::exchange(state->storage, 0); name = std::exchange(state->name, 0); }
        if (storage) {
            static_cast<void>(pthread("_pthread_mutex_destroy", {storage}));
            if (condition) static_cast<void>(pthread("_pthread_cond_destroy", {storage + mutex_size}));
            support.process.release_guest(storage);
        }
        support.release(name);
    };
    const auto check = [](std::int32_t result, std::string_view what) {
        if (result != 0) fail(std::string(what) + " failed (errno " + std::to_string(result) + ")");
    };
    const auto try_until = [&support, pthread, storage_of](Address self, double until, auto&& ready) {
        for (;;) {
            if (pthread("_pthread_mutex_trylock", {storage_of(self)}) == 0) {
                if (ready()) return true;
                static_cast<void>(pthread("_pthread_mutex_unlock", {storage_of(self)}));
            }
            if (CoreFoundation::absolute_time_now() >= until) return false;
            sleep_seconds(support, 0.001);
        }
    };
    const auto add_name = [&support](Address cls) {
        support.method(cls, "name", [&support](CpuState& s, GuestMemory&) {
            const auto state = support.objc.find_host_state<LockState>(s.x[0]);
            if (!state) { s.x[0] = 0; return; }
            std::lock_guard lock(state->mutex);
            s.x[0] = state->name;
        }, "@@:");
        support.method(cls, "setName:", [&support](CpuState& s, GuestMemory&) {
            auto state = support.objc.host_state<LockState>(s.x[0]);
            const auto copy = s.x[2] ? support.send_id(s.x[2], "copy") : 0;
            Address old;
            { std::lock_guard lock(state->mutex); old = std::exchange(state->name, copy); }
            support.release(old);
        }, "v@:@");
    };
    // `unlock`: false for NSConditionLock, whose -unlock also wakes its waiters.
    const auto add_basic = [&support, pthread, storage_of, check, try_until](Address cls, bool unlock) {
        support.method(cls, "lock", [pthread, storage_of, check](CpuState& s, GuestMemory&) {
            check(pthread("_pthread_mutex_lock", {storage_of(s.x[0])}), "-[NSLock lock]");
        }, "v@:");
        if (unlock) support.method(cls, "unlock", [pthread, storage_of, check](CpuState& s, GuestMemory&) {
            check(pthread("_pthread_mutex_unlock", {storage_of(s.x[0])}), "-[NSLock unlock] (not locked by this thread)");
        }, "v@:");
        support.method(cls, "tryLock", [pthread, storage_of](CpuState& s, GuestMemory&) {
            s.x[0] = pthread("_pthread_mutex_trylock", {storage_of(s.x[0])}) == 0;
        }, "B@:");
        support.method(cls, "lockBeforeDate:", [&support, try_until](CpuState& s, GuestMemory&) {
            s.x[0] = try_until(s.x[0], date_seconds(support, s.x[2]), [] { return true; });
        }, "B@:@");
    };
    struct Kind { const char* name; bool recursive, condition; };
    for (const auto& kind : {Kind{"NSLock", false, false}, Kind{"NSRecursiveLock", true, false}, Kind{"NSCondition", false, true},
                             Kind{"NSConditionLock", false, true}}) {
        if (support.objc.find_class(kind.name)) continue;
        const auto cls = support.declare_class(kind.name, "NSObject", foundation_library);
        support.objc.add_native_protocol(cls, "NSLocking");
        const bool recursive = kind.recursive, condition = kind.condition;
        support.method(cls, "init", [create, recursive, condition](CpuState& s, GuestMemory&) {
            static_cast<void>(create(s.x[0], recursive, condition));
        }, "@@:");
        support.method(cls, "dealloc", [&support, destroy, cls, condition](CpuState& s, GuestMemory&) {
            destroy(s.x[0], condition);
            static_cast<void>(support.objc.send_super(s.x[0], cls, support.objc.selector("dealloc")));
        }, "v@:");
        add_basic(cls, std::string_view(kind.name) != "NSConditionLock");
        add_name(cls);
        if (std::string_view(kind.name) == "NSCondition") {
            support.method(cls, "wait", [pthread, storage_of, check](CpuState& s, GuestMemory&) {
                const auto storage = storage_of(s.x[0]);
                check(pthread("_pthread_cond_wait", {storage + mutex_size, storage}), "-[NSCondition wait]");
            }, "v@:");
            support.method(cls, "waitUntilDate:", [&support, pthread, storage_of](CpuState& s, GuestMemory&) {
                const auto storage = storage_of(s.x[0]);
                // pthread_cond_timedwait takes an absolute CLOCK_REALTIME timespec.
                const auto unix_seconds = date_seconds(support, s.x[2]) + 978307200.0;
                GuestBlock spec(support, 16);
                const auto whole = std::floor(std::max(0.0, unix_seconds));
                support.memory().write64(spec, static_cast<std::uint64_t>(whole));
                support.memory().write64(spec + 8, static_cast<std::uint64_t>((std::max(0.0, unix_seconds) - whole) * 1e9));
                s.x[0] = pthread("_pthread_cond_timedwait", {storage + mutex_size, storage, spec}) == 0;
            }, "B@:@");
            support.method(cls, "signal", [pthread, storage_of](CpuState& s, GuestMemory&) {
                static_cast<void>(pthread("_pthread_cond_signal", {storage_of(s.x[0]) + mutex_size}));
            }, "v@:");
            support.method(cls, "broadcast", [pthread, storage_of](CpuState& s, GuestMemory&) {
                static_cast<void>(pthread("_pthread_cond_broadcast", {storage_of(s.x[0]) + mutex_size}));
            }, "v@:");
        }
        if (std::string_view(kind.name) == "NSConditionLock") {
            const auto value_of = [&support](Address self) -> std::int64_t& {
                return support.objc.host_state<LockState>(self)->condition;
            };
            support.method(cls, "initWithCondition:", [create, value_of](CpuState& s, GuestMemory&) {
                static_cast<void>(create(s.x[0], false, true));
                value_of(s.x[0]) = static_cast<std::int64_t>(s.x[2]);
            }, "@@:q");
            support.method(cls, "condition", [value_of](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(value_of(s.x[0])); }, "q@:");
            const auto wait_for = [pthread, storage_of, check, value_of](Address self, std::int64_t wanted) {
                const auto storage = storage_of(self);
                check(pthread("_pthread_mutex_lock", {storage}), "-[NSConditionLock lockWhenCondition:]");
                while (value_of(self) != wanted)
                    check(pthread("_pthread_cond_wait", {storage + mutex_size, storage}), "-[NSConditionLock lockWhenCondition:]");
            };
            support.method(cls, "lockWhenCondition:", [wait_for](CpuState& s, GuestMemory&) {
                wait_for(s.x[0], static_cast<std::int64_t>(s.x[2]));
            }, "v@:q");
            support.method(cls, "unlockWithCondition:", [pthread, storage_of, check, value_of](CpuState& s, GuestMemory&) {
                const auto storage = storage_of(s.x[0]);
                value_of(s.x[0]) = static_cast<std::int64_t>(s.x[2]);
                static_cast<void>(pthread("_pthread_cond_broadcast", {storage + mutex_size}));
                check(pthread("_pthread_mutex_unlock", {storage}), "-[NSConditionLock unlockWithCondition:]");
            }, "v@:q");
            support.method(cls, "tryLockWhenCondition:", [pthread, storage_of, value_of](CpuState& s, GuestMemory&) {
                const auto storage = storage_of(s.x[0]);
                if (pthread("_pthread_mutex_trylock", {storage}) != 0) { s.x[0] = 0; return; }
                if (value_of(s.x[0]) == static_cast<std::int64_t>(s.x[2])) { s.x[0] = 1; return; }
                static_cast<void>(pthread("_pthread_mutex_unlock", {storage}));
                s.x[0] = 0;
            }, "B@:q");
            support.method(cls, "lockWhenCondition:beforeDate:", [&support, try_until, value_of](CpuState& s, GuestMemory&) {
                const auto wanted = static_cast<std::int64_t>(s.x[2]);
                const auto self = s.x[0];
                s.x[0] = try_until(self, date_seconds(support, s.x[3]), [&] { return value_of(self) == wanted; });
            }, "B@:q@");
            // -unlock also wakes waiters of -lockWhenCondition:.
            support.method(cls, "unlock", [pthread, storage_of, check](CpuState& s, GuestMemory&) {
                const auto storage = storage_of(s.x[0]);
                static_cast<void>(pthread("_pthread_cond_broadcast", {storage + mutex_size}));
                check(pthread("_pthread_mutex_unlock", {storage}), "-[NSConditionLock unlock] (not locked by this thread)");
            }, "v@:");
        }
    }
}

} // namespace alchemy::gui::completion_kit
