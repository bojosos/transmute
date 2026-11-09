#include "alchemy/darwin_threads.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <thread>
#include <utility>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
constexpr std::string_view system_library = "/usr/lib/libSystem.B.dylib";
constexpr std::uint64_t data = 0x200000000ULL;
constexpr std::uint64_t code = 0x100000000ULL;
void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}
struct Fixture {
    transmute::ReferenceEngine engine{transmute::ExecutionStart{0}};
    alchemy::ProcessContext process{engine, [](auto, auto bytes) -> alchemy::OutputResult { return {bytes.size(), 0}; }};
    alchemy::DarwinThreads threads{process};
    explicit Fixture(std::uint64_t stack_size = 0)
        : engine(transmute::ExecutionStart{0, stack_size}) {
        engine.map_memory(data, 4096); engine.map_memory(code, 4096);
        engine.set_supervisor_call_handler([this](const auto& call, auto& state, auto& memory) {
            return process.syscall_extension(call, state, memory);
        });
    }
    std::uint64_t call(std::string_view symbol, std::initializer_list<std::uint64_t> arguments) {
        const auto exported = process.resolve(system_library, symbol);
        require(exported.has_value(), "pthread export missing");
        return engine.invoke_callback(exported->address, std::span(arguments.begin(), arguments.size()));
    }
};
void test_mutex_keys_and_once() {
    Fixture f;
    require(f.call("_pthread_mutex_lock", {data}) == 0, "static mutex lock failed");
    require(f.call("_pthread_mutex_trylock", {data}) == 16, "held mutex trylock failed");
    require(f.call("_pthread_mutex_destroy", {data}) == 16, "held mutex destroyed");
    require(f.call("_pthread_mutex_unlock", {data}) == 0, "mutex unlock failed");
    require(f.call("_pthread_mutex_unlock", {data}) == 1, "mutex ownership ignored");
    require(f.call("_pthread_mutex_destroy", {data}) == 0, "mutex destroy failed");
    require(f.call("_pthread_mutexattr_init", {data + 128}) == 0, "mutex attributes failed");
    require(f.call("_pthread_mutexattr_settype", {data + 128, 2}) == 0, "recursive attributes failed");
    require(f.call("_pthread_mutex_init", {data, data + 128}) == 0, "recursive init failed");
    require(f.call("_pthread_mutex_lock", {data}) == 0 && f.call("_pthread_mutex_lock", {data}) == 0, "recursive lock failed");
    require(f.call("_pthread_mutex_unlock", {data}) == 0 && f.call("_pthread_mutex_unlock", {data}) == 0, "recursive unlock failed");
    require(f.call("_pthread_key_create", {data + 256, 0}) == 0, "key create failed");
    const auto key = f.engine.read64(data + 256);
    require(f.call("_pthread_setspecific", {key, 0x123456789abcdef0ULL}) == 0, "key set failed");
    require(f.call("_pthread_getspecific", {key}) == 0x123456789abcdef0ULL, "key truncated");
    require(f.call("_pthread_key_delete", {key}) == 0 && f.call("_pthread_getspecific", {key}) == 0, "deleted key survived");
    unsigned initialized = 0;
    const auto initialize = f.engine.register_guest_call([&](auto&, auto&) { ++initialized; });
    require(f.call("_pthread_once", {data + 512, initialize}) == 0 &&
            f.call("_pthread_once", {data + 512, initialize}) == 0 && initialized == 1, "once ran twice");
    require(f.call("_pthread_mutex_lock", {0}) == 22, "invalid mutex accepted");
    // PTHREAD_RECURSIVE_MUTEX_INITIALIZER / PTHREAD_ERRORCHECK_MUTEX_INITIALIZER: their signatures choose the type.
    f.engine.write64(data + 640, 0x32aaaba2);
    require(f.call("_pthread_mutex_lock", {data + 640}) == 0 && f.call("_pthread_mutex_lock", {data + 640}) == 0 &&
            f.call("_pthread_mutex_unlock", {data + 640}) == 0 && f.call("_pthread_mutex_unlock", {data + 640}) == 0,
            "a static recursive mutex is not recursive");
    f.engine.write64(data + 704, 0x32aaaba1);
    require(f.call("_pthread_mutex_lock", {data + 704}) == 0 && f.call("_pthread_mutex_lock", {data + 704}) == 11 &&
            f.call("_pthread_mutex_unlock", {data + 704}) == 0, "a static errorcheck mutex does not report EDEADLK");
}
// pthread_introspection_hook_install: the hook sees CREATE, START, TERMINATE and DESTROY (at join) with the
// thread's pthread_t, as libpthread calls it; it answers the previous hook.
void test_introspection_hook() {
    Fixture f;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> events;
    std::mutex events_mutex;
    const auto hook = f.engine.register_guest_call([&](auto& state, auto&) {
        std::lock_guard lock(events_mutex);
        events.emplace_back(state.x[0], state.x[1]);
    });
    require(f.call("_pthread_introspection_hook_install", {hook}) == 0, "the first hook has no previous one");
    f.engine.write32(code, 0xd2824680); // mov x0, #0x1234
    f.engine.write32(code + 4, 0xd65f03c0); // ret
    f.engine.set_memory_access(code, true, false, true);
    require(f.call("_pthread_create_from_mach_thread", {data, 0, code, 0}) == 0, "pthread_create_from_mach_thread failed");
    const auto handle = f.engine.read64(data);
    require(f.call("_pthread_join", {handle, data + 8}) == 0 && f.engine.read64(data + 8) == 0x1234, "worker result wrong");
    require(f.call("_pthread_introspection_hook_install", {0}) == hook, "the previous hook was not answered");
    std::lock_guard lock(events_mutex);
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> expected{{1, handle}, {2, handle}, {3, handle}, {4, handle}};
    require(events == expected, "introspection events: " + std::to_string(events.size()));
}
void test_workers(bool jit) {
    Fixture f;
    f.engine.set_jit_enabled(jit);
    f.engine.set_jit_profiling(jit);
    f.process.jit_stats = jit;
    f.engine.write32(code, 0xd2824680); // mov x0, #0x1234
    f.engine.write32(code + 4, 0xd65f03c0); // ret
    f.engine.set_memory_access(code, true, false, true);
    require(f.call("_pthread_create", {data, 0, code, 0}) == 0, "worker create failed");
    const auto handle = f.engine.read64(data);
    require(f.call("_pthread_join", {handle, data + 8}) == 0 && f.engine.read64(data + 8) == 0x1234, "worker result wrong");
    require(f.call("_pthread_join", {handle, 0}) == 3, "a joined thread is gone (ESRCH, as libpthread)");
    require(f.call("_pthread_self", {}) != handle, "thread identity shared");
    require(f.call("_pthread_create", {data, 0, code, 0}) == 0, "detached create failed");
    const auto detached = f.engine.read64(data);
    require(f.call("_pthread_detach", {detached}) == 0 && f.call("_pthread_join", {detached, 0}) == 22, "detach ignored");
    f.threads.join_workers();
    if (jit) require(f.process.worker_execution().native_guest_instructions != 0, "JIT worker code did not execute");
}
void test_configurable_stacks(bool jit) {
    constexpr std::uint64_t large_stack = 16ULL * 1024 * 1024;
    Fixture f(large_stack);
    f.engine.set_jit_enabled(jit);
    f.process.jit_stats = jit;
    require(f.call("_pthread_attr_init", {data + 128}) == 0, "stack attributes failed");
    require(f.call("_pthread_attr_getstacksize", {data + 128, data + 256}) == 0 &&
            f.engine.read64(data + 256) == 512ULL * 1024,
            "worker default incorrectly inherited the main stack size");
    require(f.call("_pthread_attr_setstacksize", {data + 128, large_stack}) == 0 &&
            f.call("_pthread_attr_getstacksize", {data + 128, data + 256}) == 0 &&
            f.engine.read64(data + 256) == large_stack, "explicit worker stack attribute lost");
    require(f.call("_pthread_attr_setstacksize", {data + 128, 1024ULL * 1024 * 1024 + 16384}) == 22 &&
            f.call("_pthread_attr_getstacksize", {data + 128, data + 256}) == 0 &&
            f.engine.read64(data + 256) == large_stack,
            "oversized worker stack was accepted or changed the valid attribute");
    const auto inspect = f.engine.register_guest_call([&](auto& state, auto& memory) {
        auto* const current = transmute::ReferenceEngine::current();
        require(current && current != &f.engine, "stack callback ran on main engine");
        const auto [begin, end] = current->stack_bounds();
        // arm64 libpthread adds PTHREAD_T_OFFSET (12 KiB) to the stack it allocates.
        require(end - begin >= large_stack + 12 * 1024, "worker did not receive its requested stack");
        memory.write64(begin + 64, 27);
        require(memory.read64(begin + 64) == 27, "deep worker stack was inaccessible");
        state.x[0] = end - begin;
    });
    f.engine.write32(code, 0x58000090); // ldr x16, literal at +16
    f.engine.write32(code + 4, 0xd61f0200); // br x16
    f.engine.write64(code + 16, inspect);
    f.engine.set_memory_access(code, true, false, true);
    require(f.call("_pthread_create", {data, data + 128, code, 0}) == 0, "large-stack worker creation failed");
    const auto handle = f.engine.read64(data);
    require(f.call("_pthread_get_stacksize_np", {handle}) == large_stack + 12 * 1024,
            "pthread_get_stacksize_np is not the attribute's size plus PTHREAD_T_OFFSET");
    require(f.call("_pthread_join", {handle, data + 8}) == 0 && f.engine.read64(data + 8) >= large_stack + 12 * 1024,
            "large-stack worker result lost");
    // The main thread reports the size LC_MAIN asked for, not the engine's rounded allocation.
    f.process.main_stack_size = large_stack - 48 * 1024;
    require(f.call("_pthread_get_stacksize_np", {f.call("_pthread_self", {})}) == large_stack - 48 * 1024,
            "the main thread's stack size is not LC_MAIN's");
    require(f.call("_pthread_attr_destroy", {data + 128}) == 0, "stack attribute destruction failed");
    if (jit) require(f.process.worker_execution().native_guest_instructions != 0,
                     "large-stack worker did not execute translated instructions");
}
void test_explicit_thread_exit() {
    Fixture f;
    const auto exit = f.process.resolve(system_library, "_pthread_exit");
    require(exit.has_value(), "pthread_exit export missing");
    const auto push = f.process.resolve(system_library, "___alchemy_pthread_cleanup_push");
    require(push.has_value(), "cleanup helper missing");
    unsigned cleanups = 0;
    const auto cleanup = f.engine.register_guest_call([&](auto& state, auto&) {
        require(state.x[0] == 89, "exit cleanup argument wrong"); ++cleanups;
    });
    f.engine.write32(code, 0x58000200); // ldr x0, literal at +64
    f.engine.write32(code + 4, 0x58000221); // ldr x1, literal at +72
    f.engine.write32(code + 8, 0x58000242); // ldr x2, literal at +80
    f.engine.write32(code + 12, 0x58000263); // ldr x3, literal at +88
    f.engine.write32(code + 16, 0xd63f0060); // blr x3: register cleanup
    f.engine.write32(code + 20, 0x58000260); // ldr x0, literal at +96
    f.engine.write32(code + 24, 0x58000281); // ldr x1, literal at +104
    f.engine.write32(code + 28, 0xd61f0020); // br x1: pthread_exit
    f.engine.write64(code + 64, data + 128);
    f.engine.write64(code + 72, cleanup);
    f.engine.write64(code + 80, 89);
    f.engine.write64(code + 88, push->address);
    f.engine.write64(code + 96, 0x123456789abcdef0ULL);
    f.engine.write64(code + 104, exit->address);
    f.engine.set_memory_access(code, true, false, true);
    require(f.call("_pthread_create", {data, 0, code, 0}) == 0, "exit worker create failed");
    require(f.call("_pthread_join", {f.engine.read64(data), data + 8}) == 0, "exit worker join failed");
    require(f.engine.read64(data + 8) == 0x123456789abcdef0ULL, "pthread_exit truncated pointer result");
    require(cleanups == 1, "pthread_exit did not execute its cleanup handler");
    require(!f.process.requested_exit(), "pthread_exit terminated the process");
}
// Deferred cancellation: a worker looping on pthread_testcancel, and one waiting in pthread_cond_wait, end as
// pthread_exit(PTHREAD_CANCELED) at the request, running their cleanup handlers; the waiter holds the mutex again.
void test_cancellation() {
    Fixture f;
    const auto address = [&](std::string_view name) {
        const auto exported = f.process.resolve(system_library, name);
        require(exported.has_value(), "pthread export missing");
        return exported->address;
    };
    static std::atomic<std::uint64_t> cleaned{0};
    const auto cleanup = f.engine.register_guest_call([](auto& state, auto&) { cleaned += state.x[0]; });
    // ldr x<reg>, the literal at `literal`; blr x16; b back by `instructions`.
    const auto load = [&](std::uint64_t at, std::uint32_t reg, std::uint64_t literal) {
        f.engine.write32(at, 0x58000000U | static_cast<std::uint32_t>(((literal - at) / 4) << 5) | reg);
    };
    constexpr std::uint32_t call_x16 = 0xd63f0200;
    const auto back = [](std::uint32_t instructions) { return 0x14000000U | ((0U - instructions) & 0x03ffffffU); };
    const auto mutex = data + 1600, condition = data + 1700;
    // Worker 1 at code: push cleanup(5), then pthread_testcancel forever.
    const auto pool = code + 2048;
    f.engine.write64(pool, data + 128); f.engine.write64(pool + 8, cleanup); f.engine.write64(pool + 16, 5);
    f.engine.write64(pool + 24, address("___alchemy_pthread_cleanup_push")); f.engine.write64(pool + 32, address("_pthread_testcancel"));
    load(code, 0, pool); load(code + 4, 1, pool + 8); load(code + 8, 2, pool + 16); load(code + 12, 16, pool + 24);
    f.engine.write32(code + 16, call_x16);
    load(code + 20, 16, pool + 32); f.engine.write32(code + 24, call_x16); f.engine.write32(code + 28, back(2));
    // Worker 2 at code + 512: lock the mutex, push cleanup(7), then pthread_cond_wait forever.
    const auto waiter = code + 512, pool2 = code + 2560;
    f.engine.write64(pool2, mutex); f.engine.write64(pool2 + 8, address("_pthread_mutex_lock"));
    f.engine.write64(pool2 + 16, data + 160); f.engine.write64(pool2 + 24, cleanup); f.engine.write64(pool2 + 32, 7);
    f.engine.write64(pool2 + 40, address("___alchemy_pthread_cleanup_push"));
    f.engine.write64(pool2 + 48, condition); f.engine.write64(pool2 + 56, address("_pthread_cond_wait"));
    load(waiter, 0, pool2); load(waiter + 4, 16, pool2 + 8); f.engine.write32(waiter + 8, call_x16);
    load(waiter + 12, 0, pool2 + 16); load(waiter + 16, 1, pool2 + 24); load(waiter + 20, 2, pool2 + 32);
    load(waiter + 24, 16, pool2 + 40); f.engine.write32(waiter + 28, call_x16);
    load(waiter + 32, 0, pool2 + 48); load(waiter + 36, 1, pool2); load(waiter + 40, 16, pool2 + 56);
    f.engine.write32(waiter + 44, call_x16); f.engine.write32(waiter + 48, back(4));
    f.engine.set_memory_access(code, true, false, true);
    require(f.call("_pthread_mutex_init", {mutex, 0}) == 0 && f.call("_pthread_cond_init", {condition, 0}) == 0, "cancel objects");

    require(f.call("_pthread_create", {data, 0, code, 0}) == 0, "testcancel worker create");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    require(f.call("_pthread_cancel", {f.engine.read64(data)}) == 0, "pthread_cancel a worker");
    require(f.call("_pthread_join", {f.engine.read64(data), data + 8}) == 0 && f.engine.read64(data + 8) == 1,
        "a cancelled worker's result is PTHREAD_CANCELED");
    require(cleaned == 5, "the cancelled worker ran its cleanup handler");

    require(f.call("_pthread_create", {data + 16, 0, waiter, 0}) == 0, "waiting worker create");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    require(f.call("_pthread_cancel", {f.engine.read64(data + 16)}) == 0, "pthread_cancel a waiting worker");
    require(f.call("_pthread_join", {f.engine.read64(data + 16), data + 8}) == 0 && f.engine.read64(data + 8) == 1,
        "a worker cancelled in pthread_cond_wait ends with PTHREAD_CANCELED");
    require(cleaned == 12, "the waiting worker ran its cleanup handler");
    require(f.call("_pthread_mutex_trylock", {mutex}) == 16, "it took the mutex back before its cleanup handlers");
}

