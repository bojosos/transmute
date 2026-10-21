// Run loop, CF/NS run loop providers, completion thread and dispatch gap
// tests (owner: A2). They run on the reference engine: synthetic guest calls
// are host handlers, guest pthreads are reference-engine workers.
#include "runloop_internal.hpp"

#include "alchemy/darwin_dispatch.hpp"
#include "alchemy/darwin_frameworks.hpp"
#include "alchemy/darwin_services.hpp"
#include "alchemy/darwin_threads.hpp"
#include "alchemy/foundation_bridge.hpp"
#include "alchemy/gui/install.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <iostream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using Address = std::uint64_t;
using alchemy::gui::RunResult;
namespace activity = alchemy::gui::activity;
constexpr std::string_view libsystem = "/usr/lib/libSystem.B.dylib";
constexpr auto forever = UINT64_MAX;

void require(bool value, std::string_view message) {
    if (!value) throw std::runtime_error(std::string(message));
}

// Polls `done` for up to five seconds.
template <class Predicate>
void eventually(Predicate done, std::string_view message) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done()) {
        require(std::chrono::steady_clock::now() < until, message);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

struct Fixture {
    transmute::ReferenceEngine engine{transmute::ExecutionStart{0}};
    alchemy::ProcessContext process{engine, [](unsigned, std::span<const std::uint8_t> bytes) {
        return alchemy::OutputResult{bytes.size(), 0}; }};
    alchemy::DarwinServices services{process};
    alchemy::DarwinThreads threads{(alchemy::install_darwin_dispatch(process), process)};
    alchemy::ObjcRuntime objc{process};
    alchemy::CoreFoundation cf{process};
    alchemy::FoundationBridge foundation{process, objc, cf};
    std::mutex gate;

    Fixture() {
        alchemy::install_darwin_frameworks(process, objc, cf);
        alchemy::gui::InstallContext context{process, objc, cf};
        alchemy::gui::install_runloop(context);
    }
    ~Fixture() {
        try { process.request_exit(0); threads.join_workers(); } catch (...) {}
    }
    Address symbol(std::string_view library, std::string_view name) {
        const auto found = process.resolve(library, name);
        require(found.has_value(), "missing symbol " + std::string(name));
        return found->address;
    }
    // A guest call from whichever engine runs the calling thread.
    Address call(std::string_view library, std::string_view name, std::initializer_list<Address> arguments = {}) {
        return process.calling_engine().invoke_callback(symbol(library, name), std::span(arguments.begin(), arguments.size()));
    }
    Address system(std::string_view name, std::initializer_list<Address> arguments = {}) { return call(libsystem, name, arguments); }
    Address core(std::string_view name, std::initializer_list<Address> arguments = {}) {
        return call(alchemy::core_foundation_library, name, arguments);
    }
    // A guest call with a double in d0: the reference engine keeps vector
    // registers across invoke_callback, fx takes a full register image.
    template <class Engine>
    static Address call_d0(Engine& engine, Address function, const std::vector<Address>& arguments, double d0) {
        if constexpr (requires { engine.set_initial_vector(0, {}); }) {
            engine.set_initial_vector(0, {std::bit_cast<std::uint64_t>(d0), 0});
            return engine.invoke_callback(function, arguments);
        } else {
            alchemy::CallRegs registers;
            std::copy(arguments.begin(), arguments.end(), registers.x.begin());
            registers.set_d(0, d0);
            return engine.invoke_callback_regs(function, registers).x0;
        }
    }
    Address call_d0(std::string_view library, std::string_view name, const std::vector<Address>& arguments, double d0) {
        return call_d0(process.calling_engine(), symbol(library, name), arguments, d0);
    }
    // objc_msgSend with an optional double in d0.
    Address send(Address receiver, std::string_view selector, std::initializer_list<Address> arguments = {}, double d0 = 0) {
        std::vector<Address> values{receiver, objc.selector(selector)};
        values.insert(values.end(), arguments.begin(), arguments.end());
        return call_d0(alchemy::objc_library, "_objc_msgSend", values, d0);
    }
    Address stub(std::string name, transmute::GuestCallHandler handler) {
        return process.install_function(std::move(name), std::move(handler), "alchemy.test");
    }
    // A compiler-emitted (global) block whose invoke is `invoke`.
    Address block(Address invoke) {
        const auto value = process.allocate_guest(32), descriptor = process.allocate_guest(16);
        engine.write64(descriptor, 0); engine.write64(descriptor + 8, 32);
        engine.write64(value, symbol(libsystem, "__NSConcreteGlobalBlock"));
        engine.write32(value + 8, 1U << 28); engine.write32(value + 12, 0);
        engine.write64(value + 16, invoke); engine.write64(value + 24, descriptor);
        return value;
    }
    Address string(std::string_view text) {
        const auto address = process.allocate_guest(text.size() + 1);
        engine.write_bytes(address, {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
        engine.fill_bytes(address + text.size(), 0, 1);
        return address;
    }
    Address mode(std::string_view name) { return engine.read64(symbol(alchemy::core_foundation_library, "_" + std::string(name))); }
    std::uint64_t identity() { return process.current_thread()->identity; }
    std::uint64_t main_identity() { return process.thread(engine)->identity; }
};

double since(double start) { return alchemy::gui::monotonic_seconds() - start; }

void test_timers_and_modes(Fixture& f) {
    auto& loop = alchemy::gui::main_run_loop();
    std::vector<std::string> fired;
    const auto record = [&](std::string name) { return [&fired, name](alchemy::gui::TimerId) { fired.push_back(name); }; };
    const auto now = alchemy::gui::monotonic_seconds();
    static_cast<void>(loop.add_timer(now + 0.03, 0, alchemy::gui::default_mode, record("a")));
    static_cast<void>(loop.add_timer(now + 0.01, 0, alchemy::gui::default_mode, record("b")));
    static_cast<void>(loop.add_timer(now + 0.02, 0, alchemy::gui::default_mode, record("c")));
    static_cast<void>(loop.add_timer(now + 0.01, 0, alchemy::gui::default_mode, record("b2")));
    static_cast<void>(loop.add_timer(now + 0.005, 0, "test.custom", record("custom")));
    static_cast<void>(loop.add_timer(now + 0.015, 0, alchemy::gui::common_modes, record("common")));
    require(loop.run_once(alchemy::gui::default_mode, 0.1, false) == RunResult::timed_out, "timed run did not time out");
    require((fired == std::vector<std::string>{"b", "b2", "common", "c", "a"}), "timers fired out of order or in the wrong mode");
    // The mode's only timer fires once; the emptied mode then finishes.
    require(loop.run_once("test.custom", 1, false) == RunResult::finished && fired.back() == "custom", "custom mode timer");
    // Common-mode items run in every common mode, including event tracking.
    static_cast<void>(loop.add_timer(alchemy::gui::monotonic_seconds(), 0, alchemy::gui::common_modes, record("tracking")));
    static_cast<void>(loop.run_once(alchemy::gui::event_tracking_mode, 0, false));
    require(fired.back() == "tracking", "common mode timer in event tracking");
    // Repeating timer: the callback stops the loop on the third fire.
    int repeats = 0;
    alchemy::gui::TimerId repeating{};
    repeating = loop.add_timer(alchemy::gui::monotonic_seconds() + 0.005, 0.005, alchemy::gui::default_mode, [&](auto id) {
        require(id == repeating, "timer callback id");
        if (++repeats == 3) { loop.cancel_timer(id); loop.stop(); }
    });
    const auto start = alchemy::gui::monotonic_seconds();
    require(loop.run_once(alchemy::gui::default_mode, 5, false) == RunResult::stopped && repeats == 3 && since(start) < 2,
            "repeating timer / stop");
    require(std::isinf(loop.next_timer_fire(alchemy::gui::default_mode)), "cancelled timer still pending");
    // Observers: entry and exit per run, mode-filtered; one-shot observers go away.
    std::vector<std::uint32_t> seen;
    const auto observer = loop.add_observer(activity::entry | activity::exit, alchemy::gui::default_mode, true,
                                            [&](std::uint32_t value) { seen.push_back(value); });
    std::vector<std::uint32_t> once;
    static_cast<void>(loop.add_observer(activity::all, alchemy::gui::default_mode, false, [&](std::uint32_t value) { once.push_back(value); }));
    static_cast<void>(loop.run_once(alchemy::gui::default_mode, 0, false));
    static_cast<void>(loop.run_once("test.custom", 0, false));
    static_cast<void>(loop.run_once(alchemy::gui::default_mode, 0, false));
    require((seen == std::vector<std::uint32_t>{activity::entry, activity::exit, activity::entry, activity::exit}), "observer activities");
    require(once.size() == 1 && once[0] == activity::entry, "one-shot observer repeated");
    loop.remove_observer(observer);
    // A non-main loop with nothing in the mode finishes at once.
    alchemy::gui::detail::RunLoopImpl other(f.process, false);
    require(other.run_once(alchemy::gui::default_mode, 5, false) == RunResult::finished, "empty mode did not finish");
    require(loop.current_mode().empty(), "current mode outside a run");
}

void test_wake_and_sources(Fixture& f) {
    static_cast<void>(f);
    auto& loop = alchemy::gui::main_run_loop();
    std::atomic<bool> performed{};
    const auto source = loop.add_source(alchemy::gui::default_mode, [&] { performed = true; });
    {
        std::thread signaller([&] { std::this_thread::sleep_for(std::chrono::milliseconds(50)); loop.signal_source(source); });
        const auto start = alchemy::gui::monotonic_seconds();
        const auto result = loop.run_once(alchemy::gui::default_mode, 5, true);
        signaller.join();
        require(result == RunResult::handled_source && performed && since(start) < 2, "source signalled from another thread");
    }
    loop.remove_source(source);
    {
        std::thread stopper([&] { std::this_thread::sleep_for(std::chrono::milliseconds(50)); loop.stop(); });
        const auto start = alchemy::gui::monotonic_seconds();
        const auto result = loop.run_once(alchemy::gui::default_mode, 5, false);
        stopper.join();
        require(result == RunResult::stopped && since(start) < 2, "stop from another thread");
    }
    // Win32 waitable handles are sources in every mode.
    const auto event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::atomic<int> handled{};
    const auto waitable = loop.add_wait_handle(event, [&] { ++handled; });
    {
        std::thread setter([&] { std::this_thread::sleep_for(std::chrono::milliseconds(50)); SetEvent(event); });
        const auto result = loop.run_once("test.custom", 5, true);
        setter.join();
        require(result == RunResult::handled_source && handled == 1, "wait handle source");
    }
    loop.remove_source(waitable);
    CloseHandle(event);
    // The message dispatcher runs when Win32 input arrives.
    MSG message{};
    PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE); // Creates this thread's message queue.
    std::atomic<bool> received{};
    loop.set_message_dispatcher([&] {
        bool any{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { any = true; if (message.message == WM_APP + 7) received = true; }
        return any;
    });
    {
        const auto thread = GetCurrentThreadId();
        std::thread poster([&] { std::this_thread::sleep_for(std::chrono::milliseconds(50)); PostThreadMessageW(thread, WM_APP + 7, 0, 0); });
        const auto start = alchemy::gui::monotonic_seconds();
        const auto result = loop.run_once(alchemy::gui::default_mode, 5, true);
        poster.join();
        require(result == RunResult::handled_source && received && since(start) < 2, "message dispatcher did not run on input");
    }
    loop.set_message_dispatcher({});
    // Blocks: CFRunLoopPerformBlock-style work in matching modes only.
    int blocks = 0;
    static_cast<alchemy::gui::detail::RunLoopImpl&>(loop).perform({std::string(alchemy::gui::event_tracking_mode)}, [&] { ++blocks; }, false);
    static_cast<void>(loop.run_once(alchemy::gui::default_mode, 0, false));
    require(blocks == 0, "block ran in the wrong mode");
    static_cast<void>(loop.run_once(alchemy::gui::event_tracking_mode, 0, false));
    require(blocks == 1, "block did not run");
}

void test_main_queue_wake(Fixture& f) {
    auto& loop = alchemy::gui::main_run_loop();
    const auto main_queue = f.symbol(libsystem, "__dispatch_main_q");
    std::atomic<std::uint64_t> ran{}, thread{};
    const auto work = f.stub("test_main_work", [&](auto& state, auto&) { thread = f.identity(); ran = state.x[0]; state.x[0] = 0; });
    // Queued before the run: drained in common modes only.
    static_cast<void>(f.system("_dispatch_async_f", {main_queue, 11, work}));
    static_cast<void>(loop.run_once("test.custom", 0, false));
    require(ran == 0, "main queue drained in a non-common mode");
    static_cast<void>(loop.run_once(alchemy::gui::default_mode, 0, false));
    require(ran == 11 && thread == f.main_identity(), "main queue not drained by the run loop");
    // Posted from a worker while the main thread waits: the waker ends the wait.
    auto worker = f.engine.fork_thread(0, 0);
    const auto async = f.symbol(libsystem, "_dispatch_async_f");
    std::thread poster([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const std::array arguments{main_queue, Address{22}, work};
        static_cast<void>(worker->invoke_callback(async, arguments));
    });
    const auto start = alchemy::gui::monotonic_seconds();
    const auto result = loop.run_once(alchemy::gui::default_mode, 5, true);
    poster.join();
    require(result == RunResult::handled_source && ran == 22 && thread == f.main_identity() && since(start) < 2,
            "main-queue async did not wake the blocked run loop");
}

void test_completion_thread(Fixture& f) {
    std::vector<std::pair<std::uint64_t, std::vector<Address>>> calls;
    const auto invoke = f.stub("test_completion_invoke", [&](auto& state, auto&) {
        std::lock_guard lock(f.gate);
        calls.push_back({f.identity(), {state.x[0], state.x[1], state.x[2]}});
        state.x[0] = 0;
    });
    const auto block = f.block(invoke);
    auto thread = alchemy::gui::start_completion_thread(f.process, "test.completion");
    std::uint64_t host_thread{};
    std::vector<int> order;
    thread->post([&] { std::lock_guard lock(f.gate); host_thread = f.identity(); order.push_back(1); });
    thread->post_block(block, {7, 9});
    thread->post([&] { std::lock_guard lock(f.gate); order.push_back(2); });
    eventually([&] { std::lock_guard lock(f.gate); return order.size() == 2; }, "completion work did not run");
    thread->shutdown();
    std::lock_guard lock(f.gate);
    require(host_thread && host_thread != f.main_identity(), "completion work ran on the main thread");
    require(calls.size() == 1 && calls[0].first == host_thread, "block did not run on the completion thread");
    require((calls[0].second == std::vector<Address>{block, 7, 9}), "block invoke arguments");
    require((order == std::vector<int>{1, 2}), "completion work out of order");
    bool rejected{};
    try { thread->post([] {}); } catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "post after shutdown accepted");
}

void test_dispatch(Fixture& f) {
    struct Event { std::string what; Address value; std::uint64_t thread; };
    std::vector<Event> events;
    const auto log = [&](std::string what, Address value) { std::lock_guard lock(f.gate); events.push_back({std::move(what), value, f.identity()}); };
    const auto count = [&](std::string_view what) {
        std::lock_guard lock(f.gate);
        return std::count_if(events.begin(), events.end(), [&](const Event& event) { return event.what == what; });
    };
    const auto position = [&](std::string_view what, Address value) {
        std::lock_guard lock(f.gate);
        const auto found = std::find_if(events.begin(), events.end(), [&](const Event& event) { return event.what == what && event.value == value; });
        require(found != events.end(), "missing dispatch event " + std::string(what) + " " + std::to_string(value));
        return found - events.begin();
    };
    const auto slow = f.stub("test_slow", [&](auto& state, auto&) {
        const auto value = state.x[0];
        log("start", value); std::this_thread::sleep_for(std::chrono::milliseconds(20)); log("end", value); state.x[0] = 0;
    });
    const auto mark = f.stub("test_mark", [&](auto& state, auto&) { log("mark", state.x[0]); state.x[0] = 0; });
    const auto concurrent = f.call(libsystem, "_dispatch_queue_create",
        {f.string("test.concurrent"), f.symbol(libsystem, "__dispatch_queue_attr_concurrent")});
    const auto serial = f.system("_dispatch_queue_create", {f.string("test.serial"), 0});
    const auto global = f.system("_dispatch_get_global_queue", {0, 0});

    // Barriers on a private concurrent queue.
    for (Address value : {1, 2, 3}) static_cast<void>(f.system("_dispatch_async_f", {concurrent, value, slow}));
    static_cast<void>(f.system("_dispatch_barrier_async_f", {concurrent, 100, slow}));
    for (Address value : {4, 5}) static_cast<void>(f.system("_dispatch_async_f", {concurrent, value, slow}));
    eventually([&] { return count("end") == 6; }, "concurrent queue work did not finish");
    for (Address value : {1, 2, 3}) require(position("start", 100) > position("end", value), "barrier started before earlier work ended");
    for (Address value : {4, 5}) require(position("start", value) > position("end", 100), "work started before the barrier ended");
    static_cast<void>(f.system("_dispatch_async_f", {concurrent, 6, slow}));
    static_cast<void>(f.system("_dispatch_barrier_sync_f", {concurrent, 200, mark}));
    require(position("mark", 200) > position("end", 6), "barrier_sync ran before earlier work");
    { std::lock_guard lock(f.gate); require(events.back().thread == f.main_identity(), "barrier_sync did not run inline"); }

    // Groups: async, notify, wait, enter/leave.
    const auto group = f.system("_dispatch_group_create");
    for (Address value : {10, 11, 12, 13}) static_cast<void>(f.system("_dispatch_group_async_f", {group, global, value, slow}));
    static_cast<void>(f.system("_dispatch_group_notify_f", {group, serial, 50, mark}));
    require(f.system("_dispatch_group_wait", {group, forever}) == 0, "group wait failed");
    for (Address value : {10, 11, 12, 13}) static_cast<void>(position("end", value));
    eventually([&] { return count("mark") == 2; }, "group notify did not run");
    for (Address value : {10, 11, 12, 13}) require(position("mark", 50) > position("end", value), "group notify ran early");
    static_cast<void>(f.system("_dispatch_group_enter", {group}));
    require(f.system("_dispatch_group_wait", {group, f.system("_dispatch_time", {0, 20'000'000})}) != 0, "group wait ignored its timeout");
    static_cast<void>(f.system("_dispatch_group_leave", {group}));
    require(f.system("_dispatch_group_wait", {group, 0}) == 0, "balanced group still waits");
    bool unbalanced{};
    try { static_cast<void>(f.system("_dispatch_group_leave", {group})); } catch (const std::runtime_error&) { unbalanced = true; }
    require(unbalanced, "unbalanced dispatch_group_leave accepted");
    static_cast<void>(f.system("_dispatch_release", {group}));

    // dispatch_apply runs every index once and returns after all of them.
    std::set<Address> indexes;
    const auto iteration = f.stub("test_apply", [&](auto& state, auto&) {
        require(state.x[0] == 77, "dispatch_apply context");
        std::lock_guard lock(f.gate); indexes.insert(state.x[1]); state.x[0] = 0;
    });
    static_cast<void>(f.system("_dispatch_apply_f", {64, global, 77, iteration}));
    { std::lock_guard lock(f.gate); require(indexes.size() == 64 && *indexes.rbegin() == 63, "dispatch_apply indexes"); }
    indexes.clear();
    static_cast<void>(f.system("_dispatch_apply_f", {5, serial, 77, iteration}));
    require(indexes.size() == 5, "serial dispatch_apply");

    // Pooled workers: once earlier idle workers expired (1 s), back-to-back
    // activations reuse the one remaining guest thread.
    std::this_thread::sleep_for(std::chrono::milliseconds(1300));
    static_cast<void>(f.system("_dispatch_async_f", {serial, 300, mark}));
    eventually([&] { return count("mark") == 3; }, "serial work did not run");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    static_cast<void>(f.system("_dispatch_async_f", {serial, 301, mark}));
    eventually([&] { return count("mark") == 4; }, "serial work did not run again");
    {
        std::lock_guard lock(f.gate);
        require(events[events.size() - 1].thread == events[events.size() - 2].thread, "idle worker was not reused");
    }
    // Suspension holds queued work until resume.
    static_cast<void>(f.system("_dispatch_suspend", {serial}));
    static_cast<void>(f.system("_dispatch_async_f", {serial, 302, mark}));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    require(count("mark") == 4, "suspended queue ran work");
    static_cast<void>(f.system("_dispatch_resume", {serial}));
    eventually([&] { return count("mark") == 5; }, "resumed queue did not run work");

    // Timer source on a serial queue.
    const auto timer = f.system("_dispatch_source_create", {f.symbol(libsystem, "__dispatch_source_type_timer"), 0, 0, serial});
    std::atomic<int> ticks{};
    std::atomic<Address> data{};
    const auto tick = f.stub("test_tick", [&](auto& state, auto&) {
        require(state.x[0] == 5, "timer source handler context");
        data = f.system("_dispatch_source_get_data", {timer});
        ++ticks; state.x[0] = 0;
    });
    std::atomic<bool> cancelled{};
    const auto cancel = f.stub("test_cancel", [&](auto& state, auto&) { cancelled = true; state.x[0] = 0; });
    static_cast<void>(f.system("_dispatch_set_context", {timer, 5}));
    static_cast<void>(f.system("_dispatch_source_set_event_handler_f", {timer, tick}));
    static_cast<void>(f.system("_dispatch_source_set_cancel_handler_f", {timer, cancel}));
    static_cast<void>(f.system("_dispatch_source_set_timer", {timer, f.system("_dispatch_time", {0, 10'000'000}), 10'000'000, 0}));
    static_cast<void>(f.system("_dispatch_resume", {timer}));
    eventually([&] { return ticks >= 3; }, "timer source did not fire repeatedly");
    require(data >= 1, "timer source data");
    static_cast<void>(f.system("_dispatch_source_cancel", {timer}));
    eventually([&] { return cancelled.load(); }, "cancel handler did not run");
    require(f.system("_dispatch_source_testcancel", {timer}) != 0, "testcancel");
    const auto settled = ticks.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    require(ticks == settled, "cancelled timer kept firing");
    static_cast<void>(f.system("_dispatch_release", {timer}));

    // DATA_ADD coalesces merges made while the source is inactive.
    const auto adder = f.system("_dispatch_source_create", {f.symbol(libsystem, "__dispatch_source_type_data_add"), 0, 0, serial});
    std::vector<Address> merged;
    const auto merge_handler = f.stub("test_merge", [&](auto& state, auto&) {
        std::lock_guard lock(f.gate); merged.push_back(f.system("_dispatch_source_get_data", {adder})); state.x[0] = 0;
    });
    static_cast<void>(f.system("_dispatch_source_set_event_handler_f", {adder, merge_handler}));
    for (Address value : {1, 2, 3}) static_cast<void>(f.system("_dispatch_source_merge_data", {adder, value}));
    static_cast<void>(f.system("_dispatch_activate", {adder}));
    eventually([&] { std::lock_guard lock(f.gate); return merged.size() == 1; }, "data source did not fire");
    static_cast<void>(f.system("_dispatch_source_merge_data", {adder, 4}));
    eventually([&] { std::lock_guard lock(f.gate); return merged.size() == 2; }, "data source did not fire again");
    { std::lock_guard lock(f.gate); require(merged[0] == 6 && merged[1] == 4, "data source merge values"); }
    static_cast<void>(f.system("_dispatch_source_cancel", {adder}));
    static_cast<void>(f.system("_dispatch_release", {adder}));
    bool unsupported{};
    try { static_cast<void>(f.system("_dispatch_source_create", {f.symbol(libsystem, "__dispatch_source_type_signal"), 15, 0, serial})); }
    catch (const std::runtime_error& error) { unsupported = std::string(error.what()).find("unsupported") != std::string::npos; }
    require(unsupported, "unsupported source type did not fail explicitly");
    static_cast<void>(f.system("_dispatch_release", {serial}));
    static_cast<void>(f.system("_dispatch_release", {concurrent}));
}

void test_foundation_and_cf(Fixture& f) {
    auto& loop = alchemy::gui::main_run_loop();
    const auto run_loop_class = f.objc.find_class("NSRunLoop");
    const auto current = f.send(run_loop_class, "currentRunLoop");
    require(current && current == f.send(run_loop_class, "mainRunLoop"), "currentRunLoop on the main thread");
    const auto cf_main = f.core("_CFRunLoopGetMain");
    require(cf_main == f.send(current, "getCFRunLoop") && cf_main == f.core("_CFRunLoopGetCurrent"), "CFRunLoop identity");
    const auto default_mode = f.mode("kCFRunLoopDefaultMode"), common = f.mode("kCFRunLoopCommonModes");
    require(f.cf.utf8(default_mode) == "kCFRunLoopDefaultMode", "default mode constant");
    // NSEventTrackingRunLoopMode / NSModalPanelRunLoopMode are AppKit's
    // constants (appkit_tests.cpp checks them).

    // CFRunLoopSource version 0: signal, then CFRunLoopRunInMode returns after it.
    std::vector<Address> performed;
    const auto perform = f.stub("test_source_perform", [&](auto& state, auto&) { performed.push_back(state.x[0]); state.x[0] = 0; });
    const auto context = f.process.allocate_guest(80);
    f.engine.fill_bytes(context, 0, 80);
    f.engine.write64(context + 8, 123); f.engine.write64(context + 72, perform);
    const auto source = f.core("_CFRunLoopSourceCreate", {0, 0, context});
    static_cast<void>(f.core("_CFRunLoopAddSource", {cf_main, source, default_mode}));
    require(f.core("_CFRunLoopContainsSource", {cf_main, source, default_mode}) & 0xff, "CFRunLoopContainsSource");
    static_cast<void>(f.core("_CFRunLoopSourceSignal", {source}));
    require(f.call_d0(alchemy::core_foundation_library, "_CFRunLoopRunInMode", {default_mode, 1}, 1.0) == 4 &&
            performed == std::vector<Address>{123}, "CFRunLoopRunInMode source");
    static_cast<void>(f.core("_CFRunLoopSourceInvalidate", {source}));
    require(!(f.core("_CFRunLoopSourceIsValid", {source}) & 0xff), "source invalidation");
    static_cast<void>(f.core("_CFRelease", {source}));

    // CFRunLoopObserver in the common modes.
    std::vector<Address> activities;
    const auto callout = f.stub("test_observer", [&](auto& state, auto&) { activities.push_back(state.x[1]); state.x[0] = 0; });
    const auto observer = f.core("_CFRunLoopObserverCreate", {0, activity::entry | activity::exit, 1, 0, callout, 0});
    static_cast<void>(f.core("_CFRunLoopAddObserver", {cf_main, observer, common}));
    require(f.call_d0(alchemy::core_foundation_library, "_CFRunLoopRunInMode", {default_mode, 0}, 0.0) == 3,
            "zero-timeout CFRunLoopRunInMode");
    require((activities == std::vector<Address>{activity::entry, activity::exit}), "observer callouts");
    static_cast<void>(f.core("_CFRunLoopObserverInvalidate", {observer}));
    static_cast<void>(f.core("_CFRelease", {observer}));

    // NSTimer block timer fires once and invalidates itself.
    const auto timer_class = f.objc.find_class("NSTimer");
    std::vector<std::pair<Address, Address>> timer_calls;
    const auto timer_invoke = f.stub("test_timer_block", [&](auto& state, auto&) { timer_calls.push_back({state.x[0], state.x[1]}); state.x[0] = 0; });
    const auto timer_block = f.block(timer_invoke);
    const auto timer = f.send(timer_class, "scheduledTimerWithTimeInterval:repeats:block:", {0, timer_block}, 0.01);
    require(timer && (f.send(timer, "isValid") & 0xff), "scheduled NSTimer");
    static_cast<void>(f.send(timer, "retain"));
    require(loop.run_once(alchemy::gui::default_mode, 0.2, false) == RunResult::timed_out, "timer run");
    require(timer_calls.size() == 1 && timer_calls[0] == std::pair{timer_block, timer}, "NSTimer block invoke");
    require(!(f.send(timer, "isValid") & 0xff), "fired one-shot NSTimer is still valid");
    static_cast<void>(f.send(timer, "release"));

    // -performSelector:withObject:afterDelay: on a mutable array.
    const auto array = f.cf.array({}, true);
    const auto item = f.cf.string("item");
    static_cast<void>(f.send(array, "performSelector:withObject:afterDelay:", {f.objc.selector("addObject:"), item}, 0.01));
    static_cast<void>(f.send(array, "performSelector:withObject:afterDelay:", {f.objc.selector("addObject:"), item}, 0.01));
    static_cast<void>(f.send(f.objc.find_class("NSObject"), "cancelPreviousPerformRequestsWithTarget:selector:object:",
                             {array, f.objc.selector("addObject:"), item}));
    static_cast<void>(f.send(array, "performSelector:withObject:afterDelay:", {f.objc.selector("addObject:"), item}, 0.01));
    static_cast<void>(loop.run_once(alchemy::gui::default_mode, 0.2, false));
    require(f.cf.array_values(array).size() == 1, "delayed perform or its cancellation");

    // -[NSRunLoop performSelector:target:argument:order:modes:] and its cancellation.
    const auto run_loop = f.send(f.objc.find_class("NSRunLoop"), "currentRunLoop", {});
    const auto modes = f.cf.array(std::vector<Address>{f.cf.string("kCFRunLoopDefaultMode")});
    const auto add = f.objc.selector("addObject:");
    static_cast<void>(f.send(run_loop, "performSelector:target:argument:order:modes:", {add, array, item, 0, modes}));
    static_cast<void>(f.send(run_loop, "performSelector:target:argument:order:modes:", {add, array, item, 0, modes}));
    static_cast<void>(f.send(run_loop, "cancelPerformSelector:target:argument:", {add, array, item}));
    static_cast<void>(f.send(run_loop, "performSelector:target:argument:order:modes:", {add, array, item, 0, modes}));
    static_cast<void>(loop.run_once(alchemy::gui::default_mode, 0.2, false));
    require(f.cf.array_values(array).size() == 2, "NSRunLoop perform request or its cancellation");
    static_cast<void>(f.send(run_loop, "performSelector:target:argument:order:modes:", {add, array, item, 0, modes}));
    static_cast<void>(f.send(run_loop, "cancelPerformSelectorsWithTarget:", {array}));
    static_cast<void>(loop.run_once(alchemy::gui::default_mode, 0.05, false));
    require(f.cf.array_values(array).size() == 2, "cancelPerformSelectorsWithTarget:");
    f.process.objects().release(modes);

    // -performSelectorOnMainThread:...waitUntilDone:YES from a worker.
    auto worker = f.engine.fork_thread(0, 0);
    const auto message = f.symbol(alchemy::objc_library, "_objc_msgSend");
    std::atomic<bool> returned{};
    std::thread caller([&] {
        const std::array arguments{array, f.objc.selector("performSelectorOnMainThread:withObject:waitUntilDone:"),
                                   f.objc.selector("addObject:"), item, Address{1}};
        static_cast<void>(worker->invoke_callback(message, arguments));
        returned = true;
    });
    const auto result = loop.run_once(alchemy::gui::default_mode, 5, true);
    caller.join();
    require(result == RunResult::handled_source && returned && f.cf.array_values(array).size() == 3, "performSelectorOnMainThread");
    f.process.objects().release(array);
    f.process.objects().release(item);
}
// CFRunLoopStop stops the run active at the time of the call: a run entered
// later (a modal step nested in a Win32 loop) does not consume it, and a stop
// outside any run is dropped.
void test_stop_marks(Fixture& f) {
    static_cast<void>(f);
    auto& loop = alchemy::gui::main_run_loop();
    loop.stop();
    require(loop.run_once(alchemy::gui::default_mode, 0.02, false) == RunResult::timed_out, "a stop outside a run was kept");
    RunResult nested{};
    static_cast<void>(loop.add_timer(alchemy::gui::monotonic_seconds(), 0, alchemy::gui::default_mode, [&](alchemy::gui::TimerId) {
        loop.stop();
        nested = loop.run_once(alchemy::gui::event_tracking_mode, 0.02, false);
    }));
    const auto start = alchemy::gui::monotonic_seconds();
    require(loop.run_once(alchemy::gui::default_mode, 5, false) == RunResult::stopped && since(start) < 2,
            "the outer run did not stop");
    require(nested == RunResult::timed_out, "a nested run consumed the outer run's stop");
}

// Closes the thread's open message boxes (dialog class #32770; an MB_OK box answers IDOK).
void close_message_boxes(DWORD thread) {
    EnumThreadWindows(thread, [](HWND window, LPARAM) -> BOOL {
        wchar_t name[16]{};
        GetClassNameW(window, name, 16);
        if (std::wstring_view(name) == L"#32770") PostMessageW(window, WM_CLOSE, 0, 0);
        return TRUE;
    }, 0);
}

// A Win32 dialog loop (MessageBoxW, as behind -[NSAlert runModal]) keeps the
// main loop running in NSModalPanelRunLoopMode: common-mode timers and
// modal-panel blocks run, default-mode timers wait, and a step inside the
// dialog loop never pumps messages itself.
void test_win32_modal_loops(Fixture& f) {
    static_cast<void>(f);
    auto& loop = alchemy::gui::main_run_loop();
    auto& impl = static_cast<alchemy::gui::detail::RunLoopImpl&>(loop);
    if (!IsGUIThread(TRUE)) { std::cout << "modal loop test skipped: no GUI thread\n"; return; }
    static_cast<void>(loop.run_once(alchemy::gui::default_mode, 0, false));   // installs the dialog-loop hook
    const auto main_thread = GetCurrentThreadId();
    std::atomic<int> dispatched{};
    loop.set_message_dispatcher([&] { ++dispatched; return false; });
    int common = 0, default_only = 0, panel_blocks = 0, default_blocks = 0;
    std::vector<std::string> modes;
    const auto now = alchemy::gui::monotonic_seconds();
    const auto ticker = loop.add_timer(now + 0.01, 0.01, alchemy::gui::common_modes, [&](alchemy::gui::TimerId) {
        modes.push_back(loop.current_mode());
        if (++common == 3) close_message_boxes(main_thread);
    });
    const auto default_timer = loop.add_timer(now + 0.005, 0.005, alchemy::gui::default_mode,
                                              [&](alchemy::gui::TimerId) { ++default_only; });
    impl.perform({std::string(alchemy::gui::modal_panel_mode)}, [&] { ++panel_blocks; }, false);
    impl.perform({std::string(alchemy::gui::default_mode)}, [&] { ++default_blocks; }, false);
    std::atomic<bool> returned{};
    std::thread watchdog([&] {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!returned && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        while (!returned) { close_message_boxes(main_thread); std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
    });
    const auto answer = MessageBoxW(nullptr, L"Alchemy run loop test: closes itself", L"Alchemy", MB_OK | MB_TASKMODAL);
    returned = true;
    watchdog.join();
    loop.cancel_timer(ticker);
    loop.set_message_dispatcher({});
    if (answer == 0) {
        loop.cancel_timer(default_timer);
        std::cout << "modal loop test skipped: MessageBoxW failed (error " << GetLastError() << ")\n";
        return;
    }
    require(common >= 3, "common-mode timer did not fire inside the dialog loop");
    require(std::all_of(modes.begin(), modes.end(), [](const std::string& mode) { return mode == alchemy::gui::modal_panel_mode; }),
            "dialog loop steps did not run in NSModalPanelRunLoopMode");
    require(panel_blocks == 1 && default_blocks == 0, "dialog loop ran the wrong mode's blocks");
    require(default_only == 0, "default-mode timer fired inside the dialog loop");
    require(dispatched == 0, "a step inside the dialog loop pumped messages");
    // Back in the default mode the waiting items run.
    static_cast<void>(loop.run_once(alchemy::gui::default_mode, 0.05, false));
    loop.cancel_timer(default_timer);
    require(default_only > 0 && default_blocks == 1, "default-mode items did not run after the dialog");
    // The dialog's thread timer stops once no dialog loop sees it.
    static_cast<void>(loop.run_once(alchemy::gui::default_mode, 0.05, false));
}
} // namespace

int main() {
    try {
        Fixture f;
        require(alchemy::gui::main_run_loop_available(), "install_runloop did not create the main run loop");
        test_timers_and_modes(f);
        test_wake_and_sources(f);
        test_main_queue_wake(f);
        test_completion_thread(f);
        test_dispatch(f);
        test_foundation_and_cf(f);
        test_stop_marks(f);
        test_win32_modal_loops(f);
        std::cout << "Alchemy GUI run loop tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
