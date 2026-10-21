#include "alchemy/darwin/runloop/runloop_internal.hpp"

#include "alchemy/darwin_threads.hpp"
#include "alchemy/engine_adapter.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <stdexcept>

// NSRunLoop and the NSObject perform requests (owner: A2), replacing the
// headless NSRunLoop of foundation_extra.cpp. Behaviour follows Apple's
// NSRunLoop and NSObject reference documentation.
namespace alchemy::gui::detail {
namespace {
// Completion of a performSelectorOnMainThread:...waitUntilDone:YES request.
struct Completion {
    std::mutex mutex;
    std::condition_variable changed;
    bool done{};
    void finish() { { std::lock_guard lock(mutex); done = true; } changed.notify_all(); }
};
} // namespace

void Provider::install_foundation() {
    ns_run_loop_class = objc.find_class("NSRunLoop");
    if (!ns_run_loop_class) ns_run_loop_class = define_class("NSRunLoop", foundation_library);
    install_timers();
    const auto r = ns_run_loop_class;
    method(r, "currentRunLoop", [this](auto& state, auto&) { state.x[0] = ns_object(current()); }, true);
    method(r, "mainRunLoop", [this](auto& state, auto&) { state.x[0] = ns_object(main); }, true);
    method(r, "getCFRunLoop", [this](auto& state, auto&) { state.x[0] = cf_object(loop_of(state.x[0])); }, false, "^{__CFRunLoop=}@:");
    method(r, "currentMode", [this](auto& state, auto&) {
        const auto mode = loop_of(state.x[0])->current_mode();
        state.x[0] = mode.empty() ? 0 : mode_object(mode);
    });
    // -runMode:beforeDate: runs the loop once: it returns after the first
    // source is handled or at the limit date, and NO when the mode has no
    // sources or timers.
    const auto run_mode = [this](std::uint64_t self, const std::string& mode, std::uint64_t limit) {
        const auto loop = loop_of(self);
        const auto timeout = monotonic_from_date(limit) - monotonic_seconds();
        return loop->run_once(mode, std::isinf(timeout) ? distant_future : std::max(0.0, timeout), true);
    };
    method(r, "runMode:beforeDate:", [this, run_mode](auto& state, auto&) {
        state.x[0] = run_mode(state.x[0], mode_name(state.x[2]), state.x[3]) != RunResult::finished;
    }, false, "B@:@@");
    method(r, "acceptInputForMode:beforeDate:", [this, run_mode](auto& state, auto&) {
        static_cast<void>(run_mode(state.x[0], mode_name(state.x[2]), state.x[3]));
    }, false, "v@:@@");
    // -run and -runUntilDate: repeat -runMode:beforeDate: in the default mode
    // until the date passes or the mode has nothing left to run.
    const auto run_until = [this](std::uint64_t self, double until) {
        const auto loop = loop_of(self);
        for (;;) {
            const auto now = monotonic_seconds();
            if (now >= until) return;
            if (loop->run_once(default_mode, std::isinf(until) ? distant_future : until - now, true) == RunResult::finished) return;
        }
    };
    method(r, "run", [run_until](auto& state, auto&) { run_until(state.x[0], distant_future); }, false, "v@:");
    method(r, "runUntilDate:", [this, run_until](auto& state, auto&) {
        run_until(state.x[0], monotonic_from_date(state.x[2]));
    }, false, "v@:@");
    method(r, "limitDateForMode:", [this](auto& state, auto&) {
        // One non-blocking pass (due timers fire), then the next fire date;
        // nil when the mode has no sources or timers.
        const auto loop = loop_of(state.x[0]);
        const auto mode = mode_name(state.x[2]);
        if (loop->run_once(mode, 0, true) == RunResult::finished) { state.x[0] = 0; return; }
        const auto next = loop->next_timer_fire(mode);
        state.x[0] = date(absolute_from_monotonic(next));
    });
    method(r, "addTimer:forMode:", [this](auto& state, auto&) {
        schedule_timer(state.x[2], loop_of(state.x[0]), mode_name(state.x[3]));
    }, false, "v@:@@");
    const auto perform_block = [this](const std::shared_ptr<RunLoopImpl>& loop, ModeSet modes, std::uint64_t block) {
        if (!block) throw std::invalid_argument("NSRunLoop perform block is nil");
        const auto copy = block_copy(block);
        const std::weak_ptr<Provider> weak = weak_from_this();
        loop->perform(std::move(modes), [weak, copy] {
            const auto self = weak.lock();
            if (!self) return;
            struct Release { Provider& self; std::uint64_t block; ~Release() { try { self.block_release(block); } catch (...) {} } } owned{*self, copy};
            static_cast<void>(self->call_address(self->process.calling_engine().read64(copy + 16), {copy}));
        }, false);
    };
    method(r, "performBlock:", [this, perform_block](auto& state, auto&) {
        perform_block(loop_of(state.x[0]), ModeSet{std::string(common_modes)}, state.x[2]);
    }, false, "v@:@?");
    method(r, "performInModes:block:", [this, perform_block](auto& state, auto&) {
        perform_block(loop_of(state.x[0]), mode_names(state.x[2]), state.x[3]);
    }, false, "v@:@@?");

    // ---- NSObject perform requests ---------------------------------------------
    const auto root = objc.find_class("NSObject");
    if (!root) throw std::runtime_error("Alchemy run loop requires NSObject");
    // [target sel argument...] through objc_msgSend's lookup.
    const auto send = [this](std::uint64_t target, std::uint64_t selector, std::uint64_t first, std::uint64_t second) {
        if (!selector) throw std::invalid_argument("performSelector: with a NULL selector");
        CallRegs registers;
        registers.x[2] = first; registers.x[3] = second;
        return objc.send(target, selector, std::move(registers)).x0;
    };
    // Class objects answer NSObject's instance methods too (the root
    // metaclass inherits from the root class), so each perform method is
    // registered on both sides.
    for (const bool class_method : {false, true}) {
        method(root, "performSelector:", [send](auto& state, auto&) { state.x[0] = send(state.x[0], state.x[2], 0, 0); },
               class_method, "@@::");
        method(root, "performSelector:withObject:", [send](auto& state, auto&) {
            state.x[0] = send(state.x[0], state.x[2], state.x[3], 0);
        }, class_method, "@@::@");
        method(root, "performSelector:withObject:withObject:", [send](auto& state, auto&) {
            state.x[0] = send(state.x[0], state.x[2], state.x[3], state.x[4]);
        }, class_method, "@@::@@");
    }

    // -performSelector:withObject:afterDelay:[inModes:] schedules a one-shot
    // timer on the calling thread's run loop; target and argument are
    // retained until it runs or is cancelled.
    const auto after_delay = [this, send](std::uint64_t target, std::uint64_t selector, std::uint64_t argument, double delay,
                                          ModeSet modes) {
        if (!selector) throw std::invalid_argument("performSelector:afterDelay: with a NULL selector");
        if (modes.empty()) return; // No modes: the request is never performed.
        const auto loop = current();
        static_cast<void>(retain(target));
        static_cast<void>(retain(argument));
        std::uint64_t key;
        { std::lock_guard lock(mutex); key = next_delayed++; delayed.emplace(key, DelayedPerform{target, selector, argument, loop, 0}); }
        const std::weak_ptr<Provider> weak = weak_from_this();
        const auto id = loop->add_timer(monotonic_seconds() + std::max(0.0, delay), 0, *modes.begin(), [weak, key, send](TimerId) {
            const auto self = weak.lock();
            if (!self) return;
            DelayedPerform request;
            {
                std::lock_guard lock(self->mutex);
                const auto found = self->delayed.find(key);
                if (found == self->delayed.end()) return;
                request = std::move(found->second);
                self->delayed.erase(found);
            }
            struct Release {
                Provider& self; const DelayedPerform& request;
                ~Release() { try { self.release(request.target); self.release(request.argument); } catch (...) {} }
            } owned{*self, request};
            static_cast<void>(send(request.target, request.selector, request.argument, 0));
        });
        for (const auto& mode : modes) loop->add_timer_mode(id, mode);
        std::lock_guard lock(mutex);
        if (const auto found = delayed.find(key); found != delayed.end()) found->second.timer = id;
    };
    method(root, "performSelector:withObject:afterDelay:", [after_delay](auto& state, auto&) {
        after_delay(state.x[0], state.x[2], state.x[3], d_argument(state, 0), ModeSet{std::string(default_mode)});
    }, false, "v@::@d");
    method(root, "performSelector:withObject:afterDelay:inModes:", [this, after_delay](auto& state, auto&) {
        after_delay(state.x[0], state.x[2], state.x[3], d_argument(state, 0), mode_names(state.x[4]));
    }, false, "v@::@d@");
    // +cancelPreviousPerformRequestsWithTarget:[selector:object:] cancels the
    // calling thread's requests; arguments match with -isEqual:.
    const auto cancel = [this](std::uint64_t target, bool any, std::uint64_t selector, std::uint64_t argument) {
        const auto loop = current();
        std::vector<DelayedPerform> cancelled;
        std::vector<std::pair<std::uint64_t, DelayedPerform>> candidates;
        {
            std::lock_guard lock(mutex);
            for (const auto& [key, request] : delayed)
                if (request.loop == loop && request.target == target && (any || request.selector == selector)) candidates.emplace_back(key, request);
        }
        for (auto& [key, request] : candidates) {
            if (!any && request.argument != argument) {
                if (!request.argument || !argument) continue;
                CallRegs registers;
                registers.x[2] = argument;
                if ((objc.send(request.argument, "isEqual:", std::move(registers)).x0 & 0xff) == 0) continue;
            }
            std::lock_guard lock(mutex);
            if (delayed.erase(key)) cancelled.push_back(std::move(request));
        }
        for (const auto& request : cancelled) {
            request.loop->cancel_timer(request.timer);
            release(request.target); release(request.argument);
        }
    };
    method(root, "cancelPreviousPerformRequestsWithTarget:", [cancel](auto& state, auto&) {
        cancel(state.x[2], true, 0, 0);
    }, true, "v@:@");
    method(root, "cancelPreviousPerformRequestsWithTarget:selector:object:", [cancel](auto& state, auto&) {
        cancel(state.x[2], false, state.x[3], state.x[4]);
    }, true, "v@:@:@");

    // -[NSRunLoop performSelector:target:argument:order:modes:] sends the
    // message at the receiver's next iteration in one of the modes (target and
    // argument retained until then); requests run in the order they were
    // made (the order value is not used to reorder them).
    // -cancelPerformSelector:target:argument: (arguments match with -isEqual:)
    // and -cancelPerformSelectorsWithTarget: cancel the receiver's pending ones.
    method(r, "performSelector:target:argument:order:modes:", [this, send](auto& state, auto&) {
        if (!state.x[2]) throw std::invalid_argument("-[NSRunLoop performSelector:target:argument:order:modes:] with a NULL selector");
        auto modes = mode_names(state.x[6]);
        if (modes.empty()) return;
        const auto loop = loop_of(state.x[0]);
        const auto target = state.x[3], selector = state.x[2], argument = state.x[4];
        static_cast<void>(retain(target));
        static_cast<void>(retain(argument));
        std::uint64_t key;
        { std::lock_guard lock(mutex); key = next_delayed++; loop_performs.emplace(key, DelayedPerform{target, selector, argument, loop, 0}); }
        const std::weak_ptr<Provider> weak = weak_from_this();
        loop->perform(std::move(modes), [weak, key, send] {
            const auto self = weak.lock();
            if (!self) return;
            DelayedPerform request;
            {
                std::lock_guard lock(self->mutex);
                const auto found = self->loop_performs.find(key);
                if (found == self->loop_performs.end()) return;   // cancelled
                request = std::move(found->second);
                self->loop_performs.erase(found);
            }
            struct Release {
                Provider& self; const DelayedPerform& request;
                ~Release() { try { self.release(request.target); self.release(request.argument); } catch (...) {} }
            } owned{*self, request};
            static_cast<void>(send(request.target, request.selector, request.argument, 0));
        }, false);
    }, false, "v@::@@Q@");
    const auto cancel_loop_performs = [this](std::uint64_t self, std::uint64_t target, bool any, std::uint64_t selector, std::uint64_t argument) {
        const auto loop = loop_of(self);
        std::vector<std::pair<std::uint64_t, DelayedPerform>> candidates;
        {
            std::lock_guard lock(mutex);
            for (const auto& [key, request] : loop_performs)
                if (request.loop == loop && request.target == target && (any || request.selector == selector)) candidates.emplace_back(key, request);
        }
        std::vector<DelayedPerform> cancelled;
        for (auto& [key, request] : candidates) {
            if (!any && request.argument != argument) {
                if (!request.argument || !argument) continue;
                CallRegs registers;
                registers.x[2] = argument;
                if ((objc.send(request.argument, "isEqual:", std::move(registers)).x0 & 0xff) == 0) continue;
            }
            std::lock_guard lock(mutex);
            if (loop_performs.erase(key)) cancelled.push_back(std::move(request));
        }
        for (const auto& request : cancelled) { release(request.target); release(request.argument); }
    };
    method(r, "cancelPerformSelector:target:argument:", [cancel_loop_performs](auto& state, auto&) {
        cancel_loop_performs(state.x[0], state.x[3], false, state.x[2], state.x[4]);
    }, false, "v@::@@");
    method(r, "cancelPerformSelectorsWithTarget:", [cancel_loop_performs](auto& state, auto&) {
        cancel_loop_performs(state.x[0], state.x[2], true, 0, 0);
    }, false, "v@:@");

    // -performSelectorOnMainThread:withObject:waitUntilDone:[modes:] queues a
    // source-0 request on the main run loop (common modes by default); with
    // wait on the main thread it runs at once.
    const auto on_main = [this, send](std::uint64_t target, std::uint64_t selector, std::uint64_t argument, bool wait, ModeSet modes) {
        if (!selector) throw std::invalid_argument("performSelectorOnMainThread: with a NULL selector");
        const bool main_thread = &process.calling_engine() == &process.main_engine();
        if (wait && main_thread) { static_cast<void>(send(target, selector, argument, 0)); return; }
        if (modes.empty()) return;
        static_cast<void>(retain(target));
        static_cast<void>(retain(argument));
        const auto completion = std::make_shared<Completion>();
        const std::weak_ptr<Provider> weak = weak_from_this();
        main->perform(std::move(modes), [weak, send, target, selector, argument, completion] {
            struct Finish {
                std::weak_ptr<Provider> weak; std::uint64_t target, argument; std::shared_ptr<Completion> completion;
                ~Finish() {
                    if (const auto self = weak.lock()) { try { self->release(target); self->release(argument); } catch (...) {} }
                    completion->finish();
                }
            } finish{weak, target, argument, completion};
            static_cast<void>(send(target, selector, argument, 0));
        }, true);
        if (!wait) return;
        std::unique_lock lock(completion->mutex);
        while (!completion->done) {
            completion->changed.wait_for(lock, std::chrono::milliseconds(10));
            if (process.requested_exit()) {
                lock.unlock();
                if (guest_worker_must_stop(process)) throw GuestThreadStop{};
                EngineAdapter::require("cooperative guest stop during performSelectorOnMainThread:");
            }
        }
    };
    method(root, "performSelectorOnMainThread:withObject:waitUntilDone:", [on_main](auto& state, auto&) {
        on_main(state.x[0], state.x[2], state.x[3], state.x[4] & 0xff, ModeSet{std::string(common_modes)});
    }, false, "v@::@B");
    method(root, "performSelectorOnMainThread:withObject:waitUntilDone:modes:", [this, on_main](auto& state, auto&) {
        on_main(state.x[0], state.x[2], state.x[3], state.x[4] & 0xff, mode_names(state.x[5]));
    }, false, "v@::@B@");
}

} // namespace alchemy::gui::detail
