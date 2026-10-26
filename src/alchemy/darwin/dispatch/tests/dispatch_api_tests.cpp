// libdispatch beyond queues, groups and sources (dispatch.cpp, dispatch_objects.cpp): dispatch
// blocks, queue assertions, attributes, target-queue hierarchies, finalizers, QoS, workloops,
// signal sources and the I/O conveniences.
#include "alchemy/darwin/kit/completion/runtime/tests/runtime_test_kit.hpp"
#include "alchemy/darwin/dispatch/install.hpp"
#include "alchemy/engine_adapter.hpp"
#include "alchemy/path.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

namespace {

constexpr Address forever = UINT64_MAX;
constexpr std::string_view sys = "/usr/lib/libSystem.B.dylib";

// Waits (polling, up to five seconds) until `done` holds.
template <class Condition> void eventually(Condition done, std::string_view what) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("timed out: " + std::string(what));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
// A libSystem call from a host body running on a guest worker thread: through that thread's
// engine (Fixture::libsystem calls on the main engine, as the main thread).
Address here(Fixture& f, std::string_view name, std::initializer_list<Address> arguments) {
    return alchemy::EngineAdapter(f.process.calling_engine()).call(f.symbol(sys, name), std::span(arguments.begin(), arguments.size()));
}
template <class Operation> bool stops(Operation operation) {
    try { operation(); } catch (const std::exception&) { return true; }
    return false;
}

void test_blocks(Fixture& f) {
    static std::atomic<int> runs{0}, notified{0};
    const auto body = f.process.install_function("_dispatch_api_body", [](CpuState& s, GuestMemory&) { ++runs; s.x[0] = 0; });
    const auto note = f.process.install_function("_dispatch_api_note", [](CpuState& s, GuestMemory&) { ++notified; s.x[0] = 0; });
    const auto noop = f.process.install_function("_dispatch_api_noop", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    const auto queue = f.libsystem("_dispatch_queue_create", {f.c_string("api.blocks"), 0});
    const auto block = f.libsystem("_dispatch_block_create", {0, f.block(body)});
    require(block != 0, "dispatch_block_create");
    require(f.libsystem("_dispatch_block_wait", {block, 0}) != 0, "a block not yet run has not finished");
    f.libsystem("_dispatch_block_notify", {block, queue, f.block(note)});
    f.libsystem("_dispatch_async", {queue, block});
    require(f.libsystem("_dispatch_block_wait", {block, forever}) == 0 && runs == 1, "dispatch_block_wait returns once the block ran");
    f.libsystem("_dispatch_sync_f", {queue, 0, noop});
    eventually([] { return notified == 1; }, "dispatch_block_notify submits its block");
    // A notification added after the block finished is submitted at once.
    f.libsystem("_dispatch_block_notify", {block, queue, f.block(note)});
    eventually([] { return notified == 2; }, "dispatch_block_notify after completion");
    // A cancelled block returns at once when invoked, and counts as finished.
    const auto cancelled = f.libsystem("_dispatch_block_create", {0, f.block(body)});
    require(f.libsystem("_dispatch_block_testcancel", {cancelled}) == 0, "not cancelled yet");
    f.libsystem("_dispatch_block_cancel", {cancelled});
    require(f.libsystem("_dispatch_block_testcancel", {cancelled}) == 1, "dispatch_block_testcancel");
    f.libsystem("_dispatch_sync", {queue, cancelled});
    require(runs == 1 && f.libsystem("_dispatch_block_wait", {cancelled, 0}) == 0, "a cancelled block does not run");
    f.libsystem("_dispatch_block_perform", {0, f.block(body)});
    require(runs == 2, "dispatch_block_perform runs the block");
    require(f.libsystem("_dispatch_block_create", {0x400, f.block(body)}) == 0, "unknown flags make no block");
    require(f.libsystem("_dispatch_block_create_with_qos_class", {0, 0x19, 0, f.block(body)}) != 0, "dispatch_block_create_with_qos_class");
    for (const auto made : {block, cancelled}) f.libsystem("__Block_release", {made});
    f.libsystem("_dispatch_release", {queue});
}

void test_assertions(Fixture& f) {
    const auto queue = f.libsystem("_dispatch_queue_create", {f.c_string("api.assert"), 0});
    require(stops([&] { f.libsystem("_dispatch_assert_queue", {queue}); }), "dispatch_assert_queue off the queue stops");
    f.libsystem("_dispatch_assert_queue_not", {queue});
    f.libsystem("_dispatch_assert_queue", {f.symbol(sys, "__dispatch_main_q")});   // the main thread runs the main queue
    static Fixture* fixture;
    static Address probed;
    static bool inside_ok, not_stopped;
    fixture = &f; probed = queue;
    const auto probe = f.process.install_function("_dispatch_api_assert_probe", [](CpuState& s, GuestMemory&) {
        fixture->libsystem("_dispatch_assert_queue", {probed});
        fixture->libsystem("_dispatch_assert_queue_barrier", {probed});
        inside_ok = true;
        not_stopped = stops([] { fixture->libsystem("_dispatch_assert_queue_not", {probed}); });
        s.x[0] = 0;
    });
    f.libsystem("_dispatch_sync_f", {queue, 0, probe});
    require(inside_ok && not_stopped, "the assertions hold inside the queue's work");
    require(f.libsystem("_dispatch_get_current_queue") == f.symbol(sys, "__dispatch_main_q"), "dispatch_get_current_queue on the main thread");
    f.libsystem("_dispatch_release", {queue});
}

void test_attributes(Fixture& f) {
    static std::atomic<int> runs{0};
    const auto body = f.process.install_function("_dispatch_api_attr_body", [](CpuState& s, GuestMemory&) { ++runs; s.x[0] = 0; });
    const auto noop = f.process.install_function("_dispatch_api_attr_noop", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    const auto inactive = f.libsystem("_dispatch_queue_attr_make_initially_inactive", {0});
    require(inactive != 0, "dispatch_queue_attr_make_initially_inactive");
    const auto queue = f.libsystem("_dispatch_queue_create", {f.c_string("api.inactive"), inactive});
    f.libsystem("_dispatch_async_f", {queue, 0, body});
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    require(runs == 0, "an inactive queue holds its work");
    f.libsystem("_dispatch_activate", {queue});
    f.libsystem("_dispatch_sync_f", {queue, 0, noop});
    require(runs == 1, "dispatch_activate starts the queue");
    const auto concurrent = f.symbol(sys, "__dispatch_queue_attr_concurrent");
    const auto qos = f.libsystem("_dispatch_queue_attr_make_with_qos_class", {concurrent, 0x19, 0});
    const auto typed = f.libsystem("_dispatch_queue_create", {f.c_string("api.qos"), qos});
    const auto relative = f.process.allocate_guest(4);
    require(f.libsystem("_dispatch_queue_get_qos_class", {typed, relative}) == 0x19 &&
            f.libsystem("_dispatch_queue_get_qos_class", {f.symbol(sys, "__dispatch_main_q"), relative}) == 0x21, "dispatch_queue_get_qos_class");
    require(f.objc.object_class(typed) == f.cls("OS_dispatch_queue_concurrent"), "a QoS attribute keeps the queue concurrent");
    require(f.libsystem("_dispatch_queue_attr_make_with_autorelease_frequency", {0, 1}) != 0, "dispatch_queue_attr_make_with_autorelease_frequency");
    f.libsystem("_dispatch_set_qos_class_floor", {typed, 0x11, 0});
    // Workloops run their work in order, one item at a time.
    const auto workloop = f.libsystem("_dispatch_workloop_create", {f.c_string("api.workloop")});
    f.libsystem("_dispatch_async_f", {workloop, 0, body});
    f.libsystem("_dispatch_async_and_wait_f", {workloop, 0, body});
    require(runs == 3, "a workloop and dispatch_async_and_wait_f");
    for (const auto object : {queue, typed, workloop}) f.libsystem("_dispatch_release", {object});
}

void test_targets(Fixture& f) {
    static std::atomic<int> inside{0}, widest{0}, done{0};
    static Address seen{}, key{};
    static Fixture* fixture;
    fixture = &f;
    const auto body = f.process.install_function("_dispatch_api_target_body", [](CpuState& s, GuestMemory&) {
        const auto now = ++inside;
        for (int expected = widest; now > expected && !widest.compare_exchange_weak(expected, now);) {}
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        seen = here(*fixture, "_dispatch_get_specific", {key});
        --inside; ++done;
        s.x[0] = 0;
    });
    const auto noop = f.process.install_function("_dispatch_api_target_noop", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    const auto target = f.libsystem("_dispatch_queue_create", {f.c_string("api.target"), 0});
    const auto concurrent = f.symbol(sys, "__dispatch_queue_attr_concurrent");
    const auto first = f.libsystem("_dispatch_queue_create", {f.c_string("api.first"), concurrent});
    const auto second = f.libsystem("_dispatch_queue_create", {f.c_string("api.second"), 0});
    f.libsystem("_dispatch_set_target_queue", {first, target});
    f.libsystem("_dispatch_set_target_queue", {second, target});
    key = f.c_string("api.key");
    f.libsystem("_dispatch_queue_set_specific", {target, key, 0x4242, 0});
    for (int index = 0; index < 12; ++index) f.libsystem("_dispatch_async_f", {index % 2 ? first : second, 0, body});
    f.libsystem("_dispatch_sync_f", {first, 0, noop});
    f.libsystem("_dispatch_sync_f", {second, 0, noop});
    eventually([] { return done == 12; }, "work through a target queue");
    require(widest == 1, "a serial target runs its queues' work one item at a time");
    require(seen == 0x4242, "dispatch_get_specific finds the target's value");
    require(stops([&] { f.libsystem("_dispatch_set_target_queue", {target, first}); }), "a cycle of targets is refused");
    for (const auto object : {first, second, target}) f.libsystem("_dispatch_release", {object});
}

void test_finalizers(Fixture& f) {
    static Address finalized{};
    const auto finalizer = f.process.install_function("_dispatch_api_finalizer", [](CpuState& s, GuestMemory&) { finalized = s.x[0]; s.x[0] = 0; });
    const auto queue = f.libsystem("_dispatch_queue_create", {f.c_string("api.finalizer"), 0});
    f.libsystem("_dispatch_set_context", {queue, 0x77});
    f.libsystem("_dispatch_set_finalizer_f", {queue, finalizer});
    f.libsystem("_dispatch_release", {queue});
    eventually([] { return finalized == 0x77; }, "the finalizer runs with the queue's context");
}

void test_signal_sources(Fixture& f) {
    static std::atomic<int> events{0};
    static std::atomic<Address> data{0};
    static Fixture* fixture;
    static Address source;
    fixture = &f;
    constexpr Address usr1 = 30;
    f.libsystem("_signal", {usr1, 1});   // SIG_IGN: the source still sees the signal
    const auto handler = f.process.install_function("_dispatch_api_signal_handler", [](CpuState& s, GuestMemory&) {
        data += here(*fixture, "_dispatch_source_get_data", {source});
        ++events;
        s.x[0] = 0;
    });
    const auto queue = f.libsystem("_dispatch_queue_create", {f.c_string("api.signal"), 0});
    source = f.libsystem("_dispatch_source_create", {f.symbol(sys, "__dispatch_source_type_signal"), usr1, 0, queue});
    require(source != 0, "DISPATCH_SOURCE_TYPE_SIGNAL");
    require(f.libsystem("_dispatch_source_create", {f.symbol(sys, "__dispatch_source_type_signal"), 40, 0, queue}) == 0, "an invalid signal number");
    f.libsystem("_dispatch_source_set_event_handler_f", {source, handler});
    f.libsystem("_dispatch_activate", {source});
    f.libsystem("_raise", {usr1});
    f.libsystem("_raise", {usr1});
    eventually([] { return data == 2; }, "two deliveries reach the source");
    f.libsystem("_dispatch_source_cancel", {source});
    f.libsystem("_dispatch_release", {source});
    f.libsystem("_dispatch_release", {queue});
}

void test_io(Fixture& f) {
    const auto directory = std::filesystem::temp_directory_path() / "alchemy-dispatch-api";
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    std::filesystem::create_directories(directory);
    const auto path = directory / "input.txt";
    { std::ofstream file(path, std::ios::binary); file << "hello dispatch io"; }
    static std::mutex mutex;
    static std::vector<Address> sizes;
    static std::atomic<bool> finished{false}, barrier_ran{false}, cleaned{false}, read_done{false}, written{false};
    static std::atomic<Address> read_size{0};
    static Fixture* fixture;
    fixture = &f;
    const auto handler = f.process.install_function("_dispatch_api_io_handler", [](CpuState& s, GuestMemory&) {
        // ^(bool done, dispatch_data_t data, int error)
        if (s.x[2]) { std::lock_guard lock(mutex); if (const auto size = here(*fixture, "_dispatch_data_get_size", {s.x[2]})) sizes.push_back(size); }
        if (s.x[1] & 1) finished = static_cast<std::uint32_t>(s.x[3]) == 0;
        s.x[0] = 0;
    });
    const auto barrier = f.process.install_function("_dispatch_api_io_barrier", [](CpuState& s, GuestMemory&) { barrier_ran = finished.load(); s.x[0] = 0; });
    const auto cleanup = f.process.install_function("_dispatch_api_io_cleanup", [](CpuState& s, GuestMemory&) { cleaned = s.x[1] == 0; s.x[0] = 0; });
    const auto queue = f.libsystem("_dispatch_queue_create", {f.c_string("api.io"), 0});
    const auto channel = f.libsystem("_dispatch_io_create_with_path", {0, f.c_string(alchemy::guest_path_string(path)), 0, 0, queue, f.block(cleanup)});
    require(channel != 0, "dispatch_io_create_with_path");
    f.libsystem("_dispatch_io_set_high_water", {channel, 4});
    f.libsystem("_dispatch_io_read", {channel, 0, UINT64_MAX, queue, f.block(handler)});
    f.libsystem("_dispatch_io_barrier", {channel, f.block(barrier)});
    eventually([] { return finished.load() && barrier_ran.load(); }, "a path channel reads, then its barrier runs");
    {
        std::lock_guard lock(mutex);
        Address total{};
        for (const auto size : sizes) { require(size <= 4, "the high-water mark caps each delivery"); total += size; }
        require(total == 17, "the whole file was delivered");
    }
    f.libsystem("_dispatch_io_close", {channel, 0});
    f.libsystem("_dispatch_release", {channel});
    eventually([] { return cleaned.load(); }, "the cleanup handler runs after close");
    require(f.libsystem("_dispatch_io_create_with_path", {0, f.c_string("relative.txt"), 0, 0, queue, 0}) == 0, "a relative path makes no channel");
    // dispatch_read / dispatch_write: one handler call with all the data.
    const auto read_handler = f.process.install_function("_dispatch_api_read_handler", [](CpuState& s, GuestMemory&) {
        // ^(dispatch_data_t data, int error)
        read_size = s.x[1] ? here(*fixture, "_dispatch_data_get_size", {s.x[1]}) : 0;
        read_done = static_cast<std::uint32_t>(s.x[2]) == 0;
        s.x[0] = 0;
    });
    const auto descriptor = f.libsystem("_open", {f.c_string(alchemy::guest_path_string(path)), 0, 0});
    f.libsystem("_dispatch_read", {descriptor, UINT64_MAX, queue, f.block(read_handler)});
    eventually([] { return read_done.load(); }, "dispatch_read");
    require(read_size == 17, "dispatch_read delivers the whole file at once");
    f.libsystem("_close", {descriptor});
    const auto write_handler = f.process.install_function("_dispatch_api_write_handler", [](CpuState& s, GuestMemory&) {
        written = s.x[1] == 0 && static_cast<std::uint32_t>(s.x[2]) == 0;   // nothing left over
        s.x[0] = 0;
    });
    const auto output = directory / "output.txt";
    const auto out = f.libsystem("_open", {f.c_string(alchemy::guest_path_string(output)), 0x601, 0644});   // O_WRONLY | O_CREAT | O_TRUNC
    const auto payload = f.c_string("written");
    const auto data = f.libsystem("_dispatch_data_create", {payload, 7, 0, 0});
    f.libsystem("_dispatch_write", {out, data, queue, f.block(write_handler)});
    eventually([] { return written.load(); }, "dispatch_write");
    f.libsystem("_close", {out});
    f.libsystem("_dispatch_release", {data});
    require(std::filesystem::file_size(output) == 7, "dispatch_write wrote the data");
    f.libsystem("_dispatch_release", {queue});
    std::filesystem::remove_all(directory, ignored);
}

} // namespace

int main() {
    try {
        Fixture fixture;
        {   // dispatch_data_t, which dispatch_read and the I/O channels hand to their handlers
            alchemy::gui::InstallContext context{fixture.process, fixture.objc, fixture.cf};
            alchemy::dispatch::install_dispatch_data(context);
        }
        const auto stage = [](std::string_view name) { std::cout << "[stage] " << name << std::endl; };
        stage("blocks"); test_blocks(fixture);
        stage("assertions"); test_assertions(fixture);
        stage("attributes"); test_attributes(fixture);
        stage("targets"); test_targets(fixture);
        stage("finalizers"); test_finalizers(fixture);
        stage("signal sources"); test_signal_sources(fixture);
        stage("io"); test_io(fixture);
        std::cout << "Alchemy dispatch API tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
