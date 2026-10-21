#pragma once

// Host area "file": the translation cache's files (docs/FX_HOST_SEAMS.md,
// "files"): cache main files, journals, lock files and their directory.
//
// Paths are host wide strings (std::wstring: UTF-16 on win32; a Linux
// backend converts to UTF-8 at the call). Open flags keep the Win32
// CreateFileW semantics the cache's multi-process protocol relies on
// (cache/merge.cpp): sharing modes decide who may open a file at the same
// time (a Linux backend uses flock / O_EXCL), a lock file disappears when its
// last handle closes, and a mapped journal can still be deleted.

#include "fx/host/host.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace transmute::fx::host::file {

using File = void*;   // an open host file; nullptr = none

// Access and sharing (or'ed).
inline constexpr unsigned kRead = 1;
inline constexpr unsigned kWrite = 2;
inline constexpr unsigned kDelete = 4;     // access: may delete or rename it (mark_deleted)
inline constexpr unsigned kShareNone = 0;  // sharing: kRead, kWrite and kDelete as for access

enum class Create {
    open_existing,   // fail if it does not exist
    open_always,     // open, or create if it does not exist
    create_new,      // fail if it exists
    create_always,   // create or truncate
};

enum class Flags {
    normal,
    sequential,      // read or written front to back
    lock_file,       // temporary and hidden, deleted when its last handle closes
};

// nullptr on failure (thread::last_error() says why).
File open(const std::wstring& path, unsigned access, unsigned share, Create create, Flags flags) noexcept;
void close(File file) noexcept;

// The whole buffer, in chunks; false on an error or a zero-byte transfer.
bool write_all(File file, const void* data, std::size_t size) noexcept;
bool read_all(File file, void* data, std::size_t size) noexcept;
bool seek(File file, std::uint64_t offset) noexcept;
bool seek_end(File file) noexcept;
bool size(File file, std::uint64_t& bytes) noexcept;

// A read-only view of the whole file; nullptr on failure. `stage` (optional)
// tells which step failed: 1 creating the mapping, 2 mapping the view.
const std::uint8_t* map_read_only(File file, int* stage = nullptr) noexcept;
void unmap(const void* view) noexcept;

// Deletes the file's name now, even while other processes map it (POSIX
// semantics; falls back to delete-on-close). The file needs kDelete access.
void mark_deleted(File file) noexcept;
bool remove(const std::wstring& path) noexcept;
// Renames `from` to `to`, replacing `to`.
bool rename_replace(const std::wstring& from, const std::wstring& to) noexcept;
// Renames `temp` over `target` in the same directory so readers see the old
// file or the new one: POSIX rename semantics first (a reader holding the old
// file with delete sharing keeps it), else rename_replace retried briefly
// while a reader holds the target. False (temp left in place) if it cannot.
bool replace_atomically(const std::wstring& temp, const std::wstring& target) noexcept;

enum class Directory { created, exists, parent_missing, failed };
Directory create_directory(const std::wstring& path) noexcept;

struct Entry {
    std::wstring name;              // without the directory
    bool directory{};
    std::uint64_t size{};
    std::uint64_t write_time{};     // host::wall_time() units
};
// The entries matching `pattern` (a directory and a file name with * and ?);
// `large` hints a big directory.
std::vector<Entry> list(const std::wstring& pattern, bool large = false);
// Size and last write time of `path`; false if it does not exist.
bool attributes(const std::wstring& path, std::uint64_t* bytes, std::uint64_t* write_time) noexcept;
// `path` made absolute (GetFullPathNameW; realpath without resolving links
// on Linux); empty if it cannot be.
std::wstring full_path(const std::wstring& path);
// The host's directory for temporary files, as it reports it (may end in a separator).
std::wstring temp_directory();

} // namespace transmute::fx::host::file
