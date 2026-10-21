#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace transmute { class GuestMemory; }

namespace alchemy {

class ProcessContext;

// Memory shared between Alchemy processes (stage 5 of
// docs/ALCHEMY_CHROME_AUDIT.md): the host side of Mach memory entries and
// out-of-line memory that cross a process boundary. A host section (a
// pagefile-backed Windows file mapping) is mapped as fx shared guest memory
// in every process that has it; stores through any mapping, in any process,
// are seen through all of them.
//
// Transport: a Token carries a section handle value that export_section has
// already duplicated into the receiving process, so the receiver owns it from
// the moment it is sent (the sender may exit at once). The receiver either
// imports it or, for a message destroyed undelivered, discards it.
//
// Code: cross-process memory is never executable (KERN_PROTECTION_FAILURE).
// fx keeps translations coherent with stores made in its own process (write
// faults on translated pages, aliases known to each other); it cannot see
// stores made by another process.
//
// Engines without section views (the reference engine) and non-Windows hosts
// support none of it: the functions return nullopt or KERN_NOT_SUPPORTED.
namespace xmem {

struct Section {
    std::shared_ptr<void> handle;   // the host section handle (its value), closed with the last copy
    std::uint64_t size{};           // bytes known to exist from the section's start
    bool writable{true};            // false: may only be mapped read-only
};

struct Token {
    std::uint64_t handle{};         // handle value in the receiving process
    std::uint64_t offset{}, size{}; // the byte range of the section it stands for
    bool writable{true};
};

// A new zero-filled section of `size` bytes (rounded to 16 KiB pages).
std::optional<Section> create_section(std::uint64_t size);

// The section behind fx shared guest memory at [address, address + size)
// (vm_allocate'd pages, memory-entry anchors, imported views) and the
// address's byte offset in it. Nullopt for private memory (heap, private
// mmap, images) or a range that is not one contiguous run of one section.
std::optional<std::pair<Section, std::uint64_t>> section_of(ProcessContext& context, std::uint64_t address,
                                                            std::uint64_t size);

// Sender side: [offset, offset + size) of `section` for the Windows process
// `host_pid` (writable only when `writable` and the section is). Nullopt when
// that process is gone or refuses the handle.
std::optional<Token> export_section(const Section& section, std::uint64_t offset, std::uint64_t size,
                                    std::uint32_t host_pid, bool writable = true);
// Out-of-line memory (copy semantics): a new section holding a copy of the
// guest bytes [address, address + size), exported to `host_pid`.
std::optional<Token> export_copy(transmute::GuestMemory& memory, std::uint64_t address, std::uint64_t size,
                                 std::uint32_t host_pid);

// Receiver side.
Section import_token(const Token& token);
void discard_token(const Token& token);

// Maps [offset, offset + size) of `section` at `address` (0: anywhere; else
// page aligned free space, or with `overwrite` Mach-owned pages it replaces,
// as VM_FLAGS_OVERWRITE) with Mach current `protection` and `maximum`
// protection (VM_PROT_READ/WRITE; write only for a writable section) as one fx
// shared region per 16 KiB page, registered with the Mach VM services, so
// vm_deallocate, vm_protect, vm_remap and vm_region work on it. Views of one
// section in this process are aliases of each other for fx: a store through
// one invalidates code translated from another. Returns the address, or
// nullopt with the Mach error in `kern_result`.
std::optional<std::uint64_t> map_section(ProcessContext& context, const Section& section, std::uint64_t offset,
                                         std::uint64_t size, std::uint64_t address, std::uint32_t protection,
                                         std::uint32_t& kern_result, std::uint32_t maximum = 3, bool overwrite = false);

// Out-of-line receive: the token's bytes copied into new vm_allocate memory
// of this process (released with vm_deallocate); the token is consumed.
std::optional<std::uint64_t> receive_copy(ProcessContext& context, const Token& token, std::uint32_t& kern_result);

} // namespace xmem
} // namespace alchemy
