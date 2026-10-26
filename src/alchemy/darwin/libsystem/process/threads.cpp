#include "alchemy/darwin_threads.hpp"
#include "alchemy/engine_adapter.hpp"
#include "alchemy/shared_wait.hpp"
#include "transmute/jit.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <sstream>
#include <thread>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>
#include <optional>
#include <string>

namespace alchemy {
namespace {
constexpr std::uint32_t eagain = 35;
constexpr std::uint32_t esrch = 3;
constexpr std::uint32_t ebusy = 16;
constexpr std::uint32_t einval = 22;
constexpr std::uint32_t edeadlk = 11;
constexpr std::uint32_t eperm = 1;
constexpr std::uint32_t etimedout = 60;
constexpr std::uint32_t ecanceled = 89;
constexpr std::size_t mutex_size = 64;
constexpr std::size_t cond_size = 48;
constexpr std::size_t rwlock_size = 200;
constexpr std::size_t thread_object_size = 8192;
constexpr auto poll_period = std::chrono::milliseconds(10);

bool accessible(transmute::GuestMemory& memory, std::uint64_t address,
                std::uint64_t size, bool writable = true) {
    if (address == 0 || size > UINT64_MAX - address) return false;
    while (size != 0) {
        const auto region = memory.query_region(address);
        if (!region || !region->readable || (writable && !region->writable) ||
            address < region->base || address - region->base >= region->size) return false;
        const auto count = std::min(size, region->size - (address - region->base));
        address += count;
        size -= count;
    }
    return true;
}
}

struct DarwinThreads::Impl : std::enable_shared_from_this<Impl> {
    struct Worker {
        std::uint64_t handle{};
        std::shared_ptr<ThreadContext> context;
        std::unique_ptr<transmute::ReferenceEngine> engine;
        std::thread host;
        std::mutex mutex;
        std::condition_variable changed;
        bool detached{}, joining{}, joined{}, finished{};
        std::uint64_t result{};
        std::exception_ptr error;
    };
    struct Mutex {
        std::mutex gate;
        std::condition_variable changed;
        std::uint64_t owner{};
        unsigned depth{};
        unsigned type{};
        unsigned waiters{};
    };
    struct Condition {
        std::mutex gate;
        std::condition_variable changed;
        std::uint64_t generation{};
        std::uint64_t tickets{};
        unsigned waiters{};
    };
    struct AddressWait : Condition {
        std::uint32_t operation{};
    };
    struct Rwlock {
        std::mutex gate;
        std::condition_variable changed;
        std::map<std::uint64_t, unsigned> readers;
        std::uint64_t writer{};
        unsigned waiting_writers{};
    };
    struct Once {
        std::mutex gate;
        std::condition_variable changed;
        std::uint64_t owner{};
        bool complete{};
    };
    struct Guard {
        std::mutex gate;
        std::condition_variable changed;
        std::uint64_t owner{};
        bool complete{};
    };
    struct ThreadAttribute { std::uint64_t stack_size{}; bool detached{}; };

    explicit Impl(ProcessContext& value) : process(value) {}
    ProcessContext& process;
    std::mutex metadata;
    std::map<std::uint64_t, std::shared_ptr<Worker>> workers;
    std::map<std::uint64_t, std::uint64_t> handles;
    std::map<std::uint64_t, unsigned> attributes;
    std::map<std::uint64_t, ThreadAttribute> thread_attributes;
    // Guest synchronization objects by address (every pthread_mutex_lock and
    // unlock looks one up): open-addressing tables of shared_ptr values.
    template <class T> using ObjectTable = hash_map<std::uint64_t, std::shared_ptr<T>, address_hash>;
    ObjectTable<Mutex> mutexes;
    ObjectTable<Condition> conditions;
    ObjectTable<AddressWait> address_waits;
    ObjectTable<Rwlock> rwlocks;
    ObjectTable<Once> once;
    ObjectTable<Guard> guards;
    std::map<std::uint64_t, std::uint64_t> keys;
    std::map<std::uint64_t, unsigned> mutex_shared, condition_attributes;
    std::map<std::uint64_t, std::string> names;
    std::map<std::uint64_t, std::pair<unsigned, unsigned>> cancel_modes;
    std::vector<std::array<std::uint64_t, 3>> fork_handlers;
    std::uint64_t next_key{1};
    std::uint64_t worker_generation{};
    std::uint64_t exit_shim{};
    std::atomic<bool> closing{};

    std::uint64_t self() {
        const auto thread = process.current_thread();
        std::lock_guard lock(metadata);
        const auto found = handles.find(thread->identity);
        if (found != handles.end()) return found->second;
        const auto handle = process.allocate_guest(thread_object_size);
        process.main_engine().write64(handle, 0x544852440001ULL);
        handles.emplace(thread->identity, handle);
        return handle;
    }

    template<class T> std::shared_ptr<T> object(
        ObjectTable<T>& table,
        transmute::GuestMemory& memory, std::uint64_t address, std::size_t size, bool writable = true) {
        // A known object was accessible when it was created. Checking again on
        // every lock and unlock cost a VirtualQuery each under alchemy-fx, which
        // was most of a busy SwiftShader thread's time. Only a new address is checked.
        {
            std::lock_guard lock(metadata);
            if (const auto found = table.find(address); found != table.end()) return found->second;
        }
        if (!accessible(memory, address, size, writable)) return {};
        std::lock_guard lock(metadata);
        if (const auto found = table.find(address); found != table.end()) return found->second;
        auto value = std::make_shared<T>();
        table.emplace(address, value);
        return value;
    }

    bool stopping() { return closing.load() || process.requested_exit().has_value(); }
    // Workers unwind at process exit; the main thread keeps the explicit result.
    void stop_point() { if (guest_worker_must_stop(process)) throw GuestThreadStop{}; }

    std::uint32_t mutex_lock(const std::shared_ptr<Mutex>& value, bool attempt) {
        if (!value) return einval;
        const auto identity = process.current_thread()->identity;
        std::unique_lock lock(value->gate);
        if (value->owner == identity) {
            if (value->type == 2) { ++value->depth; return 0; }
            return attempt ? ebusy : edeadlk;
        }
        if (attempt && value->owner != 0) return ebusy;
        ++value->waiters;
        while (value->owner != 0) {
            value->changed.wait_for(lock, poll_period);
            if (stopping()) { --value->waiters; stop_point(); return ecanceled; }
        }
        --value->waiters;
        value->owner = identity;
        value->depth = 1;
        return 0;
    }

    std::uint32_t mutex_unlock(const std::shared_ptr<Mutex>& value) {
        if (!value) return einval;
        const auto identity = process.current_thread()->identity;
        std::lock_guard lock(value->gate);
        if (value->owner != identity) return eperm;
        if (--value->depth == 0) {
            value->owner = 0;
            value->changed.notify_one();
        }
        return 0;
    }

