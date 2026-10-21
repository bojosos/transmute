#pragma once

// POSIX record locks (fcntl F_GETLK, F_SETLK, F_SETLKW) shared by every
// Alchemy process. Windows byte-range locks are mandatory (they block other
// handles' reads and writes) and cannot name their owner, so the locks live
// in a table of their own: one named shared-memory table per file (keyed by
// volume serial number and file id, so every path and hard link of the file
// meets the same table), guarded by a named mutex.
//
// Semantics follow POSIX: locks belong to a process, never conflict with the
// same process's own locks, replace and split its overlapping ranges, and all
// of a process's locks on a file go when it closes any descriptor of that
// file (DarwinServices calls release_all) or ends (entries of processes that
// no longer run are dropped). F_SETLKW waits by polling, so the caller can
// stop on process exit. Independent of flock(2) locks.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace alchemy::record_locks {

enum : int { read_lock = 1, unlock = 2, write_lock = 3 };   // F_RDLCK, F_UNLCK, F_WRLCK

constexpr std::int64_t to_end = INT64_MAX;   // range end of a lock with l_len 0

struct Conflict {
    int type{};
    std::int64_t start{}, end{};   // end exclusive; to_end for "to the end of the file"
    std::uint32_t pid{};           // guest pid of the owner
};

// The table key of an open host file (a HANDLE on Windows, a descriptor
// elsewhere); empty when the file has no stable identity.
std::string file_key(std::intptr_t native);

// Applies `type` to [start, end) for this process. 0 on success, else a
// Darwin errno: 35 (EAGAIN) on a conflict without `wait`, 77 (ENOLCK) when
// the table is full, 89 (ECANCELED) when `stop()` asks a wait to end.
int set(const std::string& key, std::int64_t start, std::int64_t end, int type, bool wait,
        const std::function<bool()>& stop = {});

// The first lock of another process that conflicts with `type` on [start, end).
std::optional<Conflict> test(const std::string& key, std::int64_t start, std::int64_t end, int type);

// Drops every lock this process holds on the file.
void release_all(const std::string& key);

// Whether this process holds any record lock (a cheap check for close()).
bool any_held() noexcept;

} // namespace alchemy::record_locks
