#pragma once

// The Alchemy process tree: what one Alchemy process can learn about another
// (docs/ALCHEMY_CHROME_AUDIT.md stage 5). Each process publishes a record
// (pid, parent, group, user, generation, start time, guest executable) in a
// named host object that lives exactly as long as the process. Any other
// Alchemy process reads it by pid (proc_pidinfo, proc_pidpath, proc_name,
// sysctl KERN_PROC_PID, kill(pid, 0), dispatch PROC sources), and Mach IPC
// keys remote processes and their dead-name notifications by it.
//
// Guest pids are host process ids: getpid() in a child equals the pid
// posix_spawn returned to its parent, and exec in place keeps the pid (the
// new image republishes the record under the same pid). A non-Alchemy host
// process is not part of the tree and is never found.
//
// pidversion is the generation of a pid, as in audit_token_t: a nonzero
// 31-bit value derived from the creation time of the host process that
// published the record, so every process computes the same value and a
// reused pid (or an exec'd image) has a different one.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace alchemy::process_tree {

struct Record {
    std::uint32_t pid{};          // guest pid
    std::uint32_t ppid{};         // guest parent pid (1 for a process started from the host)
    std::uint32_t pgid{};
    std::uint32_t uid{};
    std::uint32_t gid{};
    std::uint32_t pidversion{};
    std::uint32_t host_pid{};     // the host process running the image (differs from pid after exec in place)
    std::uint64_t start_seconds{}, start_microseconds{};   // Unix time the image started
    std::string path;             // guest path of the executable ("" when unknown)
    std::string name;             // its last component
};

// Publishes (or updates) this process's record. `record.pidversion`,
// `host_pid` and the start time are filled in when zero. The record lasts
// until this process ends. Returns the record as published.
Record publish(Record record);

// This process's published record, if any.
std::optional<Record> self();

// The record of a live Alchemy process.
std::optional<Record> lookup(std::uint32_t pid);

// Generation of a live Alchemy process; 0 when `pid` is not one.
std::uint32_t pidversion(std::uint32_t pid);

// Whether `pid` is a live Alchemy process (with that generation, when
// `version` is nonzero).
bool alive(std::uint32_t pid, std::uint32_t version = 0);

// Calls `on_exit(pid)` once when the process ends: on a host thread-pool
// thread (never a guest thread), and at once (on the caller's thread) when
// the process is already gone or has a different generation. Returns a token
// for unwatch_exit, or 0 when `on_exit` already ran.
std::uint64_t watch_exit(std::uint32_t pid, std::uint32_t version, std::function<void(std::uint32_t pid)> on_exit);

// Cancels a watch; when it returns, the callback is not running and never
// runs. Must not be called from that watch's own callback. False when the
// token is unknown (already fired or cancelled).
bool unwatch_exit(std::uint64_t token);

} // namespace alchemy::process_tree
