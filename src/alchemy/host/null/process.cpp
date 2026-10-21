// Null host backend, area "process": every call fails with Error::unsupported
// (include/alchemy/host/host.hpp).
#include "alchemy/host/process.hpp"

#include <cstdlib>
#include <functional>
#include <thread>

namespace alchemy::host::process {

std::uint32_t current_pid() noexcept { return 0; }
// Never 0, distinct per live thread (the standard library id).
std::uint64_t current_thread_id() noexcept { return std::hash<std::thread::id>{}(std::this_thread::get_id()) | 1; }
Handle standard_handle(Standard) noexcept { return {}; }
Result<Usage> usage(std::uint32_t) { return Error::unsupported; }
Result<ThreadUsage> thread_usage() { return Error::unsupported; }
Result<MemoryUsage> memory_usage() { return Error::unsupported; }
Result<std::vector<Entry>> list() { return Error::unsupported; }
std::optional<std::string> environment(std::string_view) { return std::nullopt; }
std::vector<std::string> environment_block() { return {}; }
Status set_environment(std::string_view, std::optional<std::string_view>) { return Error::unsupported; }
Result<std::uint32_t> exit_code(Handle) { return Error::unsupported; }
Status terminate(Handle, std::uint32_t) { return Error::unsupported; }
Status terminate_pid(std::uint32_t, std::uint32_t) { return Error::unsupported; }
void terminate_self(std::uint32_t exit_code) noexcept { std::_Exit(static_cast<int>(exit_code)); }
void suspend_other_threads() noexcept {}
Result<bool> is_running(std::uint32_t) { return Error::unsupported; }
Result<std::uint64_t> start_time(std::uint32_t) { return Error::unsupported; }
Result<Handle> open(std::uint32_t) { return Error::unsupported; }
Result<std::string> image_path(std::uint32_t) { return Error::unsupported; }
Result<std::uint64_t> duplicate_into(Handle, std::uint32_t) { return Error::unsupported; }
Result<Piped> spawn_piped(std::string_view, std::span<const std::string>) { return Error::unsupported; }
Result<Child> spawn(const Spawn&) { return Error::unsupported; }
Status resume(Handle) { return Error::unsupported; }
Result<Handle> inheritable(Handle) { return Error::unsupported; }

} // namespace alchemy::host::process