    // ---- process-shared objects in MAP_SHARED memory (alchemy/shared_wait.hpp) ----
    // A PTHREAD_PROCESS_SHARED mutex or condition variable initialized inside
    // a shared file mapping keeps its whole state in the guest object, so
    // every process mapping the file uses the same object:
    //   mutex (64 bytes): +0 signature, +8 'ALPS', +12 type, +16 state
    //     (0 free, 1 locked, 2 locked with waiters), +20 owner token, +24 depth;
    //   condition (48 bytes): +0 signature, +8 'ALPC', +16 sequence.
    // Blocking waits are keyed by the shared location of the state word.
    // Shared mappings exist only on alchemy-fx, where a guest address is the
    // host address, so the state words are host atomics.
    static constexpr std::uint64_t mutex_signature = 0x32aaaba7, cond_signature = 0x3cb0b1bb;
    static constexpr std::uint32_t pshared_mutex_magic = 0x53504c41, pshared_cond_magic = 0x43504c41;
    std::optional<std::string> shared_key(std::uint64_t address) {
        if (!process.shared_location) return std::nullopt;
        const auto location = process.shared_location(address);
        if (!location) return std::nullopt;
        return location->object + ":" + std::to_string(location->offset);
    }
    static std::atomic_ref<std::uint32_t> word(std::uint64_t address) {
        return std::atomic_ref<std::uint32_t>(*reinterpret_cast<std::uint32_t*>(address));
    }
    bool pshared_object(transmute::GuestMemory& memory, std::uint64_t address, std::uint64_t signature, std::uint32_t magic) {
        // Called on every lock and unlock: without shared mappings the location
        // lookup is one atomic load.
        if (!address || address % 8 || !process.shared_location || !process.shared_location(address)) return false;
        return accessible(memory, address, 16, false) && memory.read64(address) == signature && memory.read32(address + 8) == magic;
    }
    bool pshared_mutex(transmute::GuestMemory& memory, std::uint64_t address) {
        return pshared_object(memory, address, mutex_signature, pshared_mutex_magic);
    }
    bool pshared_cond(transmute::GuestMemory& memory, std::uint64_t address) {
        return pshared_object(memory, address, cond_signature, pshared_cond_magic);
    }
    std::uint32_t pshared_lock(transmute::GuestMemory& memory, std::uint64_t address, bool attempt) {
        auto state = word(address + 16), owner = word(address + 20), depth = word(address + 24);
        const auto type = memory.read32(address + 12);
        const auto me = shared_wait::owner_token();
        if (state.load() != 0 && owner.load() == me) {
            if (type == 2) { depth.fetch_add(1); return 0; }
            return attempt ? ebusy : edeadlk;
        }
        std::uint32_t current = 0;
        if (!state.compare_exchange_strong(current, 1)) {
            if (attempt) return ebusy;
            const auto key = shared_key(address + 16);
            if (!key) return einval;
            if (current != 2) current = state.exchange(2);
            while (current != 0) {
                const auto result = shared_wait::wait(*key, [&] { return state.load() == 2; },
                    std::chrono::steady_clock::time_point::max(), [this] { return stopping(); });
                if (result == shared_wait::Result::stopped) { stop_point(); return ecanceled; }
                current = state.exchange(2);
            }
        }
        owner.store(me);
        depth.store(1);
        return 0;
    }
    std::uint32_t pshared_unlock(transmute::GuestMemory& memory, std::uint64_t address) {
        auto state = word(address + 16), owner = word(address + 20), depth = word(address + 24);
        if (state.load() == 0 || owner.load() != shared_wait::owner_token()) return eperm;
        if (memory.read32(address + 12) == 2 && depth.load() > 1) { depth.fetch_sub(1); return 0; }
        owner.store(0);
        depth.store(0);
        if (state.exchange(0) == 2) {
            if (const auto key = shared_key(address + 16)) shared_wait::wake(*key, false);
        }
        return 0;
    }
    // Either kind of mutex (a condition variable may pair with both).
    std::uint32_t lock_any(transmute::GuestMemory& memory, std::uint64_t address) {
        if (pshared_mutex(memory, address)) return pshared_lock(memory, address, false);
        return mutex_lock(object(mutexes, memory, address, mutex_size), false);
    }
    std::uint32_t unlock_any(transmute::GuestMemory& memory, std::uint64_t address) {
        if (pshared_mutex(memory, address)) return pshared_unlock(memory, address);
        return mutex_unlock(object(mutexes, memory, address, mutex_size));
    }
    // pthread_cond_[timed]wait on a process-shared condition variable.
    std::uint32_t pshared_cond_wait(transmute::GuestMemory& memory, std::uint64_t condition, std::uint64_t mutex,
                                    std::chrono::system_clock::time_point deadline) {
        auto sequence = word(condition + 16);
        const auto key = shared_key(condition + 16);
        if (!key) return einval;
        const auto start = sequence.load();
        const auto released = unlock_any(memory, mutex);
        if (released != 0) return released;
        const auto remaining = deadline - std::chrono::system_clock::now();
        const auto steady = deadline == std::chrono::system_clock::time_point::max() ||
                remaining > std::chrono::hours(24 * 365 * 100)
            ? std::chrono::steady_clock::time_point::max()
            : std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(remaining);
        const auto result = shared_wait::wait(*key, [&] { return sequence.load() == start; }, steady,
                                              [this] { return stopping(); });
        if (result == shared_wait::Result::stopped) stop_point();
        const auto acquired = lock_any(memory, mutex);
        if (acquired != 0) return acquired;
        return result == shared_wait::Result::timed_out ? etimedout : 0;
    }

    void cleanup(ThreadContext& thread) {
        std::uint64_t handle = 0;
        {
            std::lock_guard lock(metadata);
            const auto found = handles.find(thread.identity);
            if (found != handles.end()) handle = found->second;
        }
        // Darwin's public pthread handle exposes the cleanup-chain pointer at
        // offset eight. Guest records are {routine, argument, next}, all LP64.
        std::unordered_set<std::uint64_t> visited;
        while (handle != 0 && thread.engine->read64(handle + 8) != 0) {
            const auto record = thread.engine->read64(handle + 8);
            if (!accessible(*thread.engine, record, 24, false) || !visited.insert(record).second)
                throw std::runtime_error("invalid Darwin pthread cleanup chain");
            const auto function = thread.engine->read64(record);
            const auto argument = thread.engine->read64(record + 8);
            const auto next = thread.engine->read64(record + 16);
            if (function == 0) throw std::runtime_error("null Darwin pthread cleanup routine");
            thread.engine->write64(handle + 8, next);
            const std::array<std::uint64_t, 1> arguments{argument};
            static_cast<void>(EngineAdapter(*thread.engine).call(function, arguments));
        }
        while (!thread.finalizers.empty()) {
            const auto [function, argument] = thread.finalizers.back();
            thread.finalizers.pop_back();
            const std::array<std::uint64_t, 1> arguments{argument};
            static_cast<void>(EngineAdapter(*thread.engine).call(function, arguments));
        }
        // POSIX permits four destructor passes when callbacks repopulate keys.
        for (unsigned pass = 0; pass < 4; ++pass) {
            std::vector<std::pair<std::uint64_t, std::uint64_t>> pending;
            {
                std::lock_guard lock(metadata);
                for (auto& [key, value] : thread.key_values) {
                    const auto destructor = keys.find(key);
                    if (value != 0 && destructor != keys.end() && destructor->second != 0) {
                        pending.emplace_back(destructor->second, value);
                        value = 0;
                    }
                }
            }
            if (pending.empty()) break;
            for (const auto& [function, argument] : pending) {
                const std::array<std::uint64_t, 1> arguments{argument};
                static_cast<void>(EngineAdapter(*thread.engine).call(function, arguments));
            }
        }
        if (process.cleanup_objects) process.cleanup_objects(thread);
        for (const auto& [module, address] : thread.tls_blocks) {
            static_cast<void>(module);
            process.release_guest(address);
        }
        thread.tls_blocks.clear();
        thread.tlv_addresses.clear();
        if (thread.tlv_table) process.main_engine().fill_bytes(thread.tlv_table, 0, tlv_table_entries * 8);
    }

    void create(transmute::CpuState& state, transmute::GuestMemory& memory) {
        if (stopping()) { state.x[0] = eagain; return; }
        if (!accessible(memory, state.x[0], 8) || state.x[2] == 0) { state.x[0] = einval; return; }
        ThreadAttribute attribute;
        if (state.x[1] != 0) {
            std::lock_guard lock(metadata);
            const auto found = thread_attributes.find(state.x[1]);
            if (found == thread_attributes.end()) { state.x[0] = einval; return; }
            attribute = found->second;
        }
        const auto output = state.x[0];
        auto worker = std::make_shared<Worker>();
        worker->detached = attribute.detached;
        worker->engine = EngineAdapter(process.calling_engine()).create_worker(
            state.x[2], state.x[3], attribute.stack_size);
        worker->engine->set_instruction_limit(process.instruction_limit);
        worker->engine->set_jit_profiling(process.jit_stats);
        worker->context = process.thread(*worker->engine);
        worker->handle = process.allocate_guest(thread_object_size);
        memory.write64(worker->handle, 0x544852440001ULL);
        std::unique_lock start_lock(worker->mutex);
        {
            std::lock_guard lock(metadata);
            if (closing.load() || workers.size() >= 1024) {
                process.release_thread(*worker->engine);
                process.release_guest(worker->handle);
                state.x[0] = eagain;
                return;
            }
            workers.emplace(worker->handle, worker);
            handles.emplace(worker->context->identity, worker->handle);
            ++worker_generation;
        }
        memory.write64(output, worker->handle);
        try {
            const auto owned = shared_from_this();
            worker->host = std::thread([owned, worker] {
                std::uint64_t instructions = 0;
                try {
                    instructions = worker->engine->run().instructions;
                    worker->result = worker->context->exit_value.value_or(worker->engine->state().x[0]);
                    owned->cleanup(*worker->context);
                } catch (const GuestThreadStop&) {
                    // Stopped in a libSystem wait at process exit: no guest cleanup runs.
                } catch (const std::exception& error) {
                    std::ostringstream diagnostic;
                    diagnostic << error.what() << " [worker guest pc 0x" << std::hex << worker->engine->state().pc
                        << ", thread " << std::dec << worker->context->identity << ']';
                    worker->error = std::make_exception_ptr(std::runtime_error(diagnostic.str()));
                    // A detached or never-joined thread would otherwise fail silently
                    // (often while holding a guest lock that others then wait on).
                    std::fprintf(stderr, "alchemy: guest thread ended by an error: %s\n", diagnostic.str().c_str());
                } catch (...) { worker->error = std::current_exception(); }
                owned->process.record_worker_execution(instructions, worker->engine->jit_stats());
                owned->process.release_thread(*worker->engine);
                std::lock_guard lock(worker->mutex);
                worker->finished = true;
                worker->changed.notify_all();
            });
        } catch (const std::system_error&) {
            std::lock_guard lock(metadata);
            workers.erase(worker->handle);
            handles.erase(worker->context->identity);
            ++worker_generation;
            process.release_thread(*worker->engine);
            process.release_guest(worker->handle);
            state.x[0] = eagain;
            return;
        }
        state.x[0] = 0;
    }

