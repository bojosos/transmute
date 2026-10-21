#pragma once

// win32 backend of fx/host/sync.hpp: SRWLOCK and CONDITION_VARIABLE, inline.

#include "fx/host/win32/kernel32.hpp"

namespace transmute::fx::host {

class RwLock {
public:
    constexpr RwLock() noexcept = default;
    RwLock(const RwLock&) = delete;
    RwLock& operator=(const RwLock&) = delete;

    void lock() noexcept { AcquireSRWLockExclusive(native()); }
    void unlock() noexcept { ReleaseSRWLockExclusive(native()); }
    bool try_lock() noexcept { return TryAcquireSRWLockExclusive(native()) != 0; }
    void lock_shared() noexcept { AcquireSRWLockShared(native()); }
    void unlock_shared() noexcept { ReleaseSRWLockShared(native()); }
    bool try_lock_shared() noexcept { return TryAcquireSRWLockShared(native()) != 0; }

    _RTL_SRWLOCK* native() noexcept { return reinterpret_cast<_RTL_SRWLOCK*>(&state_); }

private:
    void* state_ = nullptr;   // SRWLOCK_INIT
};

class CondVar {
public:
    constexpr CondVar() noexcept = default;
    CondVar(const CondVar&) = delete;
    CondVar& operator=(const CondVar&) = delete;

    // `lock` is held exclusively; it is released while waiting and held again
    // on return. False if `milliseconds` passed (0xffffffff: no timeout).
    bool wait_for(RwLock& lock, unsigned long milliseconds) noexcept {
        return SleepConditionVariableSRW(native(), lock.native(), milliseconds, 0) != 0;
    }
    // The same with `lock` held shared (CONDITION_VARIABLE_LOCKMODE_SHARED).
    bool wait_shared_for(RwLock& lock, unsigned long milliseconds) noexcept {
        return SleepConditionVariableSRW(native(), lock.native(), milliseconds, 1) != 0;
    }
    void notify_one() noexcept { WakeConditionVariable(native()); }
    void notify_all() noexcept { WakeAllConditionVariable(native()); }

private:
    _RTL_CONDITION_VARIABLE* native() noexcept { return reinterpret_cast<_RTL_CONDITION_VARIABLE*>(&state_); }
    void* state_ = nullptr;   // CONDITION_VARIABLE_INIT
};

// An auto-reset event (CreateEventW), for the profiler's sampler thread.
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
    void* handle_;
};

} // namespace transmute::fx::host
