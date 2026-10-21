#pragma once

// Cross-process waits on shared guest memory (owner: process model).
//
// Guest memory mapped with mmap(MAP_SHARED) (a host file section, see
// DarwinServices' map_shared_file) is seen by every Alchemy process that maps
// the same file. Waits on such memory (__ulock_wait/__ulock_wake and the
// PTHREAD_PROCESS_SHARED mutexes and condition variables of
// darwin_threads.cpp) must be woken from any of those processes, so they are
// keyed by the memory's process-independent name
// (ProcessContext::SharedLocation: host file identity and byte offset), never
// by the guest address.
//
// Implementation (Windows): one session-wide table of wait slots in a named
// file mapping guarded by a named mutex ("Local\alchemy.shared-waits.v1*"),
// and one named semaphore per slot. A waiter registers in its key's slot and
// waits on the semaphore; a waker releases one permit (or one per registered
// waiter). Waiters also re-check their condition every few milliseconds, so a
// wake can only be early (spurious), never lost; when the last waiter of a
// slot leaves, its leftover permits are drained and the slot is freed.

#include <chrono>
#include <cstdint>
#include <functional>
#include <string_view>

namespace alchemy::shared_wait {

enum class Result { woken, changed, timed_out, stopped };

// Blocks while `blocked()` returns true, until a wake on `key`, the deadline,
// or `stop()` returns true. `blocked` is re-evaluated after registration (so
// a change made before the wait is never missed) and periodically.
Result wait(std::string_view key, const std::function<bool()>& blocked,
            std::chrono::steady_clock::time_point deadline, const std::function<bool()>& stop);

// Wakes one (or every) waiter registered on `key` in any process. Returns
// false when there was none.
bool wake(std::string_view key, bool all);

// A 32-bit owner token for process-shared mutexes, unique across the host
// processes of the session while the calling thread lives (the Windows
// thread id; guest threads are host threads in alchemy-fx).
std::uint32_t owner_token();

} // namespace alchemy::shared_wait
