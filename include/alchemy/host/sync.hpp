#pragma once

// Host area "sync" (owner: r1). The rules are in include/alchemy/host/host.hpp. Waits, events and
// timers that the macOS layer uses directly (guest pthreads are fx threads, not this).

#include "alchemy/host/host.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace alchemy::host::sync {

// An event, manual- or auto-reset.
[[nodiscard]] Result<Handle> create_event(bool manual_reset, bool initially_set);
[[nodiscard]] Status set_event(Handle event);
[[nodiscard]] Status reset_event(Handle event);
// Waits until one of `handles` (at most max_wait_handles) is signalled or
// `timeout_ns` passes (empty: forever). Returns the index of the signalled
// handle, or Error::timed_out.
inline constexpr std::size_t max_wait_handles = 64;
[[nodiscard]] Result<std::size_t> wait_any(std::span<const Handle> handles, std::optional<std::uint64_t> timeout_ns);

// Calls `callback(context)` once, on a host thread-pool thread, when `object`
// is signalled (a process ending, an event). Returns the watch.
[[nodiscard]] Result<Handle> watch_once(Handle object, void (*callback)(void* context), void* context);
// Ends a watch (after it fired too). With `wait_for_callback`, waits for a
// callback already running to return; never pass it from the callback.
void unwatch(Handle watch, bool wait_for_callback) noexcept;

// Objects shared by name between the processes of this login session (the
// Windows "Local\" namespace; named POSIX objects on Linux). An empty name
// makes a private object. A wait_any on a mutex locks it; a mutex whose owner
// exited holding it is locked by the wait all the same.
[[nodiscard]] Result<Handle> create_mutex(std::string_view name);
[[nodiscard]] Status unlock_mutex(Handle mutex);
// A counting semaphore (opened when the name exists; its counts are then the
// creator's; with `exclusive`, an existing name fails with Error::exists).
// A wait_any takes one count.
[[nodiscard]] Result<Handle> create_semaphore(std::string_view name, std::uint32_t initial, std::uint32_t maximum, bool exclusive = false);
// A named semaphore some process created; Error::not_found when none has.
[[nodiscard]] Result<Handle> open_semaphore(std::string_view name);
[[nodiscard]] Status release_semaphore(Handle semaphore, std::uint32_t count);

} // namespace alchemy::host::sync
