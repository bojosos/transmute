#include "alchemy/darwin/kit/export_fill/export_fill.hpp"
#include "alchemy/darwin/libsystem/install.hpp"

#include <array>
#include <map>
#include <memory>
#include <mutex>

// pthread names beyond threads.cpp's core: QoS queries, stack attributes, concurrency levels,
// priority ceilings, JIT write callbacks, and the unnamed-semaphore calls Darwin itself does
// not implement. Fill-only (install_libsystem_gaps).
namespace alchemy::export_fill {
namespace {

constexpr std::uint64_t pthread_attr_magic = 0x5448524441545452ULL;   // darwin_threads' initialized attribute
constexpr std::uint64_t qos_user_interactive = 0x21, qos_default = 0x15;

struct Threads {
    std::mutex mutex;
    std::map<std::uint64_t, std::int32_t> ceilings;   // pthread_mutex_t* or pthread_mutexattr_t* -> priority ceiling
    std::int32_t concurrency{};
};

} // namespace

void install_libsystem_threads_extra(Provider& p) {
    const auto state = std::make_shared<Threads>();
    // qos_class_t qos_class_self(void) / qos_class_main(void): the main thread runs at
    // USER_INTERACTIVE, other threads at DEFAULT (QoS is a scheduling hint here).
    p.libsystem("_qos_class_main", [](CpuState&, GuestMemory&) -> std::uint64_t { return qos_user_interactive; });
    p.libsystem("_qos_class_self", [p](CpuState&, GuestMemory&) -> std::uint64_t {
        return p.call(libsystem_library, "_pthread_main_np", {}) ? qos_user_interactive : qos_default;
    });
    // Caller-provided thread stacks are not supported (Alchemy's engine allocates guest stacks):
    // pthread_attr_setstack and _setstackaddr fail with ENOTSUP; the getters report the size
    // threads get and no address.
    for (const auto* name : {"_pthread_attr_setstack", "_pthread_attr_setstackaddr"})
        p.libsystem(name, [](CpuState& s, GuestMemory& m) -> std::uint64_t {
            if (!s.x[0] || m.read64(s.x[0]) != pthread_attr_magic) return einval;
            return enotsup;
        });
    p.libsystem("_pthread_attr_getstack", [p](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (!s.x[0] || !s.x[1] || !s.x[2] || m.read64(s.x[0]) != pthread_attr_magic) return einval;
        if (const auto result = p.call(libsystem_library, "_pthread_attr_getstacksize", {s.x[0], s.x[2]})) return result;
        m.write64(s.x[1], 0);
        return 0;
    });
    p.libsystem("_pthread_attr_getstackaddr", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (!s.x[0] || !s.x[1] || m.read64(s.x[0]) != pthread_attr_magic) return einval;
        m.write64(s.x[1], 0);
        return 0;
    });
    // int pthread_setschedprio(pthread_t, int): pthread_setschedparam with SCHED_OTHER.
    p.libsystem("_pthread_setschedprio", [p](CpuState& s, GuestMemory&) -> std::uint64_t {
        auto& process = p.process();
        const auto parameter = process.allocate_guest(8);
        process.calling_engine().write32(parameter, static_cast<std::uint32_t>(s.x[1]));
        const auto result = p.call(libsystem_library, "_pthread_setschedparam", {s.x[0], 1, parameter});
        process.release_guest(parameter);
        return result;
    });
    // pthread_getconcurrency / pthread_setconcurrency: the level is a hint, kept and reported.
    p.libsystem("_pthread_getconcurrency", [state](CpuState&, GuestMemory&) -> std::uint64_t {
        std::lock_guard lock(state->mutex); return static_cast<std::uint32_t>(state->concurrency);
    });
    p.libsystem("_pthread_setconcurrency", [state](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto level = static_cast<std::int32_t>(s.x[0]);
        if (level < 0) return einval;
        std::lock_guard lock(state->mutex); state->concurrency = level; return 0;
    });
    // Priority ceilings (PTHREAD_PRIO_PROTECT) are stored and reported; Alchemy mutexes do not
    // raise priorities. pthread_mutex_setprioceiling returns the previous ceiling.
    for (const auto* name : {"_pthread_mutexattr_getprioceiling", "_pthread_mutex_getprioceiling"})
        p.libsystem(name, [state](CpuState& s, GuestMemory& m) -> std::uint64_t {
            if (!s.x[0] || !s.x[1]) return einval;
            std::lock_guard lock(state->mutex);
            const auto found = state->ceilings.find(s.x[0]);
            m.write32(s.x[1], static_cast<std::uint32_t>(found == state->ceilings.end() ? 0 : found->second));
            return 0;
        });
    p.libsystem("_pthread_mutexattr_setprioceiling", [state](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto ceiling = static_cast<std::int32_t>(s.x[1]);
        if (!s.x[0] || ceiling < 0 || ceiling > 63) return einval;
        std::lock_guard lock(state->mutex); state->ceilings[s.x[0]] = ceiling; return 0;
    });
    p.libsystem("_pthread_mutex_setprioceiling", [state](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto ceiling = static_cast<std::int32_t>(s.x[1]);
        if (!s.x[0] || ceiling < 0 || ceiling > 63) return einval;
        std::lock_guard lock(state->mutex);
        auto& slot = state->ceilings[s.x[0]];
        if (s.x[2]) m.write32(s.x[2], static_cast<std::uint32_t>(slot));
        slot = ceiling;
        return 0;
    });
    // pthread_cond_signal_thread_np(cond, thread): wakes the waiters (POSIX allows the others'
    // wakeups as spurious), or one of them when thread is NULL.
    p.libsystem("_pthread_cond_signal_thread_np", [p](CpuState& s, GuestMemory&) -> std::uint64_t {
        return p.call(libsystem_library, s.x[1] ? "_pthread_cond_broadcast" : "_pthread_cond_signal", {s.x[0]});
    });
    // JIT memory is writable and executable for every thread here, so the write window of
    // pthread_jit_write_with_callback_np is always open: the callback runs and its result is
    // returned. Freezing the callback list changes nothing.
    p.libsystem("_pthread_jit_write_with_callback_np", [p](CpuState& s, GuestMemory&) -> std::uint64_t {
        if (!s.x[0]) return einval;
        return EngineAdapter(p.process().calling_engine()).call(s.x[0], std::array<std::uint64_t, 1>{s.x[1]}) & 0xffffffffULL;
    });
    p.libsystem("_pthread_jit_write_freeze_callbacks_np", [](CpuState&, GuestMemory&) -> std::uint64_t { return 0; });
    // Darwin has no unnamed POSIX semaphores: sem_init, sem_destroy and sem_getvalue fail with ENOSYS.
    for (const auto* name : {"_sem_init", "_sem_destroy", "_sem_getvalue"})
        p.libsystem(name, [](CpuState&, GuestMemory&) -> std::uint64_t { throw Errno{enosys}; });
}

} // namespace alchemy::export_fill
