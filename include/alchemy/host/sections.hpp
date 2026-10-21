#pragma once

// Host area "sections" (owner: r1). The rules are in include/alchemy/host/host.hpp. Shareable
// memory: sections that several processes map (Darwin shared memory, cross-process Mach memory,
// SysV shm).

#include "alchemy/host/host.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace alchemy::host::sections {

// A new anonymous section of `size` bytes (CreateFileMapping, memfd_create);
// an `inheritable` one is passed to children that process::spawn names in
// its inherit list, under the same handle value.
[[nodiscard]] Result<Handle> create_section(std::uint64_t size, bool inheritable = false);
// A section over an open file (mmap(MAP_SHARED) of a file): its pages are the
// file's, for every process that maps the file.
[[nodiscard]] Result<Handle> create_file_section(Handle file, bool writable);
// Maps `size` bytes of `section` from `offset`, read-write or read-only.
[[nodiscard]] Result<std::uintptr_t> map(Handle section, std::uint64_t offset, std::size_t size, bool writable);
[[nodiscard]] Status unmap(std::uintptr_t address);
// Whether two handles name the same section object.
[[nodiscard]] bool same(Handle first, Handle second) noexcept;
// A duplicate of `section` owned by process `target_pid` (Mach memory sent to
// another process), as its handle value there; without `writable` the
// receiver cannot map it writable.
[[nodiscard]] Result<std::uint64_t> duplicate_into(Handle section, std::uint32_t target_pid, bool writable);
// msync: writes `size` bytes of a mapped file view from `address` to the file.
[[nodiscard]] Status flush(std::uintptr_t address, std::size_t size);

// A section shared by name between the processes of this login session
// (the Windows "Local\" namespace; a POSIX shared memory name on Linux):
// `size` zero-filled bytes, created by the first process that opens the name.
struct Named {
    Handle handle;
    bool created{}; // this call created it
};
[[nodiscard]] Result<Named> open_named(std::string_view name, std::uint64_t size);
// A named section another process created; Error::not_found when none has.
[[nodiscard]] Result<Handle> open_existing(std::string_view name, bool writable);

} // namespace alchemy::host::sections
