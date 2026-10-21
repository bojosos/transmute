#include "alchemy/darwin_threads.hpp"

#include <array>
#include <chrono>
#include <future>
#include <thread>
#include <iostream>
#include <stdexcept>
#include <string_view>

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
    require(f.call("_pthread_join", {handle, 0}) == 22, "worker joined twice");
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
            f.engine.read64(data + 256) == 1024ULL * 1024,
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
        require(end - begin == large_stack, "worker did not receive its requested stack");
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
    require(f.call("_pthread_join", {handle, data + 8}) == 0 && f.engine.read64(data + 8) == large_stack,
            "large-stack worker result lost");
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
    require(finalizers == 1 && thread->tls_blocks.empty(), "thread cleanup incomplete");
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
    require(f.call("_pthread_cancel", {f.call("_pthread_self", {})}) == 78, "pthread_cancel is explicitly unsupported");
    require(f.call("_pthread_setcancelstate", {0, value}) == 0 && f.engine.read32(value) == 1 &&
        f.call("_pthread_setcancelstate", {1, value}) == 0 && f.engine.read32(value) == 0, "cancel state round trip");
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

int main() {
    try {
        test_mutex_keys_and_once();
        test_workers(false);
        test_workers(true);
        test_configurable_stacks(false);
        test_configurable_stacks(true);
        test_explicit_thread_exit();
        test_tlv_and_cleanup();
        test_guards_and_timed_condition();
        test_address_waits_and_context_release();
        test_attributes_names_and_stop();
        std::cout << "Alchemy Darwin thread tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