// The Swift runtime's reserved keys (__PTK_FRAMEWORK_SWIFT_KEY0-9, 100-109) work without pthread_key_init_np, and
// pthread_key_create never hands them out. As libpthread, the program has all 512 keys (PTHREAD_KEYS_MAX), numbered
// from 256: the reserved ones are not counted.
void test_reserved_swift_keys() {
    Fixture f;
    require(f.call("_pthread_setspecific", {103, 0x1234}) == 0 && f.call("_pthread_getspecific", {103}) == 0x1234 &&
            f.call("_pthread_setspecific", {109, 7}) == 0 && f.call("_pthread_getspecific", {109}) == 7, "reserved keys 100-109");
    for (int index = 0; index < 512; ++index) {
        require(f.call("_pthread_key_create", {data + 256, 0}) == 0, "key create failed before 512 keys");
        const auto key = f.engine.read64(data + 256);
        require(key >= 256, "pthread_key_create handed out a key below 256");
    }
    require(f.call("_pthread_key_create", {data + 256, 0}) == 35, "the 513th key is EAGAIN");
    require(f.call("_pthread_key_init_np", {120, 0}) == 0 && f.call("_pthread_key_init_np", {256, 0}) == 22,
            "pthread_key_init_np takes reserved keys only");
}

// os_unfair_recursive_lock {lock word, count}: the owner takes it again, another thread cannot until the last unlock.
void test_recursive_unfair_lock() {
    Fixture f;
    const auto lock = data + 1800;
    f.engine.write64(lock, 0);
    static_cast<void>(f.call("_os_unfair_recursive_lock_lock_with_options", {lock, 0}));
    static_cast<void>(f.call("_os_unfair_recursive_lock_lock_with_options", {lock, 0x10000}));
    require(f.engine.read32(lock + 4) == 1 && f.call("_os_unfair_recursive_lock_trylock", {lock}) == 1 && f.engine.read32(lock + 4) == 2,
        "the owner takes the recursive lock again");
    const auto trylock = f.process.resolve(system_library, "_os_unfair_recursive_lock_trylock")->address;
    auto worker = f.engine.fork_thread(0, 0);
    const auto elsewhere = [&] {
        return std::async(std::launch::async, [&] { return worker->invoke_callback(trylock, std::array{lock}); }).get();
    };
    require(elsewhere() == 0, "another thread cannot take it while it is held");
    for (int unlock = 0; unlock < 3; ++unlock) static_cast<void>(f.call("_os_unfair_recursive_lock_unlock", {lock}));
    require(f.engine.read32(lock + 4) == 0 && elsewhere() == 1, "the last unlock releases it");
}

