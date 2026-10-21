#include "alchemy/darwin_dispatch.hpp"
#include "alchemy/darwin_services.hpp"
#include "alchemy/darwin_threads.hpp"
#include "alchemy/engine_adapter.hpp"

#include <atomic>
#include <mutex>
#include <thread>

#include <array>
#include <chrono>
#include <future>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr auto forever = UINT64_MAX;
void require(bool value, std::string_view message) {
    if (!value) throw std::runtime_error(std::string(message));
}
struct Fixture {
    transmute::ReferenceEngine engine{transmute::ExecutionStart{0}};
    alchemy::ProcessContext process{engine, [](auto, auto bytes) -> alchemy::OutputResult {
        return {bytes.size(), 0};
    }};
    Fixture() { alchemy::install_darwin_dispatch(process); }
    std::uint64_t symbol(std::string_view name) {
        const auto value = process.resolve("/usr/lib/libSystem.B.dylib", name);
        require(value.has_value(), "missing dispatch function"); return value->address;
    }
    std::uint64_t call(std::string_view name, std::initializer_list<std::uint64_t> arguments = {}) {
        return engine.invoke_callback(symbol(name), std::span(arguments.begin(), arguments.size()));
    }
    template<class Operation> void rejected(Operation operation, std::string_view reason) {
        bool caught{};
        try { operation(); } catch (const std::runtime_error&) { caught = true; }
        require(caught, reason);
    }
};

void test_counting_and_ownership() {
    Fixture f;
    require(f.call("_dispatch_semaphore_create", {UINT64_MAX}) == 0, "negative semaphore count accepted");
    const auto semaphore = f.call("_dispatch_semaphore_create", {2});
    require(semaphore && f.engine.query_region(semaphore), "semaphore lacks guest storage");
    const auto alias = f.process.resolve("/usr/lib/system/libdispatch.dylib", "_dispatch_semaphore_wait");
    require(alias && alias->address == f.symbol("_dispatch_semaphore_wait"), "libdispatch identity does not share implementation");
    require(f.call("_dispatch_semaphore_wait", {semaphore, 0}) == 0 &&
        f.call("_dispatch_semaphore_wait", {semaphore, 0}) == 0 &&
        f.call("_dispatch_semaphore_wait", {semaphore, 0}) != 0, "semaphore permit accounting");
    require(f.call("_dispatch_semaphore_signal", {semaphore}) == 0 &&
        f.call("_dispatch_semaphore_wait", {semaphore, 0}) == 0, "nonblocking signal/wait");
    static_cast<void>(f.call("_dispatch_retain", {semaphore}));
    static_cast<void>(f.call("_dispatch_release", {semaphore}));
    require(f.call("_dispatch_semaphore_signal", {semaphore}) == 0, "retain did not keep semaphore alive");
    static_cast<void>(f.call("_dispatch_release", {semaphore}));
    require(!f.engine.query_region(semaphore), "last release retained guest object storage");
    f.rejected([&] { static_cast<void>(f.call("_dispatch_release", {semaphore})); }, "released handle survived");
    f.rejected([&] { static_cast<void>(f.call("_dispatch_semaphore_wait", {0, 0})); }, "null handle accepted");
    const auto full = f.call("_dispatch_semaphore_create", {INT64_MAX});
    f.rejected([&] { static_cast<void>(f.call("_dispatch_semaphore_signal", {full})); }, "semaphore count overflow accepted");
    static_cast<void>(f.call("_dispatch_release", {full}));
}

