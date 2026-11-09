#include "alchemy/darwin/kit/support/support_kit.hpp"

#include "alchemy/host/process.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <map>
#include <mutex>

// Foundation: NSFileTypeForHFSTypeCode, NSThread (one NSThread object per
// running host thread, and threads made with +alloc/-init... that -start runs
// on a libSystem pthread), +[NSBundle bundleForClass:], and the
// NSMutableString and NSUserNotification(Center) class shells. NSBundle's
// localizations are foundation_extra.cpp's, -executableURL value_classes.cpp's.
// Qt 6.5's bind-time surface first needed them (moved from
// darwin/identities/qt_imports.cpp); installed by the support kit
// (kit/support/install.cpp) at the old position, only when missing.
namespace alchemy::gui::support_kit {

namespace {

constexpr std::string_view foundation = "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation";
constexpr std::string_view libsystem = "/usr/lib/libSystem.B.dylib";
constexpr std::string_view libobjc = "/usr/lib/libobjc.A.dylib";

} // namespace

void install_foundation_threads_and_localizations(Support& support) {
    auto* s = &support;
    // ---- Foundation functions (real) ----
    support.fill_function(foundation, "_NSFileTypeForHFSTypeCode", [s](CpuState& c, GuestMemory&) {
        const auto code = static_cast<std::uint32_t>(c.x[0]);
        std::string text = "'";
        for (int shift = 24; shift >= 0; shift -= 8) text.push_back(static_cast<char>((code >> shift) & 0xff));
        text += "'";
        c.x[0] = s->send(s->cf.string(text), "autorelease").x0;
    });

    // ---- NSThread ----
    if (!support.process.resolve(foundation, "_OBJC_CLASS_$_NSThread")) {
        const auto thread = support.declare_class("NSThread", "NSObject", foundation);
        const auto is_main = [s](CpuState& c, GuestMemory&) { c.x[0] = s->on_main_thread() ? 1 : 0; };
        support.method(thread, "isMainThread", is_main, "B@:", true);
        support.method(thread, "isMultiThreaded", [](CpuState& c, GuestMemory&) { c.x[0] = 1; }, "B@:", true);
        // One NSThread object per host thread (immortal: threads outlive autorelease pools), and the threads made
        // with +alloc/-init... (their work: a target and selector with an argument, or a block; -main runs it).
        // phase: 0 not started, 1 executing, 2 finished.
        struct ThreadObject final : ObjectPayload {
            bool main{}; Address dictionary{}, name{};
            Address target{}, selector{}, argument{}, block{};
            std::uint64_t stack_size{};
            double priority{0.5};
            std::int64_t quality_of_service{-1};   // NSQualityOfServiceDefault
            std::atomic<int> phase{0};
            std::atomic<bool> cancelled{};
        };
        auto objects = std::make_shared<std::pair<std::mutex, std::map<unsigned long, Address>>>();
        const auto current = [s, thread, objects](bool main_only) -> Address {
            const auto id = main_only ? s->main_thread_id : static_cast<unsigned long>(host::process::current_thread_id());
            std::lock_guard lock(objects->first);
            auto& slot = objects->second[id];
            if (!slot) {
                auto payload = std::make_shared<ThreadObject>();
                payload->main = id == s->main_thread_id;
                payload->phase = 1;   // already running
                slot = s->process.objects().create(16, thread, ObjectKind::instance, payload, {}, true);
            }
            return slot;
        };
        support.method(thread, "currentThread", [current](CpuState& c, GuestMemory&) { c.x[0] = current(false); }, "@@:", true);
        support.method(thread, "mainThread", [current](CpuState& c, GuestMemory&) { c.x[0] = current(true); }, "@@:", true);
        support.method(thread, "isMainThread", [s](CpuState& c, GuestMemory&) {
            const auto state = s->objc.find_host_state<ThreadObject>(c.x[0]);
            c.x[0] = state && state->main ? 1 : 0;
        }, "B@:");
        support.method(thread, "threadDictionary", [s](CpuState& c, GuestMemory&) {
            const auto state = s->objc.find_host_state<ThreadObject>(c.x[0]);
            if (!state) fail("-[NSThread threadDictionary] on an NSThread that is not a running thread");
            if (!state->dictionary) state->dictionary = s->cf.dictionary(std::span<const std::pair<std::uint64_t, std::uint64_t>>{}, true);
            c.x[0] = state->dictionary;
        }, "@@:");
        support.method(thread, "name", [s](CpuState& c, GuestMemory&) {
            const auto state = s->objc.find_host_state<ThreadObject>(c.x[0]);
            c.x[0] = state ? state->name : 0;
        }, "@@:");
        support.method(thread, "setName:", [s](CpuState& c, GuestMemory&) {
            const auto state = s->objc.find_host_state<ThreadObject>(c.x[0]);
            if (!state) return;
            const auto old = std::exchange(state->name, c.x[2] ? s->send(c.x[2], "copy").x0 : 0);
            s->release(old);
        }, "v@:@");

        // ---- threads made with +alloc / -init... and run by -start ----
        const auto made = [s](Address object) { return s->objc.host_state<ThreadObject>(object); };
        support.method(thread, "init", [made](CpuState& c, GuestMemory&) { static_cast<void>(made(c.x[0])); }, "@@:");
        support.method(thread, "initWithTarget:selector:object:", [s, made](CpuState& c, GuestMemory&) {
            if (!c.x[2] || !c.x[3]) fail("NSInvalidArgumentException: -[NSThread initWithTarget:selector:object:] needs a target and a selector");
            const auto state = made(c.x[0]);
            state->target = s->send(c.x[2], "retain").x0;
            state->selector = c.x[3];
            state->argument = c.x[4] ? s->send(c.x[4], "retain").x0 : 0;
        }, "@@:@:@");
        support.method(thread, "initWithBlock:", [s, made](CpuState& c, GuestMemory&) {
            if (!c.x[2]) fail("NSInvalidArgumentException: -[NSThread initWithBlock:] needs a block");
            made(c.x[0])->block = s->call(libsystem, "__Block_copy", {c.x[2]});
        }, "@@:@?");
        // -main: the thread's work (a subclass overrides it).
        support.method(thread, "main", [s](CpuState& c, GuestMemory&) {
            const auto state = s->objc.find_host_state<ThreadObject>(c.x[0]);
            if (!state) return;
            if (state->block) {
                const std::array<Address, 1> arguments{state->block};
                static_cast<void>(s->process.calling_engine().invoke_callback(s->memory().read64(state->block + 16), arguments));
            } else if (state->target) {
                CallRegs registers;
                registers.x[2] = state->argument;
                static_cast<void>(s->objc.send(state->target, state->selector, registers));
            }
        }, "v@:");
        // The started thread: +currentThread there is the object; -main runs inside an autorelease pool, then
        // NSThreadWillExitNotification goes out and the object's reference (taken by -start) is dropped.
        support.string_constant(foundation, "_NSThreadWillExitNotification", "NSThreadWillExitNotification");
        const auto entry = support.process.install_function("___alchemy_nsthread_start", [s, objects](CpuState& c, GuestMemory&) {
            const auto self = c.x[0];
            const auto state = s->objc.find_host_state<ThreadObject>(self);
            const auto id = static_cast<unsigned long>(host::process::current_thread_id());
            { std::lock_guard lock(objects->first); objects->second[id] = self; }
            const auto pool = s->call(libobjc, "_objc_autoreleasePoolPush", {});
            static_cast<void>(s->send(self, "main"));
            static_cast<void>(s->call(libobjc, "_objc_autoreleasePoolPop", {pool}));
            const auto center = s->send_id(s->objc.find_class("NSNotificationCenter"), "defaultCenter");
            const auto name = s->cf.string("NSThreadWillExitNotification");
            static_cast<void>(s->send_id(center, "postNotificationName:object:", {name, self}));
            s->release(name);
            if (state) state->phase = 2;
            { std::lock_guard lock(objects->first); objects->second.erase(id); }
            static_cast<void>(s->send(self, "release"));
            c.x[0] = 0;
        });
        // -start: a detached pthread (with the requested stack size) runs the thread; starting twice raises.
        support.method(thread, "start", [s, entry](CpuState& c, GuestMemory&) {
            const auto state = s->objc.find_host_state<ThreadObject>(c.x[0]);
            int idle = 0;
            if (!state || !state->phase.compare_exchange_strong(idle, 1))
                fail("NSInvalidArgumentException: -[NSThread start]: attempt to start the thread again");
            static_cast<void>(s->send(c.x[0], "retain"));
            const auto attributes = s->process.allocate_guest(64, 16);   // pthread_attr_t
            const auto handle = s->process.allocate_guest(8, 8);
            static_cast<void>(s->call(libsystem, "_pthread_attr_init", {attributes}));
            if (state->stack_size) static_cast<void>(s->call(libsystem, "_pthread_attr_setstacksize", {attributes, state->stack_size}));
            static_cast<void>(s->call(libsystem, "_pthread_attr_setdetachstate", {attributes, 2}));   // PTHREAD_CREATE_DETACHED
            const auto result = static_cast<std::uint32_t>(s->call(libsystem, "_pthread_create", {handle, attributes, entry, c.x[0]}));
            static_cast<void>(s->call(libsystem, "_pthread_attr_destroy", {attributes}));
            s->process.release_guest(attributes);
            s->process.release_guest(handle);
            if (result) {
                state->phase = 0;
                static_cast<void>(s->send(c.x[0], "release"));
                fail("NSThread: pthread_create failed (" + std::to_string(result) + ")");
            }
        }, "v@:");
        const auto query = [&](const char* selector, auto answer) {
            support.method(thread, selector, [s, answer](CpuState& c, GuestMemory&) {
                const auto state = s->objc.find_host_state<ThreadObject>(c.x[0]);
                c.x[0] = state && answer(*state) ? 1 : 0;
            }, "B@:");
        };
        query("isExecuting", [](const ThreadObject& t) { return t.phase == 1; });
        query("isFinished", [](const ThreadObject& t) { return t.phase == 2; });
        query("isCancelled", [](const ThreadObject& t) { return t.cancelled.load(); });
        support.method(thread, "cancel", [s](CpuState& c, GuestMemory&) {
            if (const auto state = s->objc.find_host_state<ThreadObject>(c.x[0])) state->cancelled = true;
        }, "v@:");
        // Settings: the stack size applies at -start; priority and quality of service are recorded (the host
        // scheduler decides).
        support.method(thread, "stackSize", [s](CpuState& c, GuestMemory&) {
            const auto state = s->objc.find_host_state<ThreadObject>(c.x[0]);
            c.x[0] = state ? state->stack_size : 0;
        }, "Q@:");
        support.method(thread, "setStackSize:", [s](CpuState& c, GuestMemory&) {
            if (const auto state = s->objc.find_host_state<ThreadObject>(c.x[0])) state->stack_size = c.x[2];
        }, "v@:Q");
        support.method(thread, "threadPriority", [s](CpuState& c, GuestMemory&) {
            const auto state = s->objc.find_host_state<ThreadObject>(c.x[0]);
            c.v[0] = {std::bit_cast<std::uint64_t>(state ? state->priority : 0.5), 0};
        }, "d@:");
        support.method(thread, "setThreadPriority:", [s](CpuState& c, GuestMemory&) {
            if (const auto state = s->objc.find_host_state<ThreadObject>(c.x[0])) state->priority = std::bit_cast<double>(c.v[0][0]);
        }, "v@:d");
        support.method(thread, "qualityOfService", [s](CpuState& c, GuestMemory&) {
            const auto state = s->objc.find_host_state<ThreadObject>(c.x[0]);
            c.x[0] = static_cast<std::uint64_t>(state ? state->quality_of_service : -1);
        }, "q@:");
        support.method(thread, "setQualityOfService:", [s](CpuState& c, GuestMemory&) {
            if (const auto state = s->objc.find_host_state<ThreadObject>(c.x[0])) state->quality_of_service = static_cast<std::int64_t>(c.x[2]);
        }, "v@:q");
        support.method(thread, "dealloc", [s, thread](CpuState& c, GuestMemory&) {
            if (const auto state = s->objc.find_host_state<ThreadObject>(c.x[0])) {
                for (const auto object : {state->target, state->argument}) if (object) static_cast<void>(s->send(object, "release"));
                if (state->block) static_cast<void>(s->call(libsystem, "__Block_release", {state->block}));
                s->release(state->name);
                s->release(state->dictionary);
            }
            static_cast<void>(s->objc.send_super(c.x[0], thread, s->objc.selector("dealloc")));
        }, "v@:");
    }

    // ---- NSBundle (localizations are foundation_extra.cpp's; -executableURL is value_classes.cpp's) ----
    if (const auto bundle = support.objc.find_class("NSBundle")) {
        // +bundleForClass: answers the main bundle (images are not bundles of
        // their own here; Qt only reads identifiers and paths from it).
        if (!support.objc.responds(bundle, support.objc.selector("bundleForClass:")))
            support.method(bundle, "bundleForClass:", [s, bundle](CpuState& c, GuestMemory&) {
                c.x[0] = s->send(bundle, "mainBundle").x0;
            }, "@@:#", true);
    }

    const auto shell = [&](std::string_view library, const char* name, const char* superclass) {
        if (support.process.resolve(library, "_OBJC_CLASS_$_" + std::string(name))) return;
        if (!support.objc.find_class(superclass)) return;
        static_cast<void>(support.declare_class(name, superclass, library));
    };
    shell(foundation, "NSMutableString", "NSString");
    shell(foundation, "NSUserNotification", "NSObject");
    shell(foundation, "NSUserNotificationCenter", "NSObject");
}

} // namespace alchemy::gui::support_kit
