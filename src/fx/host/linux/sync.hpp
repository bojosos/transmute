#pragma once

// linux backend of fx/host/sync.hpp. RwLock is a glibc rwlock that prefers
// writers, like SRWLOCK: a waiting writer holds back new readers (so shared
// locking must not recurse, as with SRW). CondVar is a futex sequence word,
// usable with the lock held either way (pthread condition variables need a
// mutex).

#include <pthread.h>

#include <atomic>
#include <cstdint>

namespace transmute::fx::host {

class RwLock {
public:
    constexpr RwLock() noexcept = default;
    RwLock(const RwLock&) = delete;
    RwLock& operator=(const RwLock&) = delete;

    void lock() noexcept { pthread_rwlock_wrlock(&lock_); }
    void unlock() noexcept { pthread_rwlock_unlock(&lock_); }
    bool try_lock() noexcept { return pthread_rwlock_trywrlock(&lock_) == 0; }
    void lock_shared() noexcept { pthread_rwlock_rdlock(&lock_); }
    void unlock_shared() noexcept { pthread_rwlock_unlock(&lock_); }
    bool try_lock_shared() noexcept { return pthread_rwlock_tryrdlock(&lock_) == 0; }

private:
    pthread_rwlock_t lock_ = PTHREAD_RWLOCK_WRITER_NONRECURSIVE_INITIALIZER_NP;
};

class CondVar {
public:
    constexpr CondVar() noexcept = default;
    CondVar(const CondVar&) = delete;
    CondVar& operator=(const CondVar&) = delete;

    // `lock` is held exclusively; it is released while waiting and held again
    // on return. False if `milliseconds` passed (0xffffffff: no timeout).
    bool wait_for(RwLock& lock, unsigned long milliseconds) noexcept { return wait(lock, milliseconds, false); }
    // The same with `lock` held shared.
    bool wait_shared_for(RwLock& lock, unsigned long milliseconds) noexcept { return wait(lock, milliseconds, true); }
    void notify_one() noexcept;
    void notify_all() noexcept;

private:
    bool wait(RwLock& lock, unsigned long milliseconds, bool shared) noexcept;
    std::atomic<std::uint32_t> sequence_{0};
};

// An auto-reset event, for the profiler's sampler thread.
class AutoResetEvent {
public:
    AutoResetEvent() noexcept;
    ~AutoResetEvent();
    AutoResetEvent(const AutoResetEvent&) = delete;
    AutoResetEvent& operator=(const AutoResetEvent&) = delete;
    void set() noexcept;
    // False on timeout.
    bool wait_for(unsigned long milliseconds) noexcept;

private:
    std::atomic<std::uint32_t> signaled_{0};
};

} // namespace transmute::fx::host
