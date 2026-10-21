#pragma once

#include <cstdint>

namespace alchemy {

class ProcessContext;

// Independently authored, self-task Mach VM services over checked guest memory.
void install_darwin_mach_vm(ProcessContext& context);

// Marks [base, base + size) as one purgeable object (true) for
// mach_vm_purgable_control, or forgets the objects in it (false): memory a
// provider maps from a purgeable memory entry (MAP_MEM_PURGABLE). False when
// the process has no Mach VM services installed.
bool register_darwin_purgeable(ProcessContext& context, std::uint64_t base, std::uint64_t size, bool purgeable);

// A host section (Windows file-mapping HANDLE) mapped as Mach-owned memory:
// [offset, offset + size) of it at `address` (0: anywhere; the chosen address
// is stored back) with Mach current `protection` and `maximum` protection
// (never execute; write only for a writable section), one fx shared region
// per page, so vm_deallocate, vm_protect and vm_remap apply. `overwrite`
// (fixed address only) replaces Mach-owned pages there. Returns a
// kern_return_t (KERN_NOT_SUPPORTED without section views or Mach VM services).
std::uint32_t map_darwin_section(ProcessContext& context, void* section, std::uint64_t offset, std::uint64_t size,
                                 bool writable_section, std::uint64_t& address, std::uint32_t protection,
                                 std::uint32_t maximum = 3, bool overwrite = false);
// vm_allocate(VM_FLAGS_ANYWHERE) for a provider; a kern_return_t.
std::uint32_t allocate_darwin_vm(ProcessContext& context, std::uint64_t size, std::uint64_t& address);

} // namespace alchemy
