// linux backend of fx/host/thread.hpp.
//
// The thread exit hook must run before the thread's C++ thread_local
// destructors (thread.hpp). glibc runs those before pthread key destructors,
// so threads started here run the hook themselves, when their entry returns
// or calls exit_thread. Other threads that run guest code (borrowed ones) get
// it from a pthread key destructor, armed by copy_thread_block, which the
// runtime calls once per guest thread state: later than on Windows, after
// their thread_locals are gone.
#include "fx/host/thread.hpp"
#include "fx/host/linux/internal.hpp"

#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>
#include <thread>

namespace transmute::fx::host {

namespace {

// A thread this backend started.
struct Thread {
    pthread_t thread{};
    ThreadEntry entry{};
    void* argument{};
    std::atomic<std::uint32_t> id{0};   // the kernel thread id, once the thread runs
    std::atomic<int> references{2};     // the creator's handle and the thread itself
};

// set_last_error's value, kept exactly while errno still says what it set
// (a Win32 code without an errno equivalent would not survive the mapping).
thread_local std::uint32_t t_last_code = 0;
thread_local int t_last_errno = -1;

// current_thread()'s pseudo handle (never closed).
Thread* const kCurrent = reinterpret_cast<Thread*>(~std::uintptr_t{1});

std::atomic<void (*)()> g_thread_exit_hook{nullptr};
pthread_key_t g_exit_key;
std::once_flag g_exit_key_once;

void release(Thread* thread) noexcept {
    if (thread->references.fetch_sub(1, std::memory_order_acq_rel) == 1) delete thread;
}

void run_exit_hook() noexcept {
    if (const auto hook = g_thread_exit_hook.load(std::memory_order_acquire)) hook();
}

void key_destructor(void*) { run_exit_hook(); }

void arm_exit_key() noexcept {
    std::call_once(g_exit_key_once, [] { pthread_key_create(&g_exit_key, &key_destructor); });
    if (!pthread_getspecific(g_exit_key)) pthread_setspecific(g_exit_key, reinterpret_cast<void*>(1));
}

void* thread_main(void* raw) {
    auto* thread = static_cast<Thread*>(raw);
    linux_internal::ensure_signal_stack();
    thread->id.store(current_thread_id(), std::memory_order_release);
    const auto entry = thread->entry;
    void* argument = thread->argument;
    release(thread);
    const std::uint32_t code = entry(argument);
    run_exit_hook();
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(code));
}

pid_t native_id(ThreadHandle handle) noexcept {
    // Priorities and affinity are per kernel thread; other threads' ids are
    // not kept, so only the calling thread's settings apply.
    return handle == kCurrent ? static_cast<pid_t>(current_thread_id()) : 0;
}

// thread pool: a few lazily started workers.
struct Pool {
    std::mutex lock;
    std::condition_variable ready;
    std::deque<std::pair<void (*)(void*), void*>> work;
    unsigned workers{};
    unsigned idle{};
};
Pool& pool() {
    static auto* p = new Pool;   // never destroyed: workers outlive static destruction
    return *p;
}

void pool_worker() {
    auto& p = pool();
    std::unique_lock lock(p.lock);
    for (;;) {
        ++p.idle;
        p.ready.wait(lock, [&] { return !p.work.empty(); });
        --p.idle;
        const auto item = p.work.front();
        p.work.pop_front();
        lock.unlock();
        item.first(item.second);
        lock.lock();
    }
}

} // namespace

namespace linux_thread {
std::uint32_t read_thread_id() noexcept { return static_cast<std::uint32_t>(syscall(SYS_gettid)); }

std::uint64_t read_stack_base() noexcept {
    pthread_attr_t attributes;
    if (pthread_getattr_np(pthread_self(), &attributes) != 0) return 0;
    void* low = nullptr;
    std::size_t size = 0;
    pthread_attr_getstack(&attributes, &low, &size);
    pthread_attr_destroy(&attributes);
    return reinterpret_cast<std::uint64_t>(low) + size;
}
} // namespace linux_thread

