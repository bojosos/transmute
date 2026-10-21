// Null host backend, area "sections": every call fails with Error::unsupported
// (include/alchemy/host/host.hpp).
#include "alchemy/host/sections.hpp"

namespace alchemy::host::sections {

Result<Handle> create_section(std::uint64_t, bool) { return Error::unsupported; }
Result<Handle> create_file_section(Handle, bool) { return Error::unsupported; }
Status flush(std::uintptr_t, std::size_t) { return Error::unsupported; }
bool same(Handle first, Handle second) noexcept { return first == second; }
Result<std::uint64_t> duplicate_into(Handle, std::uint32_t, bool) { return Error::unsupported; }
Result<std::uintptr_t> map(Handle, std::uint64_t, std::size_t, bool) { return Error::unsupported; }
Status unmap(std::uintptr_t) { return Error::unsupported; }
Result<Named> open_named(std::string_view, std::uint64_t) { return Error::unsupported; }
Result<Handle> open_existing(std::string_view, bool) { return Error::unsupported; }

} // namespace alchemy::host::sections