void test_deadlines() {
    Fixture f;
    require(f.call("_dispatch_time", {forever, 27}) == forever, "forever changed with delta");
    require(f.call("_dispatch_time", {1, 1ULL << 63}) == 1, "negative deadline underflow");
    require(f.call("_dispatch_time", {(1ULL << 63) - 4, 27}) == forever, "deadline overflow");
    const auto semaphore = f.call("_dispatch_semaphore_create", {0});
    const auto expired = f.call("_dispatch_time", {0, static_cast<std::uint64_t>(-1'000'000'000LL)});
    require(f.call("_dispatch_semaphore_wait", {semaphore, expired}) != 0, "expired deadline blocked");
    const auto finite = f.call("_dispatch_time", {0, 2'000'000});
    const auto start = std::chrono::steady_clock::now();
    require(f.call("_dispatch_semaphore_wait", {semaphore, finite}) != 0, "finite deadline succeeded without signal");
    require(std::chrono::steady_clock::now() - start < std::chrono::seconds(1), "finite deadline did not expire");
    const auto storage = f.process.allocate_guest(16);
    f.engine.write64(storage, 0); f.engine.write64(storage + 8, 0);
    const auto wall = f.call("_dispatch_walltime", {storage, 0});
    require(f.call("_dispatch_semaphore_wait", {semaphore, wall}) != 0, "past wall deadline blocked");
    f.engine.write64(storage + 8, 1'000'000'000);
    f.rejected([&] { static_cast<void>(f.call("_dispatch_walltime", {storage, 0})); }, "invalid timespec accepted");
    f.rejected([&] { static_cast<void>(f.call("_dispatch_walltime", {UINT64_MAX - 4, 0})); }, "overflowing timespec address accepted");
    require(f.call("_dispatch_semaphore_signal", {semaphore}) == 0 &&
        f.call("_dispatch_semaphore_wait", {semaphore, wall}) == 0, "timeout consumed a future permit or rejected an available one");
    static_cast<void>(f.call("_dispatch_release", {semaphore}));
}

void test_concurrent_waiters() {
    Fixture f;
    const auto semaphore = f.call("_dispatch_semaphore_create", {0});
    const auto wait = f.symbol("_dispatch_semaphore_wait");
    std::vector<std::unique_ptr<transmute::ReferenceEngine>> workers;
    std::vector<std::future<std::uint64_t>> results;
    struct StopOnFailure {
        alchemy::ProcessContext& process;
        int exceptions{std::uncaught_exceptions()};
        ~StopOnFailure() { if (std::uncaught_exceptions() > exceptions) process.request_exit(1); }
    } cleanup{f.process};
    for (unsigned index = 0; index < 4; ++index) {
        workers.push_back(f.engine.fork_thread(0, 0));
        auto* engine = workers.back().get();
        results.push_back(std::async(std::launch::async, [=] {
            const std::array arguments{semaphore, forever};
            return engine->invoke_callback(wait, arguments);
        }));
    }
    // Give launched engines an opportunity to enter their waits. The assertions
    // accept either scheduling order; both stored permits and queued wakes count.
    for (auto& result : results)
        require(result.wait_for(std::chrono::milliseconds(2)) == std::future_status::timeout,
            "unsignaled concurrent wait completed");
    // Signals can precede waiter entry; either ordering must deliver one permit.
    for (unsigned index = 0; index < 4; ++index)
        require(f.call("_dispatch_semaphore_signal", {semaphore}) <= 1, "invalid signal result");
    for (auto& result : results) {
        if (result.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
            f.process.request_exit(1);
            throw std::runtime_error("dispatch lost a wakeup");
        }
        require(result.get() == 0, "concurrent wait failed");
    }
    require(f.call("_dispatch_semaphore_wait", {semaphore, 0}) != 0, "concurrent waits consumed too few permits");
    static_cast<void>(f.call("_dispatch_release", {semaphore}));
}

void test_shutdown() {
    Fixture f;
    const auto semaphore = f.call("_dispatch_semaphore_create", {0});
    auto worker = f.engine.fork_thread(0, 0);
    const auto wait = f.symbol("_dispatch_semaphore_wait");
    auto result = std::async(std::launch::async, [&] {
        const std::array arguments{semaphore, forever};
        // Workers unwind with GuestThreadStop; their pthread wrapper ends them without an error.
        try { static_cast<void>(worker->invoke_callback(wait, arguments)); }
        catch (const alchemy::GuestThreadStop&) { return true; }
        catch (...) { return false; }
        return false;
    });
    f.process.request_exit(1);
    require(result.wait_for(std::chrono::seconds(2)) == std::future_status::ready && result.get(),
        "dispatch shutdown did not stop the blocked worker");
    static_cast<void>(f.call("_dispatch_release", {semaphore}));
}

struct QueueFixture {
    transmute::ReferenceEngine engine{transmute::ExecutionStart{0}};
    alchemy::ProcessContext process{engine, [](auto, auto bytes) -> alchemy::OutputResult { return {bytes.size(), 0}; }};
    alchemy::DarwinServices services{process};
    alchemy::DarwinThreads threads{process};
    QueueFixture() { alchemy::install_darwin_dispatch(process); }
    std::uint64_t call(std::string_view name, std::initializer_list<std::uint64_t> arguments = {}) {
        const auto value = process.resolve("/usr/lib/libSystem.B.dylib", name);
        require(value.has_value(), "missing dispatch function");
        return engine.invoke_callback(value->address, std::span(arguments.begin(), arguments.size()));
    }
    std::uint64_t string(std::string_view value) {
        const auto address = process.allocate_guest(value.size() + 1);
        engine.write_bytes(address, {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
        static_cast<transmute::GuestMemory&>(engine).write8(address + value.size(), 0);
        return address;
    }
};

void test_queues() {
    QueueFixture f;
    std::mutex gate;
    std::vector<std::uint64_t> order;
    std::vector<std::uint64_t> threads;
    const auto work = f.process.install_function("test_dispatch_work", [&](auto& s, auto&) {
        std::lock_guard lock(gate); order.push_back(s.x[0]); threads.push_back(f.process.current_thread()->identity); s.x[0] = 0;
    });
    const auto wait_for = [&](std::size_t count) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        for (;;) {
            { std::lock_guard lock(gate); if (order.size() >= count) return; }
            require(std::chrono::steady_clock::now() < until, "dispatch work did not run");
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    };
    const auto serial = f.call("_dispatch_queue_create", {f.string("test.serial"), 0});
    require(serial != 0, "serial queue");
    const auto label = f.call("_dispatch_queue_get_label", {serial});
    const auto bytes = f.engine.read_string(label, 1, 64);
    require(std::string(bytes.begin(), bytes.end()) == "test.serial", "queue label");
    for (std::uint64_t index = 0; index < 8; ++index) static_cast<void>(f.call("_dispatch_async_f", {serial, index, work}));
    wait_for(8);
    { std::lock_guard lock(gate);
      for (std::uint64_t index = 0; index < 8; ++index) require(order[index] == index, "serial queue reordered work");
      require(threads[0] != f.process.current_thread()->identity, "async work ran on the caller"); }
    static_cast<void>(f.call("_dispatch_sync_f", {serial, 100, work}));
    { std::lock_guard lock(gate); require(order.back() == 100 && threads.back() == f.process.current_thread()->identity, "dispatch_sync runs inline after queued work"); }
    const auto global = f.call("_dispatch_get_global_queue", {0, 0});
    require(global && f.call("_dispatch_get_global_queue", {0, 1}) == 0, "global queue flags");
    for (std::uint64_t index = 0; index < 16; ++index) static_cast<void>(f.call("_dispatch_async_f", {global, 1000 + index, work}));
    wait_for(25);
    const auto start = std::chrono::steady_clock::now();
    const auto when = f.call("_dispatch_time", {0, 20'000'000});
    static_cast<void>(f.call("_dispatch_after_f", {when, serial, 200, work}));
    wait_for(26);
    require(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(19), "dispatch_after fired early");
    // A global (compiler-emitted) block: Block_copy keeps it, its invoke receives the block.
    const auto block = f.process.allocate_guest(32), descriptor = f.process.allocate_guest(16);
    f.engine.write64(descriptor, 0); f.engine.write64(descriptor + 8, 32);
    f.engine.write64(block, f.process.resolve("/usr/lib/libSystem.B.dylib", "__NSConcreteGlobalBlock")->address);
    f.engine.write32(block + 8, 1U << 28); f.engine.write32(block + 12, 0); f.engine.write64(block + 16, work); f.engine.write64(block + 24, descriptor);
    static_cast<void>(f.call("_dispatch_async", {serial, block}));
    wait_for(27);
    { std::lock_guard lock(gate); require(order.back() == block, "block invoke did not receive the block"); }
    // The main queue runs only when the main thread drains it.
    const auto main_queue = f.process.resolve("/usr/lib/libSystem.B.dylib", "__dispatch_main_q")->address;
    static_cast<void>(f.call("_dispatch_async_f", {main_queue, 300, work}));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    { std::lock_guard lock(gate); require(order.back() != 300, "main queue work ran without a drain"); }
    static_cast<void>(f.call("__dispatch_main_queue_callback_4CF", {0}));
    { std::lock_guard lock(gate); require(order.back() == 300 && threads.back() == f.process.current_thread()->identity, "main queue drain"); }
    static_cast<void>(f.call("_dispatch_release", {serial}));
    f.threads.join_workers();
}
} // namespace

int main() {
    try {
        test_counting_and_ownership(); test_deadlines(); test_concurrent_waiters(); test_queues(); test_shutdown();
        std::cout << "Alchemy dispatch tests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
