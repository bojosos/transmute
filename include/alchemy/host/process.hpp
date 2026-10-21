#pragma once

// Host area "process" (owner: r1). The rules are in include/alchemy/host/host.hpp. Processes:
// spawn, wait, exit watching, pids, environment, standard handles, host modules.

#include "alchemy/host/host.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy::host::process {

// The host process id of this process.
[[nodiscard]] std::uint32_t current_pid() noexcept;
// The host thread id of the calling thread: never 0, distinct among live threads.
[[nodiscard]] std::uint64_t current_thread_id() noexcept;

// This process's standard input, output or error (borrowed: never close it);
// an empty Handle when the process has none.
enum class Standard : std::uint8_t { input, output, error };
[[nodiscard]] Handle standard_handle(Standard which) noexcept;

// CPU time and memory of a host process (getrusage, task_info, proc_pidinfo).
struct Usage {
    std::uint64_t user_ns{}, system_ns{};     // CPU time
    std::uint64_t resident{}, resident_peak{}; // bytes in physical memory (working set) and its peak
    std::uint64_t private_bytes{};            // committed private memory (the footprint)
    std::uint64_t virtual_size{};             // address space in use (this process only; 0 for others)
    std::uint64_t page_faults{};
};
// `host_pid` 0 is this process. A process this one may not query fails
// with Error::not_found or Error::access_denied.
[[nodiscard]] Result<Usage> usage(std::uint32_t host_pid = 0);
// CPU time of the calling thread (thread_info THREAD_BASIC_INFO).
struct ThreadUsage {
    std::uint64_t user_ns{}, system_ns{};
};
[[nodiscard]] Result<ThreadUsage> thread_usage();
// This process's memory: its private committed bytes, and the committed bytes of
// the host heap that malloc and new use (diagnostics: ALCHEMY_FX_STATS).
struct MemoryUsage {
    std::uint64_t private_bytes{}, heap_committed_bytes{};
};
[[nodiscard]] Result<MemoryUsage> memory_usage();

// The host processes now running (proc_listpids, sysctl KERN_PROC).
struct Entry { std::uint32_t pid{}, parent{}; };
[[nodiscard]] Result<std::vector<Entry>> list();
// A host environment variable (UTF-8); empty when unset.
[[nodiscard]] std::optional<std::string> environment(std::string_view name);
// Sets (or, with no value, removes) a host environment variable of this
// process, inherited by the children it starts.
[[nodiscard]] Status set_environment(std::string_view name, std::optional<std::string_view> value);
// The whole host environment of this process, as "NAME=value" (UTF-8).
[[nodiscard]] std::vector<std::string> environment_block();

// Child and other host processes. A process Handle (from spawning) is
// signalled for sync::wait_any when the process ends; close it with
// host::close.
// The exit code of an ended process; Error::busy while it still runs.
[[nodiscard]] Result<std::uint32_t> exit_code(Handle process);
// Ends a process with `exit_code` (kill(2) of a process that cannot run a
// handler): by handle, or by host pid.
[[nodiscard]] Status terminate(Handle process, std::uint32_t exit_code);
[[nodiscard]] Status terminate_pid(std::uint32_t host_pid, std::uint32_t exit_code);
// Ends this process at once with `exit_code`: no exit handlers, no flushing.
[[noreturn]] void terminate_self(std::uint32_t exit_code) noexcept;
// Suspends every other thread of this process (exec in place: the old image
// never runs again).
void suspend_other_threads() noexcept;
// When a host process started (0: this process), in nanoseconds since the
// Unix epoch: a pid reused by a later process has a different one.
// Error::not_found / Error::access_denied as is_running.
[[nodiscard]] Result<std::uint64_t> start_time(std::uint32_t host_pid = 0);
// A handle to a running host process, for sync::wait_any (signalled when it
// ends) and sync::watch_once; holding it keeps its pid from being reused.
[[nodiscard]] Result<Handle> open(std::uint32_t host_pid);
// Whether a host process is running; Error::not_found when there is none,
// Error::access_denied when this process may not ask.
[[nodiscard]] Result<bool> is_running(std::uint32_t host_pid);
// The host path of a running process's executable.
[[nodiscard]] Result<std::string> image_path(std::uint32_t host_pid);
// A general child process (the Darwin process model's children): `program`
// with `arguments` (UTF-8; quoted for the host by the backend). The child
// inherits exactly the handles in `inherit` (made with inheritable(), under
// the same values) and the standard handles given, and the host
// environment. It shares this process's console; without one it gets no
// window. A command line too long for the host fails with
// Error::out_of_range.
struct Spawn {
    std::string program;
    std::vector<std::string> arguments;
    std::string working_directory;            // empty: this process's
    Handle standard_input, standard_output, standard_error;   // empty: none
    std::vector<Handle> inherit;
    bool new_group{};                         // a new console process group (no console signals from ours)
    bool suspended{};                         // the child's first thread waits for resume()
    bool end_with_parent{};                   // the child ends when this process does (a kill-on-close job)
};
struct Child {
    Handle process, thread;                   // close both with host::close
    std::uint32_t pid{};
};
[[nodiscard]] Result<Child> spawn(const Spawn& options);
[[nodiscard]] Status resume(Handle thread);
// An inheritable duplicate of `handle` for Spawn::inherit (close it after
// the spawn).
[[nodiscard]] Result<Handle> inheritable(Handle handle);

// Starts `program arguments...` (UTF-8; the backend quotes them for the
// host) with its standard input, output and error each a new pipe, without a
// console window. The child inherits only its three pipe ends, and the host
// environment and working directory. Returned are the child's process
// handle and pid and this process's ends of the pipes: write `input` (close
// it for end of file), read `output` and `error` until they read 0 bytes.
// Close all four with host::close.
struct Piped {
    Handle process;
    std::uint32_t pid{};
    Handle input, output, error;
};
[[nodiscard]] Result<Piped> spawn_piped(std::string_view program, std::span<const std::string> arguments);
// A duplicate of `handle` owned by process `target_pid` (0: this process),
// as its value there: a descriptor handed to another process.
[[nodiscard]] Result<std::uint64_t> duplicate_into(Handle handle, std::uint32_t target_pid);

} // namespace alchemy::host::process
