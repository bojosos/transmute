// Null host backend, area "sync": every call fails with Error::unsupported
// (include/alchemy/host/host.hpp).
#include "alchemy/host/sync.hpp"

namespace alchemy::host::sync {

Result<Handle> create_event(bool, bool) { return Error::unsupported; }
Status set_event(Handle) { return Error::unsupported; }
Status reset_event(Handle) { return Error::unsupported; }
Result<std::size_t> wait_any(std::span<const Handle>, std::optional<std::uint64_t>) { return Error::unsupported; }
Result<Handle> create_mutex(std::string_view) { return Error::unsupported; }
Status unlock_mutex(Handle) { return Error::unsupported; }
Result<Handle> create_semaphore(std::string_view, std::uint32_t, std::uint32_t, bool) { return Error::unsupported; }
Result<Handle> open_semaphore(std::string_view) { return Error::unsupported; }
Result<Handle> watch_once(Handle, void (*)(void*), void*) { return Error::unsupported; }
void unwatch(Handle, bool) noexcept {}
Status release_semaphore(Handle, std::uint32_t) { return Error::unsupported; }

} // namespace alchemy::host::sync
