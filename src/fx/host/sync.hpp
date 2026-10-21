#pragma once

// Host area "sync": the engine's locks (docs/FX_HOST_SEAMS.md, "sync").
//
// RwLock      reader-writer lock, not recursive, constant-initialized (a
//             namespace-scope RwLock needs no constructor at start-up) and
//             never destroyed in a way that matters: a terminated thread may
//             leave one held, which runtime_exit_safe() detects with
//             try_lock().
//               lock()/unlock(), lock_shared()/unlock_shared(),
//               try_lock(), try_lock_shared()
// CondVar     condition variable used with an RwLock.
//               wait_for(lock, milliseconds) -> false on timeout (lock held exclusively)
//               wait_shared_for(lock, milliseconds)              (lock held shared)
//               notify_one(), notify_all()
// AutoResetEvent  set(), wait_for(milliseconds) -> false on timeout; one
//             waiter is released per set() (out of line: diagnostics only).
// SharedLock, ExclusiveLock
//             scope guards over an RwLock.
//
// Every backend defines these classes in <backend>/sync.hpp, inline: the
// translated-code lookup takes an RwLock on every miss.

#include "fx/host/host.hpp"

#if defined(TRANSMUTE_FX_HOST_WIN32)
#include "fx/host/win32/sync.hpp"
#endif

namespace transmute::fx::host {

// CondVar::wait_for without a timeout.
inline constexpr unsigned long kWaitForever = 0xffffffffu;

class SharedLock {
public:
    explicit SharedLock(RwLock& lock) noexcept : lock_(lock) { lock_.lock_shared(); }
    ~SharedLock() { lock_.unlock_shared(); }
    SharedLock(const SharedLock&) = delete;
    SharedLock& operator=(const SharedLock&) = delete;
private:
    RwLock& lock_;
};

class ExclusiveLock {
public:
    explicit ExclusiveLock(RwLock& lock) noexcept : lock_(lock) { lock_.lock(); }
    ~ExclusiveLock() { lock_.unlock(); }
    ExclusiveLock(const ExclusiveLock&) = delete;
    ExclusiveLock& operator=(const ExclusiveLock&) = delete;
private:
    RwLock& lock_;
};

} // namespace transmute::fx::host