void test_tlv_and_cleanup() {
    Fixture f;
    alchemy::TlsTemplate tls;
    tls.module_id = 17;
    tls.initial_data = {27, 0, 0, 0};
    tls.zero_fill_size = 12;
    tls.descriptors = {data};
    alchemy::ImageRuntimeInfo image;
    image.module_id = 17;
    image.tls.push_back(tls);
    f.process.publish_image(image);
    f.engine.write64(data + 8, 17);
    f.engine.write64(data + 16, 0);
    const auto value = f.call("__tlv_bootstrap", {data});
    require(f.engine.read32(value) == 27 && f.engine.read64(value + 8) == 0, "TLV initialization wrong");
    require(f.call("__tlv_bootstrap", {data}) == value, "TLV allocated twice");
    unsigned finalizers = 0;
    const auto finalize = f.engine.register_guest_call([&](auto& state, auto&) {
        require(state.x[0] == 1234, "thread finalizer argument wrong"); ++finalizers;
    });
    require(f.call("___cxa_thread_atexit", {finalize, 1234, 0}) == 0, "thread finalizer registration failed");
    std::vector<std::uint64_t> cleanup_order;
    const auto cleanup = f.engine.register_guest_call([&](auto& state, auto&) { cleanup_order.push_back(state.x[0]); });
    require(f.call("___alchemy_pthread_cleanup_push", {data + 128, cleanup, 27}) == 0, "cleanup push failed");
    require(f.call("___alchemy_pthread_cleanup_push", {data + 160, cleanup, 42}) == 0, "nested cleanup push failed");
    require(f.call("___alchemy_pthread_cleanup_pop", {data + 160, 1}) == 0 && cleanup_order == std::vector<std::uint64_t>{42}, "cleanup pop execution wrong");
    require(f.call("___alchemy_pthread_cleanup_push", {data + 160, cleanup, 43}) == 0, "cleanup repush failed");
    auto thread = f.process.thread(f.engine);
    f.threads.cleanup_thread(*thread);
    // The main thread's thread-local storage outlives its cleanup (exit runs image terminators and atexit
    // handlers after it, which may still read a thread_local), as on Darwin.
    require(&f.engine == &f.process.main_engine(), "the fixture runs on the main engine");
    require(finalizers == 1 && !thread->tls_blocks.empty() && f.engine.read32(value) == 27, "main thread cleanup released its TLS");
    require(cleanup_order == std::vector<std::uint64_t>({42, 43, 27}), "pthread cleanup order wrong");
}
void test_guards_and_timed_condition() {
    Fixture f;
    require(f.call("___cxa_guard_acquire", {data}) == 1, "guard acquire failed");
    require(f.call("___cxa_guard_abort", {data}) == 0, "guard abort failed");
    require(f.call("___cxa_guard_acquire", {data}) == 1, "aborted guard was committed");
    require(f.call("___cxa_guard_release", {data}) == 0 && f.call("___cxa_guard_acquire", {data}) == 0, "guard commit failed");
    require(f.call("_pthread_mutex_lock", {data + 128}) == 0, "condition mutex failed");
    f.engine.write64(data + 512, 0); f.engine.write64(data + 520, 0);
    require(f.call("_pthread_cond_timedwait", {data + 256, data + 128, data + 512}) == 60, "expired condition deadline ignored");
    require(f.call("_pthread_mutex_unlock", {data + 128}) == 0, "condition failed to reacquire mutex");
    require(f.call("_pthread_mutex_lock", {data + 128}) == 0, "negative deadline mutex failed");
    f.engine.write64(data + 512, UINT64_MAX);
    const auto past_result = f.call("_pthread_cond_timedwait", {data + 256, data + 128, data + 512});
    require(past_result == 60, "past signed deadline returned " + std::to_string(past_result));
    require(f.call("_pthread_mutex_unlock", {data + 128}) == 0, "negative deadline lost mutex ownership");
    require(f.call("_pthread_rwlock_wrlock", {data + 768}) == 0, "rwlock writer failed");
    require(f.call("_pthread_rwlock_tryrdlock", {data + 768}) == 16 && f.call("_pthread_rwlock_trywrlock", {data + 768}) == 16, "rwlock busy trylock error wrong");
    require(f.call("_pthread_rwlock_unlock", {data + 768}) == 0, "rwlock writer unlock failed");
}
void test_address_waits_and_context_release() {
    Fixture f;
    const auto original = f.process.thread(f.engine);
    f.process.set_errno(13);
    require(f.call("___ulock_wait", {0x01000001, 0, 0, 1}) == static_cast<std::uint64_t>(-14LL), "ulock no-errno invalid address result wrong");
    require(f.engine.read32(original->errno_address) == 13, "ulock no-errno changed errno");
    require(f.call("___ulock_wait", {0x01000001, data + 1, 0, 1}) == static_cast<std::uint64_t>(-22LL), "ulock unaligned address error wrong");
    require(f.call("___ulock_wake", {0x01000001, data, 0}) == static_cast<std::uint64_t>(-2LL), "ulock absent queue accepted");
    f.engine.write32(data, 27);
    require(f.call("___ulock_wait", {1, data, 28, 1}) == 0, "ulock mismatch blocked");
    require(f.call("___ulock_wait", {0x01000001, data, 27, 1}) == static_cast<std::uint64_t>(-60LL), "ulock deadline ignored");
    require(f.engine.read32(original->errno_address) == 13, "ulock no-errno timeout changed errno");
    require(f.call("___ulock_wait", {1, data, 27, 1}) == UINT64_MAX && f.engine.read32(original->errno_address) == 60, "ulock errno timeout wrong");
    auto worker = f.engine.fork_thread(code, 0);
    auto previous = f.process.thread(*worker);
    previous->key_values[17] = 42;
    f.engine.write32(previous->errno_address, 22);
    f.process.release_thread(*worker);
    auto replacement = f.process.thread(*worker);
    require(previous->identity != replacement->identity && replacement->key_values.empty(), "released worker context was reused");
    require(f.engine.read32(replacement->errno_address) == 0, "released worker errno was reused");
    f.process.release_thread(*worker);
}
}
void test_attributes_names_and_stop() {
    Fixture f;
    const auto attribute = data + 256, condition = data + 512, value = data + 1024, name = data + 1100, buffer = data + 1200;
    require(f.call("_pthread_condattr_init", {attribute}) == 0 && f.call("_pthread_condattr_setpshared", {attribute, 1}) == 0, "condattr pshared");
    require(f.call("_pthread_condattr_getpshared", {attribute, value}) == 0 && f.engine.read32(value) == 1, "condattr getpshared");
    require(f.call("_pthread_condattr_setpshared", {attribute, 3}) == 22, "invalid pshared accepted");
    require(f.call("_pthread_cond_init", {condition, attribute}) == 0 && f.call("_pthread_cond_destroy", {condition}) == 0, "cond_init with attributes");
    require(f.call("_pthread_cond_init", {condition, data + 2000}) == 22, "unknown condattr accepted");
    require(f.call("_pthread_condattr_destroy", {attribute}) == 0, "condattr destroy");
    require(f.call("_pthread_mutexattr_init", {attribute}) == 0 && f.call("_pthread_mutexattr_setpshared", {attribute, 2}) == 0 &&
        f.call("_pthread_mutexattr_getpshared", {attribute, value}) == 0 && f.engine.read32(value) == 2, "mutexattr pshared");
    f.engine.write_bytes(name, std::span(reinterpret_cast<const std::uint8_t*>("worker"), 7));
    require(f.call("_pthread_setname_np", {name}) == 0, "setname");
    require(f.call("_pthread_getname_np", {f.call("_pthread_self", {}), buffer, 64}) == 0 &&
        f.engine.read64(buffer) == 0x00007265'6b726f77ULL, "getname");
    require(f.call("_pthread_set_qos_class_self_np", {0x19, 0}) == 0 && f.call("_pthread_set_qos_class_self_np", {0x77, 0}) == 22, "qos classes");
    require(f.call("_pthread_setcancelstate", {0, value}) == 0 && f.engine.read32(value) == 1 &&
        f.call("_pthread_setcancelstate", {1, value}) == 0 && f.engine.read32(value) == 0, "cancel state round trip");
    // With cancellation disabled a request waits: testcancel and join return normally.
    require(f.call("_pthread_setcancelstate", {0, 0}) == 0 && f.call("_pthread_cancel", {f.call("_pthread_self", {})}) == 0 &&
        f.call("_pthread_testcancel", {}) == 0 && !f.process.thread(f.engine)->exit_value, "a disabled request waits");
    require(f.call("_pthread_cancel", {0x7777000}) == 3, "pthread_cancel of an unknown thread: ESRCH");
    require(f.call("_pthread_atfork", {0, 0, 0}) == 0, "pthread_atfork records handlers");
    // A worker blocked in a libSystem wait unwinds cooperatively at process exit.
    require(f.call("_pthread_mutex_lock", {data + 1600}) == 0, "stop mutex lock");
    auto worker = f.engine.fork_thread(0, 0);
    const auto lock = f.process.resolve(system_library, "_pthread_mutex_lock")->address;
    auto result = std::async(std::launch::async, [&] {
        const std::array arguments{data + 1600};
        try { static_cast<void>(worker->invoke_callback(lock, arguments)); }
        catch (const alchemy::GuestThreadStop&) { return true; }
        catch (...) { return false; }
        return false;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    f.process.request_exit(0);
    require(result.wait_for(std::chrono::seconds(2)) == std::future_status::ready && result.get(), "blocked worker did not stop cooperatively");
}

// pthread_cond_signal wakes a thread blocked when it was called: a thread that starts
// waiting afterwards (here the signaller itself) cannot take that wakeup. A NULL
// pthread_cond_timedwait deadline is an untimed wait.
void test_condition_wakeup_order() {
    Fixture f;
    const auto address = [&](std::string_view name) {
        const auto exported = f.process.resolve(system_library, name);
        require(exported.has_value(), "pthread export missing");
        return exported->address;
    };
    static std::atomic<int> woke{0};
    woke = 0;
    const auto record = f.engine.register_guest_call([](auto&, auto&) { ++woke; });
    const auto load = [&](std::uint64_t at, std::uint32_t reg, std::uint64_t literal) {
        f.engine.write32(at, 0x58000000U | static_cast<std::uint32_t>(((literal - at) / 4) << 5) | reg);
    };
    constexpr std::uint32_t call_x16 = 0xd63f0200;
    const auto mutex = data + 1600, condition = data + 1700;
    // The worker: lock; pthread_cond_wait (or pthread_cond_timedwait with a NULL deadline); record; unlock; pthread_exit(0).
    const auto worker = code + 1024, pool = code + 3072;
    f.engine.write64(pool, mutex); f.engine.write64(pool + 8, address("_pthread_mutex_lock"));
    f.engine.write64(pool + 16, condition); f.engine.write64(pool + 24, address("_pthread_cond_wait"));
    f.engine.write64(pool + 32, record); f.engine.write64(pool + 40, address("_pthread_mutex_unlock"));
    f.engine.write64(pool + 48, 0); f.engine.write64(pool + 56, address("_pthread_exit"));
    f.engine.write64(pool + 64, address("_pthread_cond_timedwait"));
    // Two copies: at `worker` waiting with pthread_cond_wait (pool + 24), at `worker + 128` with
    // pthread_cond_timedwait and a NULL deadline (pool + 64).
    for (const auto [at, wait] : {std::pair{worker, pool + 24}, std::pair{worker + 128, pool + 64}}) {
        load(at, 0, pool); load(at + 4, 16, pool + 8); f.engine.write32(at + 8, call_x16);
        load(at + 12, 0, pool + 16); load(at + 16, 1, pool); load(at + 20, 2, pool + 48); load(at + 24, 16, wait);
        f.engine.write32(at + 28, call_x16);
        load(at + 32, 16, pool + 32); f.engine.write32(at + 36, call_x16);
        load(at + 40, 0, pool); load(at + 44, 16, pool + 40); f.engine.write32(at + 48, call_x16);
        load(at + 52, 0, pool + 48); load(at + 56, 16, pool + 56); f.engine.write32(at + 60, call_x16);
    }
    f.engine.set_memory_access(code, true, false, true);
    require(f.call("_pthread_mutex_init", {mutex, 0}) == 0 && f.call("_pthread_cond_init", {condition, 0}) == 0, "condition objects");
    for (const bool timed : {false, true}) {
        woke = 0;
        require(f.call("_pthread_create", {data, 0, timed ? worker + 128 : worker, 0}) == 0, "waiting worker create");
        const auto deadline_at = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        // The worker holds the mutex until it waits, so once this lock succeeds after a pause it is waiting.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        require(f.call("_pthread_mutex_lock", {mutex}) == 0, "signaller lock");
        require(f.call("_pthread_cond_signal", {condition}) == 0, "signal");
        const auto now = std::chrono::system_clock::now() + std::chrono::milliseconds(300);
        const auto since = now.time_since_epoch();
        f.engine.write64(data + 512, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(since).count()));
        f.engine.write64(data + 520, static_cast<std::uint64_t>((std::chrono::duration_cast<std::chrono::nanoseconds>(since) % std::chrono::seconds(1)).count()));
        require(f.call("_pthread_cond_timedwait", {condition, mutex, data + 512}) == 60, "a later waiter took the signal");
        require(f.call("_pthread_mutex_unlock", {mutex}) == 0, "signaller unlock");
        while (woke == 0 && std::chrono::steady_clock::now() < deadline_at) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        require(woke == 1, timed ? "a NULL-deadline waiter missed its signal" : "the signalled waiter did not wake");
        require(f.call("_pthread_join", {f.engine.read64(data), 0}) == 0, "waiting worker join");
    }
}

// Joined threads and finished detached threads leave the thread table: more threads than
// its 1024 limit can be made over the process's life.
void test_thread_reaping() {
    Fixture f;
    f.engine.write32(code, 0xd2824680); // mov x0, #0x1234
    f.engine.write32(code + 4, 0xd65f03c0); // ret
    f.engine.set_memory_access(code, true, false, true);
    for (int index = 0; index < 1100; ++index) {
        require(f.call("_pthread_create", {data, 0, code, 0}) == 0, "joinable create " + std::to_string(index));
        require(f.call("_pthread_join", {f.engine.read64(data), 0}) == 0, "join");
    }
    for (int batch = 0; batch < 40; ++batch) {
        for (int index = 0; index < 32; ++index) {
            require(f.call("_pthread_create", {data, 0, code, 0}) == 0,
                    "detached create " + std::to_string(batch * 32 + index));
            require(f.call("_pthread_detach", {f.engine.read64(data)}) == 0, "detach");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    f.threads.join_workers();
}

int main() {
    try {
        const auto stage = [](std::string_view name) { std::cout << "[stage] " << name << std::endl; };
        stage("test_mutex_keys_and_once()"); test_mutex_keys_and_once();
        stage("test_condition_wakeup_order()"); test_condition_wakeup_order();
        stage("test_introspection_hook()"); test_introspection_hook();
        stage("test_thread_reaping()"); test_thread_reaping();
        stage("test_workers(false)"); test_workers(false);
        stage("test_workers(true)"); test_workers(true);
        stage("test_configurable_stacks(false)"); test_configurable_stacks(false);
        stage("test_configurable_stacks(true)"); test_configurable_stacks(true);
        stage("test_explicit_thread_exit()"); test_explicit_thread_exit();
        stage("test_cancellation()"); test_cancellation();
        stage("test_recursive_unfair_lock()"); test_recursive_unfair_lock();
        stage("test_reserved_swift_keys()"); test_reserved_swift_keys();
        stage("test_tlv_and_cleanup()"); test_tlv_and_cleanup();
        stage("test_guards_and_timed_condition()"); test_guards_and_timed_condition();
        stage("test_address_waits_and_context_release()"); test_address_waits_and_context_release();
        stage("test_attributes_names_and_stop()"); test_attributes_names_and_stop();
        std::cout << "Alchemy Darwin thread tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