ThreadHandle create_thread(ThreadEntry entry, void* argument, std::size_t stack_size, std::uint32_t flags,
                           std::uint32_t* thread_id) noexcept {
    if (flags & kThreadSuspended) {   // nothing could resume it: no seam has ResumeThread
        set_last_error(87);           // ERROR_INVALID_PARAMETER
        return nullptr;
    }
    auto* thread = new (std::nothrow) Thread;
    if (!thread) return nullptr;
    thread->entry = entry;
    thread->argument = argument;
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    if (stack_size) pthread_attr_setstacksize(&attributes, std::max<std::size_t>(stack_size, PTHREAD_STACK_MIN));
    const int error = pthread_create(&thread->thread, &attributes, &thread_main, thread);
    pthread_attr_destroy(&attributes);
    if (error != 0) {
        errno = error;
        delete thread;
        return nullptr;
    }
    // The kernel id is the thread's own to read: wait the moment it takes to start.
    if (thread_id) {
        std::uint32_t id = 0;
        while ((id = thread->id.load(std::memory_order_acquire)) == 0) sched_yield();
        *thread_id = id;
    }
    return thread;
}

void close_thread(ThreadHandle handle) noexcept {
    auto* thread = static_cast<Thread*>(handle);
    if (!thread || thread == kCurrent) return;
    pthread_detach(thread->thread);
    release(thread);
}

ThreadHandle current_thread() noexcept { return kCurrent; }

void set_thread_priority(ThreadHandle handle, ThreadPriority priority) noexcept {
    const pid_t id = native_id(handle);
    if (!id) return;
    int nice = 0;
    switch (priority) {
    case ThreadPriority::normal: nice = 0; break;
    case ThreadPriority::below_normal: nice = 5; break;
    case ThreadPriority::idle: nice = 19; break;
    case ThreadPriority::above_normal: nice = -5; break;   // needs privilege; refused quietly
    case ThreadPriority::highest: nice = -10; break;
    }
    setpriority(PRIO_PROCESS, static_cast<id_t>(id), nice);
}

void set_thread_affinity(ThreadHandle handle, std::uint64_t mask) noexcept {
    const pid_t id = native_id(handle);
    if (!id) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    for (unsigned i = 0; i < 64; ++i)
        if (mask & (std::uint64_t{1} << i)) CPU_SET(i, &set);
    sched_setaffinity(id, sizeof(set), &set);
}

void set_thread_eco_qos(ThreadHandle) noexcept {}

void exit_thread(std::uint32_t exit_code) noexcept {
    run_exit_hook();
    pthread_exit(reinterpret_cast<void*>(static_cast<std::uintptr_t>(exit_code)));
}

void yield_thread() noexcept { sched_yield(); }

bool submit_to_thread_pool(void (*work)(void* context), void* context) noexcept {
    try {
        auto& p = pool();
        std::lock_guard lock(p.lock);
        p.work.emplace_back(work, context);
        if (p.idle == 0 && p.workers < 4) {
            std::thread(&pool_worker).detach();
            ++p.workers;
        }
        p.ready.notify_one();
        return true;
    } catch (...) {
        return false;
    }
}

std::uint32_t last_error() noexcept {
    if (errno == t_last_errno) return t_last_code;
    return linux_internal::win32_error(errno);
}
void set_last_error(std::uint32_t error) noexcept {
    errno = linux_internal::errno_of(error);
    t_last_errno = errno;
    t_last_code = error;
}

std::size_t copy_thread_block(void*, std::size_t) noexcept {
    arm_exit_key();
    return 0;
}

std::uintptr_t activate_context(void*) noexcept { return 0; }
void deactivate_context(std::uintptr_t) noexcept {}

void set_thread_exit_hook(void (*hook)()) noexcept { g_thread_exit_hook.store(hook, std::memory_order_release); }

} // namespace transmute::fx::host