    void join(transmute::CpuState& state, transmute::GuestMemory& memory) {
        if (state.x[1] != 0 && !accessible(memory, state.x[1], 8)) { state.x[0] = einval; return; }
        std::shared_ptr<Worker> worker;
        {
            std::lock_guard lock(metadata);
            const auto found = workers.find(state.x[0]);
            if (found == workers.end()) { state.x[0] = esrch; return; }
            worker = found->second;
        }
        {
            std::lock_guard lock(worker->mutex);
            if (worker->context->identity == process.current_thread()->identity) { state.x[0] = edeadlk; return; }
            if (worker->detached || worker->joining || worker->joined) { state.x[0] = einval; return; }
            worker->joining = true;
        }
        try { worker->host.join(); }
        catch (...) {
            std::lock_guard lock(worker->mutex);
            worker->joining = false;
            worker->changed.notify_all();
            throw;
        }
        {
            std::lock_guard lock(worker->mutex);
            worker->joined = true;
            worker->joining = false;
            worker->changed.notify_all();
        }
        if (worker->error) std::rethrow_exception(worker->error);
        if (state.x[1] != 0) memory.write64(state.x[1], worker->result);
        state.x[0] = 0;
    }

    void join_all() {
        const auto main = process.thread(process.main_engine());
        const bool graceful = main->exit_value.has_value() && !process.requested_exit();
        do {
        std::vector<std::shared_ptr<Worker>> values;
        std::uint64_t generation;
        { std::lock_guard lock(metadata); closing.store(!graceful); generation = worker_generation; for (const auto& [handle, worker] : workers) {
            static_cast<void>(handle); values.push_back(worker);
        } }
        std::exception_ptr failure;
        for (const auto& worker : values) {
            std::unique_lock lock(worker->mutex);
            while (worker->joining) worker->changed.wait_for(lock, poll_period);
            if (worker->host.joinable()) {
                worker->joining = true;
                lock.unlock();
                worker->host.join();
                lock.lock();
                worker->joining = false;
                worker->joined = true;
                worker->changed.notify_all();
            }
            if (worker->error && !failure) failure = worker->error;
        }
        if (failure) std::rethrow_exception(failure);
        { std::lock_guard lock(metadata); if (generation == worker_generation) break; }
        } while (true);
    }

    // Guest fast path of __tlv_bootstrap (engines with a thread pointer): the
    // descriptor's module id indexes this thread's table at TPIDR_EL0; an
    // empty table, entry or out-of-range id tail-calls the host handler
    // below, which fills the entry. Like Darwin's tlv_get_addr it clobbers
    // only x0, x16 and x17 (and not the flags):
    //   ldr x16, [x0, #8]; lsr x17, x16, #10; cbnz x17, slow
    //   mrs x17, tpidr_el0; cbz x17, slow; ldr x17, [x17, x16, lsl #3]; cbz x17, slow
    //   ldr x16, [x0, #16]; add x0, x17, x16; ret
    //   slow: ldr x16, =host; br x16
    static constexpr std::uint64_t tlv_table_entries = 1024;
    std::uint64_t tlv_fast_path(std::uint64_t host) {
        static constexpr std::array<std::uint32_t, 12> code{
            0xf9400410, 0xd34afe11, 0xb5000111, 0xd53bd051, 0xb40000d1, 0xf8707a31,
            0xb4000091, 0xf9400810, 0x8b100220, 0xd65f03c0, 0x58000050, 0xd61f0200};
        const auto stub = process.allocate_guest(64, 16);
        auto& memory = process.main_engine();
        for (std::size_t i = 0; i < code.size(); ++i) memory.write32(stub + i * 4, code[i]);
        memory.write64(stub + 48, host);
        memory.set_memory_access(stub, true, false, true);
        return stub;
    }
    // Publishes `base` as this thread's TLS block of `module` to the fast path.
    void publish_tlv(ThreadContext& thread, transmute::GuestMemory& memory, std::uint64_t module, std::uint64_t base) {
        if constexpr (engine_has_thread_pointer<transmute::ReferenceEngine>) {
            if (module >= tlv_table_entries) return;
            if (!thread.tlv_table) {
                // Never released: a stale TPIDR_EL0 of an ended thread then
                // still reads zeroed memory (cleanup_thread clears it).
                const auto table = engine_heap_allocate(process.main_engine(), tlv_table_entries * 8, 16);
                thread.tlv_table = table ? *table : process.allocate_guest(tlv_table_entries * 8, 16);
                memory.fill_bytes(thread.tlv_table, 0, tlv_table_entries * 8);
            }
            memory.write64(thread.tlv_table + module * 8, base);
            set_engine_thread_pointer(process.calling_engine(), thread.tlv_table);
        } else {
            static_cast<void>(thread); static_cast<void>(memory); static_cast<void>(module); static_cast<void>(base);
        }
    }

    void tlv(transmute::CpuState& state, transmute::GuestMemory& memory) {
        const auto descriptor = state.x[0];
        auto thread = process.current_thread();
        // Fast path: every thread_local access of compiled code calls here.
        if (const auto cached = thread->tlv_addresses.find(descriptor); cached != thread->tlv_addresses.end()) {
            state.x[0] = cached->second;
            if constexpr (engine_has_thread_pointer<transmute::ReferenceEngine>) {
                const auto module = memory.read64(descriptor + 8);
                if (const auto block = thread->tls_blocks.find(module); block != thread->tls_blocks.end())
                    publish_tlv(*thread, memory, module, block->second);
            }
            return;
        }
        if (!accessible(memory, descriptor, 24, false)) throw std::runtime_error("invalid Darwin TLV descriptor");
        const auto module = memory.read64(descriptor + 8);
        const auto offset = memory.read64(descriptor + 16);
        for (const auto& image : process.images()) for (const auto& tls : image.tls) {
            if (tls.module_id != module || std::find(tls.descriptors.begin(), tls.descriptors.end(), descriptor) == tls.descriptors.end()) continue;
            if (tls.zero_fill_size > UINT64_MAX - tls.initial_data.size()) throw std::runtime_error("Darwin TLV size overflow");
            const auto size = tls.initial_data.size() + tls.zero_fill_size;
            if (offset >= size) throw std::runtime_error("Darwin TLV offset outside template");
            auto found = thread->tls_blocks.find(module);
            if (found == thread->tls_blocks.end()) {
                const auto base = process.allocate_guest(size, tls.alignment);
                memory.write_bytes(base, tls.initial_data);
                found = thread->tls_blocks.emplace(module, base).first;
                for (const auto initializer : tls.initializers) {
                    const std::array<std::uint64_t, 1> arguments{base};
                    static_cast<void>(EngineAdapter(process.calling_engine()).call(initializer, arguments));
                    if (thread->exit_value) EngineAdapter::require("thread exit from a nested guest callback");
                }
            }
            state.x[0] = found->second + offset;
            thread->tlv_addresses.emplace(descriptor, state.x[0]);
            publish_tlv(*thread, memory, module, found->second);
            return;
        }
        throw std::runtime_error("Darwin TLV descriptor references unpublished module");
    }

