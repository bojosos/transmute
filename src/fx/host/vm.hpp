#pragma once

// Host area "vm": address space, protection, placeholders and sections
// (docs/FX_HOST_SEAMS.md, "vm" and "placeholders").
//
// Protections are fx's protection codes: the Win32 PAGE_* values, which
// fx's public API (transmute/fx/process.hpp) and the PE abi already use. The
// win32 backend passes them through; a Linux backend maps them to PROT_*
// (kGuard to a PROT_NONE page fx's fault handler re-arms, the copy-on-write
// codes to MAP_PRIVATE mappings).
//
// Every call is a system call, so none is inline; the backend's out-of-line
// call adds nothing measurable to it.

#include "fx/host/host.hpp"

#include <cstddef>
#include <cstdint>

namespace transmute::fx::host::vm {

using Protection = std::uint32_t;
inline constexpr Protection kNoAccess = 0x01;
inline constexpr Protection kReadOnly = 0x02;
inline constexpr Protection kReadWrite = 0x04;
inline constexpr Protection kWriteCopy = 0x08;
inline constexpr Protection kExecute = 0x10;
inline constexpr Protection kExecuteRead = 0x20;
inline constexpr Protection kExecuteReadWrite = 0x40;
inline constexpr Protection kExecuteWriteCopy = 0x80;
// Modifiers (or'ed onto one of the above).
inline constexpr Protection kGuard = 0x100;
inline constexpr Protection kNoCache = 0x200;
inline constexpr Protection kWriteCombine = 0x400;

enum class State : std::uint8_t { free, reserved, committed };
enum class Type : std::uint8_t { none, private_memory, mapped, image };

// One run of pages with equal attributes (VirtualQuery's
// MEMORY_BASIC_INFORMATION; /proc/self/maps on Linux).
struct RegionInfo {
    std::uint64_t base{};              // the page containing the queried address
    std::uint64_t allocation_base{};   // the reservation (mapping) it belongs to
    std::uint64_t size{};              // bytes from `base` with these attributes
    Protection protect{};              // 0 for free and reserved pages
    Protection allocation_protect{};
    State state{State::free};
    Type type{Type::none};
    [[nodiscard]] bool committed() const noexcept { return state == State::committed; }
};

// False if `address` is outside the user address space.
bool query(const void* address, RegionInfo& info) noexcept;

// Reserves (and commits, for allocate) `size` bytes. `address` nullptr: the
// host chooses; otherwise exactly there or nullptr. top_down: the highest
// free range (keeps low addresses for the guest).
void* allocate(void* address, std::size_t size, Protection protect) noexcept;
void* reserve(void* address, std::size_t size, Protection protect, bool top_down = false) noexcept;
// Reserves `size` inaccessible bytes at or above `lowest`, wherever the host
// finds room (allocation-granularity aligned); nullptr if there is none.
void* reserve_above(std::uint64_t lowest, std::size_t size) noexcept;
// Commits reserved pages (committed ones keep their contents); the address
// or nullptr.
void* commit(void* address, std::size_t size, Protection protect) noexcept;
bool decommit(void* address, std::size_t size) noexcept;
// Releases a whole reservation made by allocate/reserve/reserve_above.
bool release(void* address) noexcept;
// On a copy-on-write view (a view mapped kWriteCopy) writable protections
// become their write-copy forms: such a view refuses kReadWrite.
bool protect(void* address, std::size_t size, Protection protection, Protection* old = nullptr) noexcept;
// The host may discard the pages' contents (they read back as anything,
// zero on Linux) without decommitting them (MEM_RESET; MADV_FREE).
void discard(void* address, std::size_t size) noexcept;
// After writing code at `address`.
void flush_instruction_cache(const void* address, std::size_t size) noexcept;
// Starts reading the pages of [address, address + size) (a mapped file view)
// into memory in large requests, so later touches find them resident instead
// of reading them one page at a time (PrefetchVirtualMemory; madvise
// MADV_WILLNEED). A hint: failure is ignored.
void prefetch(const void* address, std::size_t size) noexcept;

// ---- placeholders and sections ---------------------------------------------------
// A placeholder is reserved address space that a section view can replace
// and that a view turns back into when it is unmapped, so no other
// allocation can take the address meanwhile (VirtualAlloc2 /
// MapViewOfFile3 / UnmapViewOfFile2; mmap(MAP_FIXED_NOREPLACE) over a
// PROT_NONE reservation and memfd views on Linux).
using Section = void*;   // a host section (file-mapping HANDLE; memfd)

// `address` nullptr: the host chooses.
void* reserve_placeholder(void* address, std::size_t size) noexcept;
// Splits the placeholder starting at `address` after `size` bytes.
bool split_placeholder(void* address, std::size_t size) noexcept;
// Joins adjacent placeholders covering exactly [address, address + size).
bool coalesce_placeholders(void* address, std::size_t size) noexcept;
// A pagefile-backed read-write section of `size` bytes (committed, zero).
Section create_section(std::uint64_t size) noexcept;
// Another handle to `section` (a duplicate keeps it alive); nullptr on failure.
Section duplicate_section(Section section) noexcept;
void close_section(Section section) noexcept;
// Replaces the placeholder [address, address + size) by a view of `section`
// at `offset` with `protect` (kReadWrite, kReadOnly, or kWriteCopy for a
// copy-on-write view of a read-only file section); the address or nullptr.
void* map_view_into_placeholder(Section section, void* address, std::uint64_t offset, std::size_t size,
                                Protection protect) noexcept;
// Unmaps the view at `address`, leaving its placeholder.
bool unmap_view_to_placeholder(void* address) noexcept;

// ---- private heap ------------------------------------------------------------------
// A growable host heap separate from the C runtime's, with a
// low-fragmentation front end (HeapCreate + LFH); nullptr on failure.
using Heap = void*;
Heap create_heap() noexcept;
// Zero-filled, 16-byte aligned; nullptr when out of memory.
void* heap_allocate(Heap heap, std::size_t size) noexcept;
// False if `block` is not a block of `heap`.
bool heap_free(Heap heap, void* block) noexcept;

} // namespace transmute::fx::host::vm