    void install() {
        const auto owned = shared_from_this();
        const auto add = [&](std::string name, auto operation) {
            static_cast<void>(process.install_function(std::move(name), [owned, operation](auto& state, auto& memory) {
                operation(*owned, state, memory);
            }));
        };
        add("_pthread_create", [](auto& self, auto& s, auto& m) { self.create(s, m); });
        add("_pthread_join", [](auto& self, auto& s, auto& m) { self.join(s, m); });
        add("_pthread_self", [](auto& self, auto& s, auto&) { s.x[0] = self.self(); });
        add("_pthread_equal", [](auto&, auto& s, auto&) { s.x[0] = s.x[0] == s.x[1] ? 1 : 0; });
        add("_pthread_attr_init", [](auto& self, auto& s, auto& m) {
            if (!accessible(m, s.x[0], 64)) { s.x[0] = einval; return; }
            std::lock_guard lock(self.metadata); self.thread_attributes[s.x[0]] = {};
            m.fill_bytes(s.x[0], 0, 64); m.write64(s.x[0], 0x5448524441545452ULL); s.x[0] = 0;
        });
        add("_pthread_attr_destroy", [](auto& self, auto& s, auto&) {
            std::lock_guard lock(self.metadata); s.x[0] = self.thread_attributes.erase(s.x[0]) ? 0 : einval;
        });
        add("_pthread_attr_setscope", [](auto& self, auto& s, auto&) {
            std::lock_guard lock(self.metadata);
            if (!self.thread_attributes.contains(s.x[0])) s.x[0] = einval;
            else s.x[0] = s.x[1] == 1 ? 0 : (s.x[1] == 2 ? 45 : einval);
        });
        add("_pthread_attr_getscope", [](auto& self, auto& s, auto& m) {
            std::lock_guard lock(self.metadata);
            if (!self.thread_attributes.contains(s.x[0]) || !accessible(m, s.x[1], 4)) { s.x[0] = einval; return; }
            m.write32(s.x[1], 1); s.x[0] = 0;
        });
        add("_pthread_attr_setstacksize", [](auto& self, auto& s, auto&) {
            std::lock_guard lock(self.metadata); const auto found = self.thread_attributes.find(s.x[0]);
            if (found == self.thread_attributes.end() || s.x[1] < 16384 ||
                s.x[1] > EngineAdapter::maximum_stack_size || s.x[1] % 16384) { s.x[0] = einval; return; }
            found->second.stack_size = s.x[1]; s.x[0] = 0;
        });
        add("_pthread_attr_getstacksize", [](auto& self, auto& s, auto& m) {
            const auto default_size = EngineAdapter(self.process.calling_engine()).default_worker_stack_size();
            std::lock_guard lock(self.metadata); const auto found = self.thread_attributes.find(s.x[0]);
            if (found == self.thread_attributes.end() || !accessible(m, s.x[1], 8)) { s.x[0] = einval; return; }
            m.write64(s.x[1], found->second.stack_size ? found->second.stack_size : default_size); s.x[0] = 0;
        });
        add("_pthread_attr_setdetachstate", [](auto& self, auto& s, auto&) {
            std::lock_guard lock(self.metadata); const auto found = self.thread_attributes.find(s.x[0]);
            if (found == self.thread_attributes.end() || (s.x[1] != 1 && s.x[1] != 2)) { s.x[0] = einval; return; }
            found->second.detached = s.x[1] == 2; s.x[0] = 0;
        });
        add("_pthread_attr_getdetachstate", [](auto& self, auto& s, auto& m) {
            std::lock_guard lock(self.metadata); const auto found = self.thread_attributes.find(s.x[0]);
            if (found == self.thread_attributes.end() || !accessible(m, s.x[1], 4)) { s.x[0] = einval; return; }
            m.write32(s.x[1], found->second.detached ? 2 : 1); s.x[0] = 0;
        });
        add("_pthread_threadid_np", [](auto& self, auto& s, auto& m) {
            if (!accessible(m, s.x[1], 8)) { s.x[0] = einval; return; }
            if (!s.x[0]) { m.write64(s.x[1], self.process.current_thread()->identity); s.x[0] = 0; return; }
            std::lock_guard lock(self.metadata);
            for (const auto& [identity, handle] : self.handles) if (handle == s.x[0]) { m.write64(s.x[1], identity); s.x[0] = 0; return; }
            s.x[0] = esrch;
        });
        add("_pthread_get_stacksize_np", [](auto& self, auto& s, auto&) {
            const auto own = self.self();
            if (s.x[0] == own) { const auto bounds = self.process.calling_engine().stack_bounds(); s.x[0] = bounds.second - bounds.first; return; }
            std::lock_guard lock(self.metadata); const auto found = self.workers.find(s.x[0]);
            if (found == self.workers.end()) { s.x[0] = 0; return; }
            const auto bounds = found->second->engine->stack_bounds(); s.x[0] = bounds.second - bounds.first;
        });
        add("_pthread_get_stackaddr_np", [](auto& self, auto& s, auto&) {
            const auto own = self.self();
            if (s.x[0] == own) { s.x[0] = self.process.calling_engine().stack_bounds().second; return; }
            std::lock_guard lock(self.metadata); const auto found = self.workers.find(s.x[0]);
            s.x[0] = found == self.workers.end() ? 0 : found->second->engine->stack_bounds().second;
        });
        add("_pthread_sigmask", [](auto& self, auto& s, auto&) {
            const auto function = self.process.resolve("/usr/lib/libSystem.B.dylib", "_sigprocmask");
            if (!function) { s.x[0] = 78; return; }
            const std::array<std::uint64_t, 3> arguments{s.x[0], s.x[1], s.x[2]};
            const auto result = EngineAdapter(self.process.calling_engine()).call(function->address, arguments);
            s.x[0] = result == UINT64_MAX ? self.process.calling_engine().read32(self.process.current_thread()->errno_address) : 0;
        });
        add("_pthread_kill", [](auto& self, auto& s, auto&) {
            if (s.x[1] > 31) { s.x[0] = einval; return; }
            const auto own = self.self();
            std::lock_guard lock(self.metadata);
            if (s.x[0] != own && !self.workers.contains(s.x[0])) { s.x[0] = esrch; return; }
            s.x[0] = s.x[1] ? 78 : 0;
        });
        add("_sched_yield", [](auto&, auto& s, auto&) { std::this_thread::yield(); s.x[0] = 0; });
        add("_pthread_atfork", [](auto& self, auto& s, auto&) {
            // Recorded for completeness; fork itself is unsupported, so no handler ever runs.
            std::lock_guard lock(self.metadata);
            if (self.fork_handlers.size() >= 1024) { s.x[0] = 12; return; }
            self.fork_handlers.push_back({s.x[0], s.x[1], s.x[2]}); s.x[0] = 0;
        });
        add("_pthread_setname_np", [](auto& self, auto& s, auto& m) {
            if (!s.x[0] || !accessible(m, s.x[0], 1, false)) { s.x[0] = 14; return; }
            bool terminated{};
            const auto bytes = m.read_string(s.x[0], 1, 64, &terminated);
            if (!terminated) { s.x[0] = 63; return; } // ENAMETOOLONG beyond MAXTHREADNAMESIZE - 1.
            const auto identity = self.process.current_thread()->identity;
            std::lock_guard lock(self.metadata); self.names[identity].assign(bytes.begin(), bytes.end()); s.x[0] = 0;
        });
        add("_pthread_getname_np", [](auto& self, auto& s, auto& m) {
            if (!s.x[2] || !accessible(m, s.x[1], s.x[2])) { s.x[0] = einval; return; }
            const auto own = self.self();
            const auto current = self.process.current_thread()->identity;
            std::lock_guard lock(self.metadata);
            std::optional<std::uint64_t> identity;
            if (s.x[0] == own) identity = current;
            else for (const auto& [thread, handle] : self.handles) if (handle == s.x[0]) identity = thread;
            if (!identity) { s.x[0] = esrch; return; }
            const auto found = self.names.find(*identity);
            const std::string name = found == self.names.end() ? std::string{} : found->second;
            const auto count = std::min<std::uint64_t>(name.size(), s.x[2] - 1);
            m.write_bytes(s.x[1], {reinterpret_cast<const std::uint8_t*>(name.data()), static_cast<std::size_t>(count)});
            m.write8(s.x[1] + count, 0); s.x[0] = 0;
        });
        add("_pthread_set_qos_class_self_np", [](auto&, auto& s, auto&) {
            // QoS is a scheduling hint; valid classes are accepted, the host scheduler is unchanged.
            const auto qos = static_cast<std::uint32_t>(s.x[0]);
            const auto relative = static_cast<std::int32_t>(static_cast<std::uint32_t>(s.x[1]));
            const bool known = qos == 0x21 || qos == 0x19 || qos == 0x15 || qos == 0x11 || qos == 0x09 || qos == 0;
            s.x[0] = known && relative <= 0 && relative >= -15 ? 0 : einval;
        });
        add("_pthread_cancel", [](auto& self, auto& s, auto&) {
            const auto own = self.self();
            std::lock_guard lock(self.metadata);
            if (s.x[0] != own && !self.workers.contains(s.x[0])) { s.x[0] = esrch; return; }
            s.x[0] = 78; // ENOSYS: asynchronous and deferred cancellation are not implemented.
        });
        for (const auto& [name, type] : std::array<std::pair<const char*, bool>, 2>{{{"_pthread_setcancelstate", false}, {"_pthread_setcanceltype", true}}}) {
            add(name, [type](auto& self, auto& s, auto& m) {
                // State 1/0 is enable/disable, type 2/0 deferred/asynchronous. No cancellation is
                // ever delivered, so these only record the mode for the old-value output.
                const auto value = static_cast<std::uint32_t>(s.x[0]);
                if (type ? value != 0 && value != 2 : value > 1) { s.x[0] = einval; return; }
                if (s.x[1] && !accessible(m, s.x[1], 4)) { s.x[0] = einval; return; }
                const auto identity = self.process.current_thread()->identity;
                std::lock_guard lock(self.metadata);
                auto& mode = self.cancel_modes.try_emplace(identity, 1U, 2U).first->second;
                auto& slot = type ? mode.second : mode.first;
                if (s.x[1]) m.write32(s.x[1], slot);
                slot = value; s.x[0] = 0;
            });
        }
        add("_pthread_testcancel", [](auto&, auto& s, auto&) { s.x[0] = 0; });
        add("_pthread_yield_np", [](auto&, auto& s, auto&) { std::this_thread::yield(); s.x[0] = 0; });
        for (const auto name : {"_sched_get_priority_min", "_sched_get_priority_max"}) add(name, [](auto& self, auto& s, auto&) {
            // Only the ordinary SCHED_OTHER policy is implemented.
            if (s.x[0] != 1) { self.process.set_errno(78); s.x[0] = UINT64_MAX; return; }
            s.x[0] = 0;
        });
        add("___alchemy_pthread_cleanup_push", [](auto& self, auto& s, auto& m) {
            if (!accessible(m, s.x[0], 24) || s.x[1] == 0) throw std::runtime_error("invalid Alchemy pthread cleanup record");
            const auto handle = self.self();
            m.write64(s.x[0], s.x[1]); m.write64(s.x[0] + 8, s.x[2]);
            m.write64(s.x[0] + 16, m.read64(handle + 8)); m.write64(handle + 8, s.x[0]); s.x[0] = 0;
        });
        add("___alchemy_pthread_cleanup_pop", [](auto& self, auto& s, auto& m) {
            const auto handle = self.self();
            if (!accessible(m, s.x[0], 24, false) || m.read64(handle + 8) != s.x[0]) throw std::runtime_error("unbalanced Alchemy pthread cleanup pop");
            const auto function = m.read64(s.x[0]), argument = m.read64(s.x[0] + 8);
            m.write64(handle + 8, m.read64(s.x[0] + 16));
            if (s.x[1] != 0) {
                const std::array<std::uint64_t, 1> arguments{argument};
                static_cast<void>(EngineAdapter(self.process.calling_engine()).call(function, arguments));
                if (self.process.current_thread()->exit_value) EngineAdapter::require("thread exit from a nested guest callback");
            }
            s.x[0] = 0;
        });
        add("_pthread_mach_thread_np", [](auto& self, auto& s, auto&) {
            std::lock_guard lock(self.metadata);
            for (const auto& [identity, handle] : self.handles) {
                if (handle == s.x[0]) { s.x[0] = static_cast<std::uint32_t>(identity); return; }
            }
            s.x[0] = 0;
        });
        // pthread_from_mach_thread_np: the inverse of pthread_mach_thread_np
        // (NULL for a port that names no thread of this process).
        add("_pthread_from_mach_thread_np", [](auto& self, auto& s, auto&) {
            const auto port = static_cast<std::uint32_t>(s.x[0]);
            if (port == static_cast<std::uint32_t>(self.process.current_thread()->identity)) { s.x[0] = self.self(); return; }
            std::lock_guard lock(self.metadata);
            for (const auto& [identity, handle] : self.handles) {
                if (static_cast<std::uint32_t>(identity) == port) { s.x[0] = handle; return; }
            }
            s.x[0] = 0;
        });
        add("_pthread_detach", [](auto& self, auto& s, auto&) {
            std::shared_ptr<Worker> worker;
            { std::lock_guard lock(self.metadata); const auto found = self.workers.find(s.x[0]);
              if (found == self.workers.end()) { s.x[0] = esrch; return; } worker = found->second; }
            std::lock_guard lock(worker->mutex);
            if (worker->detached || worker->joining || worker->joined) { s.x[0] = einval; return; }
            worker->detached = true; s.x[0] = 0;
        });
        for (const auto& [name, attempt] : std::array<std::pair<const char*, bool>, 2>{{{"_pthread_mutex_lock", false}, {"_pthread_mutex_trylock", true}}}) {
            add(name, [attempt](auto& self, auto& s, auto& m) {
                if (self.pshared_mutex(m, s.x[0])) { s.x[0] = self.pshared_lock(m, s.x[0], attempt); return; }
                s.x[0] = self.mutex_lock(self.object(self.mutexes, m, s.x[0], mutex_size), attempt);
            });
        }
        add("_pthread_mutex_unlock", [](auto& self, auto& s, auto& m) { s.x[0] = self.unlock_any(m, s.x[0]); });
        add("_pthread_mutex_init", [](auto& self, auto& s, auto& m) {
            unsigned type = 0;
            bool process_shared = false;
            if (s.x[1] != 0) { std::lock_guard lock(self.metadata); const auto found = self.attributes.find(s.x[1]);
                if (found == self.attributes.end()) { s.x[0] = einval; return; } type = found->second;
                const auto scope = self.mutex_shared.find(s.x[1]);
                process_shared = scope != self.mutex_shared.end() && scope->second == 1; }
            // PTHREAD_PROCESS_SHARED inside a shared file mapping: the state lives in the object.
            if (process_shared && s.x[0] % 8 == 0 && accessible(m, s.x[0], mutex_size) && self.shared_key(s.x[0])) {
                if (self.pshared_mutex(m, s.x[0]) && Impl::word(s.x[0] + 16).load() != 0) { s.x[0] = ebusy; return; }
                m.write64(s.x[0] + 8, 0); m.write64(s.x[0] + 16, 0); m.write64(s.x[0] + 24, 0);
                m.write32(s.x[0] + 12, type); m.write32(s.x[0] + 8, Impl::pshared_mutex_magic);
                m.write64(s.x[0], Impl::mutex_signature); s.x[0] = 0; return;
            }
            const auto value = self.object(self.mutexes, m, s.x[0], mutex_size);
            if (!value) { s.x[0] = einval; return; }
            std::lock_guard lock(value->gate);
            if (value->owner != 0 || value->waiters != 0) { s.x[0] = ebusy; return; }
            value->type = type; m.write64(s.x[0], 0x32aaaba7); s.x[0] = 0;
        });
        add("_pthread_mutex_destroy", [](auto& self, auto& s, auto& m) {
            const auto address = s.x[0];
            if (self.pshared_mutex(m, address)) {
                if (Impl::word(address + 16).load() != 0) { s.x[0] = ebusy; return; }
                m.write64(address + 8, 0); m.write64(address, 0); s.x[0] = 0; return;
            }
            const auto value = self.object(self.mutexes, m, address, mutex_size);
            if (!value) { s.x[0] = einval; return; }
            std::lock_guard object_lock(value->gate);
            if (value->owner != 0 || value->waiters != 0) { s.x[0] = ebusy; return; }
            std::lock_guard lock(self.metadata); self.mutexes.erase(address); m.write64(address, 0); s.x[0] = 0;
        });
        add("_pthread_mutexattr_init", [](auto& self, auto& s, auto& m) {
            if (!accessible(m, s.x[0], 16)) { s.x[0] = einval; return; }
            std::lock_guard lock(self.metadata); self.attributes[s.x[0]] = 0; m.write64(s.x[0], 0x4d545841); s.x[0] = 0;
        });
        add("_pthread_mutexattr_settype", [](auto& self, auto& s, auto&) {
            std::lock_guard lock(self.metadata); const auto found = self.attributes.find(s.x[0]);
            if (found == self.attributes.end() || s.x[1] > 2) { s.x[0] = einval; return; }
            found->second = static_cast<unsigned>(s.x[1]); s.x[0] = 0;
        });
        add("_pthread_mutexattr_destroy", [](auto& self, auto& s, auto&) {
            std::lock_guard lock(self.metadata); s.x[0] = self.attributes.erase(s.x[0]) != 0 ? 0 : einval;
        });
        add("_pthread_mutexattr_setpshared", [](auto& self, auto& s, auto&) {
            // Without fork every guest thread shares one process, so both scopes behave alike.
            std::lock_guard lock(self.metadata);
            if (!self.attributes.contains(s.x[0]) || (s.x[1] != 1 && s.x[1] != 2)) { s.x[0] = einval; return; }
            self.mutex_shared[s.x[0]] = static_cast<unsigned>(s.x[1]); s.x[0] = 0;
        });
        add("_pthread_mutexattr_getpshared", [](auto& self, auto& s, auto& m) {
            std::lock_guard lock(self.metadata);
            if (!self.attributes.contains(s.x[0]) || !accessible(m, s.x[1], 4)) { s.x[0] = einval; return; }
            const auto found = self.mutex_shared.find(s.x[0]);
            m.write32(s.x[1], found == self.mutex_shared.end() ? 2 : found->second); s.x[0] = 0;
        });
        add("_pthread_mutexattr_gettype", [](auto& self, auto& s, auto& m) {
            std::lock_guard lock(self.metadata); const auto found = self.attributes.find(s.x[0]);
            if (found == self.attributes.end() || !accessible(m, s.x[1], 4)) { s.x[0] = einval; return; }
            m.write32(s.x[1], found->second); s.x[0] = 0;
        });
        add("_pthread_condattr_init", [](auto& self, auto& s, auto& m) {
            if (!accessible(m, s.x[0], 16)) { s.x[0] = einval; return; }
            std::lock_guard lock(self.metadata); self.condition_attributes[s.x[0]] = 2;
            m.write64(s.x[0], 0x434e4441); s.x[0] = 0;
        });
        add("_pthread_condattr_destroy", [](auto& self, auto& s, auto&) {
            std::lock_guard lock(self.metadata); s.x[0] = self.condition_attributes.erase(s.x[0]) != 0 ? 0 : einval;
        });
        add("_pthread_condattr_setpshared", [](auto& self, auto& s, auto&) {
            std::lock_guard lock(self.metadata); const auto found = self.condition_attributes.find(s.x[0]);
            if (found == self.condition_attributes.end() || (s.x[1] != 1 && s.x[1] != 2)) { s.x[0] = einval; return; }
            found->second = static_cast<unsigned>(s.x[1]); s.x[0] = 0;
        });
        add("_pthread_condattr_getpshared", [](auto& self, auto& s, auto& m) {
            std::lock_guard lock(self.metadata); const auto found = self.condition_attributes.find(s.x[0]);
            if (found == self.condition_attributes.end() || !accessible(m, s.x[1], 4)) { s.x[0] = einval; return; }
            m.write32(s.x[1], found->second); s.x[0] = 0;
        });
        add("_pthread_cond_init", [](auto& self, auto& s, auto& m) {
            bool process_shared = false;
            if (s.x[1] != 0) {
                std::lock_guard lock(self.metadata);
                const auto found = self.condition_attributes.find(s.x[1]);
                if (found == self.condition_attributes.end()) { s.x[0] = einval; return; }
                process_shared = found->second == 1;
            }
            // PTHREAD_PROCESS_SHARED inside a shared file mapping: a sequence word in the object.
            if (process_shared && s.x[0] % 8 == 0 && accessible(m, s.x[0], cond_size) && self.shared_key(s.x[0])) {
                m.write64(s.x[0] + 16, 0); m.write32(s.x[0] + 12, 0); m.write32(s.x[0] + 8, Impl::pshared_cond_magic);
                m.write64(s.x[0], Impl::cond_signature); s.x[0] = 0; return;
            }
            const auto value = self.object(self.conditions, m, s.x[0], cond_size);
            if (!value) { s.x[0] = einval; return; } m.write64(s.x[0], 0x3cb0b1bb); s.x[0] = 0;
        });
        for (const auto& [name, broadcast] : std::array<std::pair<const char*, bool>, 2>{{{"_pthread_cond_signal", false}, {"_pthread_cond_broadcast", true}}}) {
            add(name, [broadcast](auto& self, auto& s, auto& m) {
                if (self.pshared_cond(m, s.x[0])) {
                    Impl::word(s.x[0] + 16).fetch_add(1);
                    if (const auto key = self.shared_key(s.x[0] + 16)) shared_wait::wake(*key, broadcast);
                    s.x[0] = 0; return;
                }
                const auto value = self.object(self.conditions, m, s.x[0], cond_size);
                if (!value) { s.x[0] = einval; return; }
                std::lock_guard lock(value->gate);
                if (broadcast) { ++value->generation; value->tickets = 0; value->changed.notify_all(); }
                else if (value->tickets < value->waiters) { ++value->tickets; value->changed.notify_one(); }
                s.x[0] = 0;
            });
        }
        for (const auto& [name, timed] : std::array<std::pair<const char*, bool>, 2>{{{"_pthread_cond_wait", false}, {"_pthread_cond_timedwait", true}}}) {
            add(name, [timed](auto& self, auto& s, auto& m) {
                const bool shared_condition = self.pshared_cond(m, s.x[0]);
                const auto condition = shared_condition ? nullptr : self.object(self.conditions, m, s.x[0], cond_size);
                const bool shared_mutex = self.pshared_mutex(m, s.x[1]);
                const auto mutex = shared_mutex ? nullptr : self.object(self.mutexes, m, s.x[1], mutex_size);
                if ((!shared_condition && !condition) || (!shared_mutex && !mutex)) { s.x[0] = einval; return; }
                auto deadline = std::chrono::system_clock::time_point::max();
                if (timed) {
                    if (!accessible(m, s.x[2], 16, false)) { s.x[0] = einval; return; }
                    const auto seconds = static_cast<std::int64_t>(m.read64(s.x[2])); const auto nanos = m.read64(s.x[2] + 8);
                    if (nanos >= 1'000'000'000) { s.x[0] = einval; return; }
                    using Clock = std::chrono::system_clock;
                    const auto maximum = Clock::time_point::max().time_since_epoch();
                    const auto minimum = Clock::time_point::min().time_since_epoch();
                    if (seconds > std::chrono::duration_cast<std::chrono::seconds>(maximum).count()) deadline = Clock::time_point::max();
                    else if (seconds < std::chrono::duration_cast<std::chrono::seconds>(minimum).count()) deadline = Clock::time_point::min();
                    else {
                        const auto whole = std::chrono::duration_cast<Clock::duration>(std::chrono::seconds(seconds));
                        const auto fraction = std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(nanos));
                        deadline = whole >= Clock::duration::zero() && fraction > maximum - whole ?
                            Clock::time_point::max() : Clock::time_point(whole + fraction);
                    }
                }
                if (shared_condition) { s.x[0] = self.pshared_cond_wait(m, s.x[0], s.x[1], deadline); return; }
                std::unique_lock lock(condition->gate);
                const auto generation = condition->generation;
                const auto released = shared_mutex ? self.pshared_unlock(m, s.x[1]) : self.mutex_unlock(mutex);
                if (released != 0) { s.x[0] = released; return; }
                ++condition->waiters;
                std::uint32_t error = 0;
                while (generation == condition->generation && condition->tickets == 0) {
                    condition->changed.wait_for(lock, poll_period);
                    if (timed && std::chrono::system_clock::now() >= deadline) { error = etimedout; break; }
                    if (self.stopping()) { error = ecanceled; break; }
                }
                if (generation == condition->generation && condition->tickets != 0) --condition->tickets;
                --condition->waiters; lock.unlock();
                if (error == ecanceled) self.stop_point();
                const auto acquired = shared_mutex ? self.pshared_lock(m, s.x[1], false) : self.mutex_lock(mutex, false);
                s.x[0] = acquired != 0 ? acquired : error;
            });
        }
        add("_pthread_cond_destroy", [](auto& self, auto& s, auto& m) {
            const auto address = s.x[0];
            if (self.pshared_cond(m, address)) { m.write64(address + 8, 0); m.write64(address, 0); s.x[0] = 0; return; }
            const auto value = self.object(self.conditions, m, address, cond_size);
            if (!value) { s.x[0] = einval; return; }
            std::lock_guard object_lock(value->gate);
            if (value->waiters != 0) { s.x[0] = ebusy; return; }
            std::lock_guard lock(self.metadata); self.conditions.erase(address); m.write64(address, 0); s.x[0] = 0;
        });
        // pthread_rwlockattr_t: a marker word and the process-shared setting. Locks are
        // process-private; PTHREAD_PROCESS_SHARED (1) is not supported (ENOTSUP).
        static constexpr std::uint64_t rwlock_attribute_marker = 0x52574c4b41545452ULL;   // "RTTAKLWR"
        add("_pthread_rwlockattr_init", [](auto&, auto& s, auto& m) {
            if (!accessible(m, s.x[0], 16)) { s.x[0] = einval; return; }
            m.write64(s.x[0], rwlock_attribute_marker); m.write64(s.x[0] + 8, 2); s.x[0] = 0;   // PTHREAD_PROCESS_PRIVATE
        });
        add("_pthread_rwlockattr_destroy", [](auto&, auto& s, auto& m) {
            if (!accessible(m, s.x[0], 16) || m.read64(s.x[0]) != rwlock_attribute_marker) { s.x[0] = einval; return; }
            m.write64(s.x[0], 0); s.x[0] = 0;
        });
        add("_pthread_rwlockattr_setpshared", [](auto&, auto& s, auto& m) {
            if (!accessible(m, s.x[0], 16) || m.read64(s.x[0]) != rwlock_attribute_marker || (s.x[1] != 1 && s.x[1] != 2)) { s.x[0] = einval; return; }
            if (s.x[1] == 1) { s.x[0] = 45; return; }
            m.write64(s.x[0] + 8, s.x[1]); s.x[0] = 0;
        });
        add("_pthread_rwlockattr_getpshared", [](auto&, auto& s, auto& m) {
            if (!accessible(m, s.x[0], 16) || m.read64(s.x[0]) != rwlock_attribute_marker || !accessible(m, s.x[1], 4)) { s.x[0] = einval; return; }
            m.write32(s.x[1], static_cast<std::uint32_t>(m.read64(s.x[0] + 8))); s.x[0] = 0;
        });
        add("_pthread_rwlock_init", [](auto& self, auto& s, auto& m) {
            if (s.x[1] && (!accessible(m, s.x[1], 16) || m.read64(s.x[1]) != rwlock_attribute_marker)) { s.x[0] = einval; return; }
            if (!self.object(self.rwlocks, m, s.x[0], rwlock_size)) { s.x[0] = einval; return; }
            m.write64(s.x[0], 0x2da8b3b4); s.x[0] = 0;
        });
        for (const auto& [name, write, attempt] : std::array<std::tuple<const char*, bool, bool>, 4>{{
            {"_pthread_rwlock_rdlock", false, false}, {"_pthread_rwlock_wrlock", true, false},
            {"_pthread_rwlock_tryrdlock", false, true}, {"_pthread_rwlock_trywrlock", true, true}}}) {
            add(name, [write, attempt](auto& self, auto& s, auto& m) {
                const auto value = self.object(self.rwlocks, m, s.x[0], rwlock_size);
                if (!value) { s.x[0] = einval; return; }
                const auto identity = self.process.current_thread()->identity;
                std::unique_lock lock(value->gate);
                if (value->writer == identity || (write && value->readers.contains(identity))) { s.x[0] = attempt ? ebusy : edeadlk; return; }
                const auto ready = [&] { return value->writer == 0 && (!write || value->readers.empty()); };
                if (attempt && !ready()) { s.x[0] = ebusy; return; }
                if (write) ++value->waiting_writers;
                while (!ready()) { value->changed.wait_for(lock, poll_period);
                    if (self.stopping()) { if (write) --value->waiting_writers; self.stop_point(); s.x[0] = ecanceled; return; } }
                if (write) { --value->waiting_writers; value->writer = identity; }
                else ++value->readers[identity];
                s.x[0] = 0;
            });
        }
        add("_pthread_rwlock_unlock", [](auto& self, auto& s, auto& m) {
            const auto value = self.object(self.rwlocks, m, s.x[0], rwlock_size);
            if (!value) { s.x[0] = einval; return; }
            const auto identity = self.process.current_thread()->identity;
            std::lock_guard lock(value->gate);
            if (value->writer == identity) value->writer = 0;
            else { const auto found = value->readers.find(identity);
                if (found == value->readers.end()) { s.x[0] = eperm; return; }
                if (--found->second == 0) value->readers.erase(found); }
            value->changed.notify_all(); s.x[0] = 0;
        });
        add("_pthread_rwlock_destroy", [](auto& self, auto& s, auto& m) {
            const auto address = s.x[0]; const auto value = self.object(self.rwlocks, m, address, rwlock_size);
            if (!value) { s.x[0] = einval; return; }
            std::lock_guard object_lock(value->gate);
            if (value->writer != 0 || !value->readers.empty() || value->waiting_writers != 0) { s.x[0] = ebusy; return; }
            std::lock_guard lock(self.metadata); self.rwlocks.erase(address); m.write64(address, 0); s.x[0] = 0;
        });
        add("_pthread_once", [](auto& self, auto& s, auto& m) {
            const auto value = self.object(self.once, m, s.x[0], 16);
            if (!value || s.x[1] == 0) { s.x[0] = einval; return; }
            const auto identity = self.process.current_thread()->identity;
            std::unique_lock lock(value->gate);
            while (value->owner != 0 && !value->complete) {
                if (value->owner == identity) { s.x[0] = edeadlk; return; }
                value->changed.wait_for(lock, poll_period);
                if (self.stopping()) { self.stop_point(); s.x[0] = ecanceled; return; }
            }
            if (!value->complete) {
                value->owner = identity; lock.unlock();
                try {
                    static_cast<void>(EngineAdapter(self.process.calling_engine()).call(s.x[1]));
                    if (self.process.current_thread()->exit_value) EngineAdapter::require("thread exit from a nested guest callback");
                }
                catch (...) { lock.lock(); value->owner = 0; value->changed.notify_all(); throw; }
                lock.lock(); value->complete = true; value->owner = 0; value->changed.notify_all();
            }
            s.x[0] = 0;
        });
        add("_dispatch_once_f", [](auto& self, auto& s, auto& m) {
            const auto predicate = s.x[0], function = s.x[2], argument = s.x[1];
            const auto value = self.object(self.once, m, predicate, 8);
            if (!value || !function) throw std::runtime_error("invalid Darwin dispatch_once_f arguments");
            const auto identity = self.process.current_thread()->identity;
            std::unique_lock lock(value->gate);
            while (value->owner && !value->complete) {
                if (value->owner == identity) throw std::runtime_error("recursive Darwin dispatch_once_f initialization");
                value->changed.wait_for(lock, poll_period);
                if (self.stopping()) { self.stop_point(); EngineAdapter::require("cooperative guest stop during dispatch_once_f"); }
            }
            if (!value->complete) {
                value->owner = identity; lock.unlock();
                try { const std::array<std::uint64_t, 1> arguments{argument};
                    static_cast<void>(EngineAdapter(self.process.calling_engine()).call(function, arguments));
                    if (self.process.current_thread()->exit_value) EngineAdapter::require("thread exit from a nested guest callback");
                } catch (...) { lock.lock(); value->owner = 0; value->changed.notify_all(); throw; }
                lock.lock(); value->complete = true; value->owner = 0; m.write64(predicate, UINT64_MAX); value->changed.notify_all();
            }
            s.x[0] = 0;
        });
        add("_pthread_key_create", [](auto& self, auto& s, auto& m) {
            if (!accessible(m, s.x[0], 8)) { s.x[0] = einval; return; }
            std::lock_guard lock(self.metadata);
            if (self.keys.size() >= 512) { s.x[0] = eagain; return; }
            while (self.keys.contains(self.next_key)) ++self.next_key; // Skip framework-reserved keys.
            const auto key = self.next_key++; self.keys.emplace(key, s.x[1]); m.write64(s.x[0], key); s.x[0] = 0;
        });
        // pthread_key_init_np: a framework-reserved key (the Swift runtime's
        // __PTK_FRAMEWORK_SWIFT_KEY*), usable with get/setspecific.
        add("_pthread_key_init_np", [](auto& self, auto& s, auto&) {
            std::lock_guard lock(self.metadata);
            const auto key = static_cast<std::uint64_t>(static_cast<std::uint32_t>(s.x[0]));
            if (key == 0 || key >= 512) { s.x[0] = einval; return; }
            self.keys[key] = s.x[1]; s.x[0] = 0;
        });
        // os_unfair_lock: a 32-bit lock word, zero-initialized, owned by one
        // thread; contended waits block like pthread mutexes.
        add("_os_unfair_lock_lock", [](auto& self, auto& s, auto& m) {
            const auto result = self.mutex_lock(self.object(self.mutexes, m, s.x[0], 4), false);
            if (result) throw std::runtime_error("os_unfair_lock_lock failed (recursive acquisition or invalid lock)");
        });
        add("_os_unfair_lock_lock_with_options", [](auto& self, auto& s, auto& m) {
            const auto result = self.mutex_lock(self.object(self.mutexes, m, s.x[0], 4), false);
            if (result) throw std::runtime_error("os_unfair_lock_lock failed (recursive acquisition or invalid lock)");
        });
        add("_os_unfair_lock_trylock", [](auto& self, auto& s, auto& m) {
            s.x[0] = self.mutex_lock(self.object(self.mutexes, m, s.x[0], 4), true) == 0;
        });
        add("_os_unfair_lock_unlock", [](auto& self, auto& s, auto& m) {
            if (self.mutex_unlock(self.object(self.mutexes, m, s.x[0], 4)))
                throw std::runtime_error("os_unfair_lock_unlock of a lock not owned by this thread");
        });
        for (const auto must_own : {true, false}) add(must_own ? "_os_unfair_lock_assert_owner" : "_os_unfair_lock_assert_not_owner",
            [must_own](auto& self, auto& s, auto& m) {
                const auto value = self.object(self.mutexes, m, s.x[0], 4, false);
                if (!value) throw std::runtime_error("os_unfair_lock assertion on an invalid lock");
                const auto identity = self.process.current_thread()->identity;
                std::lock_guard lock(value->gate);
                if ((value->owner == identity) != must_own)
                    throw std::runtime_error(must_own ? "os_unfair_lock is not owned by the current thread" : "os_unfair_lock is owned by the current thread");
            });
        add("_pthread_key_delete", [](auto& self, auto& s, auto&) {
            std::lock_guard lock(self.metadata); s.x[0] = self.keys.erase(s.x[0]) != 0 ? 0 : einval;
        });
        add("_pthread_setspecific", [](auto& self, auto& s, auto&) {
            const auto thread = self.process.current_thread(); std::lock_guard lock(self.metadata);
            if (!self.keys.contains(s.x[0])) { s.x[0] = einval; return; }
            thread->key_values[s.x[0]] = s.x[1]; s.x[0] = 0;
        });
        add("_pthread_getspecific", [](auto& self, auto& s, auto&) {
            const auto thread = self.process.current_thread(); std::lock_guard lock(self.metadata);
            const auto value = thread->key_values.find(s.x[0]);
            s.x[0] = self.keys.contains(s.x[0]) && value != thread->key_values.end() ? value->second : 0;
        });
        add("___ulock_wait", [](auto& self, auto& s, auto& m) {
            const auto operation = static_cast<std::uint32_t>(s.x[0]);
            const auto address = s.x[1];
            const auto expected = s.x[2];
            const auto timeout = static_cast<std::uint32_t>(s.x[3]);
            const auto kind = operation & 255;
            if ((kind != 1 && kind != 5) || (operation & ~0x010000ffU) != 0)
                throw std::runtime_error("unsupported Darwin ulock wait operation");
            const auto fail = [&](std::uint32_t error) {
                if (operation & 0x01000000U) s.x[0] = static_cast<std::uint64_t>(-static_cast<std::int64_t>(error));
                else { self.process.set_errno(error); s.x[0] = UINT64_MAX; }
            };
            if (address % (kind == 5 ? 8 : 4) != 0) { fail(einval); return; }
            const auto deadline = timeout == 0 ? std::chrono::steady_clock::time_point::max() :
                std::chrono::steady_clock::now() + std::chrono::microseconds(timeout);
            // Shared file memory: woken from any process (alchemy/shared_wait.hpp).
            if (const auto key = self.shared_key(address)) {
                const auto result = shared_wait::wait(*key, [&] {
                    return kind == 5 ? m.read64(address) == expected : m.read32(address) == static_cast<std::uint32_t>(expected);
                }, deadline, [&self] { return self.stopping(); });
                if (result == shared_wait::Result::stopped) { self.stop_point(); fail(ecanceled); return; }
                if (result == shared_wait::Result::timed_out) { fail(etimedout); return; }
                s.x[0] = 0; return;
            }
            const auto value = self.object(self.address_waits, m, address, kind == 5 ? 8 : 4, false);
            if (!value) { fail(14); return; }
            std::unique_lock lock(value->gate);
            if (value->waiters != 0 && value->operation != kind) { fail(33); return; }
            value->operation = kind;
            const auto matches = [&] { return kind == 5 ? m.read64(address) == expected :
                m.read32(address) == static_cast<std::uint32_t>(expected); };
            if (!matches()) { s.x[0] = 0; return; }
            const auto generation = value->generation;
            ++value->waiters;
            std::uint32_t error = 0;
            try {
                while (generation == value->generation && value->tickets == 0 &&
                    matches()) {
                    value->changed.wait_for(lock, poll_period);
                    if (std::chrono::steady_clock::now() >= deadline) { error = etimedout; break; }
                    if (self.stopping()) { error = ecanceled; break; }
                }
            } catch (...) { --value->waiters; throw; }
            if (generation == value->generation && value->tickets != 0) --value->tickets;
            --value->waiters;
            if (value->waiters == 0) value->tickets = 0;
            if (error == ecanceled) { lock.unlock(); self.stop_point(); }
            if (error) fail(error);
            else s.x[0] = value->waiters;
        });
        add("___ulock_wake", [](auto& self, auto& s, auto& m) {
            const auto operation = static_cast<std::uint32_t>(s.x[0]);
            const auto kind = operation & 255;
            if ((kind != 1 && kind != 5) || (operation & ~0x010001ffU) != 0 || s.x[2] != 0)
                throw std::runtime_error("unsupported Darwin ulock wake operation");
            const auto fail = [&](std::uint32_t error) {
                if (operation & 0x01000000U) s.x[0] = static_cast<std::uint64_t>(-static_cast<std::int64_t>(error));
                else { self.process.set_errno(error); s.x[0] = UINT64_MAX; }
            };
            if (s.x[1] % (kind == 5 ? 8 : 4) != 0) { fail(einval); return; }
            if (const auto key = self.shared_key(s.x[1])) {
                if (!shared_wait::wake(*key, (operation & 0x100) != 0)) fail(2);
                else s.x[0] = 0;
                return;
            }
            const auto value = self.object(self.address_waits, m, s.x[1], kind == 5 ? 8 : 4, false);
            if (!value) { fail(14); return; }
            std::lock_guard lock(value->gate);
            if (value->waiters == 0) { fail(2); return; }
            if (value->operation != kind) { fail(33); return; }
            if (operation & 0x100) { ++value->generation; value->tickets = 0; value->changed.notify_all(); }
            else if (value->tickets < value->waiters) { ++value->tickets; value->changed.notify_one(); }
            s.x[0] = 0;
        });
        if constexpr (engine_has_thread_pointer<transmute::ReferenceEngine>) {
            // Descriptors point at the guest fast path; the host handler, under
            // an internal name, is its slow path.
            const auto host = process.install_function("__alchemy_tlv_bootstrap_slow_path",
                [owned](auto& state, auto& memory) { owned->tlv(state, memory); }, "alchemy.internal");
            process.install_export("/usr/lib/libSystem.B.dylib", "__tlv_bootstrap", {tlv_fast_path(host)});
        } else {
            add("__tlv_bootstrap", [](auto& self, auto& s, auto& m) { self.tlv(s, m); });
        }
        for (const auto name : {"___cxa_thread_atexit", "___cxa_thread_atexit_impl", "__tlv_atexit"}) {
            add(name, [](auto& self, auto& s, auto&) {
                if (s.x[0] == 0) { s.x[0] = einval; return; }
                self.process.current_thread()->finalizers.emplace_back(s.x[0], s.x[1]); s.x[0] = 0;
            });
        }
        add("___cxa_guard_acquire", [](auto& self, auto& s, auto& m) {
            const auto value = self.object(self.guards, m, s.x[0], 8);
            if (!value) throw std::runtime_error("invalid C++ guard address");
            const auto identity = self.process.current_thread()->identity;
            std::unique_lock lock(value->gate);
            if (m.read8(s.x[0]) != 0) value->complete = true;
            while (value->owner != 0 && !value->complete) {
                if (value->owner == identity) throw std::runtime_error("recursive C++ local static initialization");
                value->changed.wait_for(lock, poll_period);
                if (self.stopping()) { self.stop_point(); EngineAdapter::require("cooperative guest thread stop during C++ guard wait"); }
            }
            if (value->complete) s.x[0] = 0;
            else { value->owner = identity; s.x[0] = 1; }
        });
        for (const auto& [name, complete] : std::array<std::pair<const char*, bool>, 2>{{{"___cxa_guard_release", true}, {"___cxa_guard_abort", false}}}) {
            add(name, [complete](auto& self, auto& s, auto& m) {
                const auto value = self.object(self.guards, m, s.x[0], 8);
                if (!value) throw std::runtime_error("invalid C++ guard address");
                std::lock_guard lock(value->gate);
                if (value->owner != self.process.current_thread()->identity) throw std::runtime_error("C++ guard released by wrong thread");
                value->complete = complete; value->owner = 0;
                if (complete) m.write8(s.x[0], 1);
                value->changed.notify_all(); s.x[0] = 0;
            });
        }

        // Private replacement code terminates the current engine through its
        // existing supervisor-call outcome. This is not a Darwin syscall.
        exit_shim = process.allocate_guest(16);
        process.main_engine().write32(exit_shim, 0xd4001421); // svc #0xa1
        process.main_engine().write32(exit_shim + 4, 0xd4200000); // brk if handler incorrectly resumes
        process.main_engine().set_memory_access(exit_shim, true, false, true);
        process.install_export("/usr/lib/libSystem.B.dylib", "_pthread_exit", {exit_shim, ExportKind::function});
        process.syscall_extension = [owned](const auto& call, auto& state, auto&) -> std::optional<std::uint32_t> {
            if (call.immediate != 0xa1) return std::nullopt;
            owned->process.current_thread()->exit_value = state.x[0];
            return 0;
        };
        process.join_workers = [owned] { owned->join_all(); };
        process.cleanup_thread = [owned](auto& thread) { owned->cleanup(thread); };
    }
};

bool guest_worker_must_stop(ProcessContext& context) {
    auto& engine = context.calling_engine();
    return (context.requested_exit().has_value() || engine_stop_requested(engine)) && &engine != &context.main_engine();
}

DarwinThreads::DarwinThreads(ProcessContext& context) : impl_(std::make_shared<Impl>(context)) { impl_->install(); }
DarwinThreads::~DarwinThreads() {
    try { impl_->join_all(); } catch (...) { /* Explicit joins report guest failures. */ }
    impl_->process.join_workers = {};
    impl_->process.cleanup_thread = {};
    impl_->process.syscall_extension = {};
}
void DarwinThreads::join_workers() { impl_->join_all(); }
void DarwinThreads::cleanup_thread(ThreadContext& thread) { impl_->cleanup(thread); }

} // namespace alchemy
