#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <span>

namespace alchemy {

class ProcessContext;

// A Darwin errno from a DarwinVmSpace operation (EINVAL, ENOMEM, ...).
struct DarwinVmError { std::uint32_t value; };

// Anonymous private memory (mmap MAP_PRIVATE | MAP_ANON) with the Darwin
// kernel's range semantics, on engines with VM ranges (engine_has_vm_ranges):
//  * a non-null address without MAP_FIXED is a hint, tried first, then any
//    address (never an error by itself);
//  * MAP_FIXED replaces pages the guest owns here (or free address space)
//    with fresh zero-filled pages of the new protection; over memory Alchemy
//    gives the guest some other way (images, heap, file views) it fails with
//    ENOMEM;
//  * munmap, mprotect and madvise take any page range, not whole mappings;
//  * sizes are bounded only by the host's address space (ENOMEM when it
//    cannot reserve them).
// Ownership is tracked per Darwin page range, not per mmap call: the kernel's
// map entries split and merge the same way. Engines without VM ranges keep
// Alchemy's whole-mapping model in darwin_services.cpp (enabled() is false).
class DarwinVmSpace {
public:
    explicit DarwinVmSpace(ProcessContext& context);
    ~DarwinVmSpace();
    DarwinVmSpace(const DarwinVmSpace&) = delete;
    DarwinVmSpace& operator=(const DarwinVmSpace&) = delete;
    [[nodiscard]] bool enabled() const noexcept;
    // mmap(MAP_PRIVATE | MAP_ANON): `flags` are Darwin MAP_* bits.
    std::uint64_t map(std::uint64_t address, std::uint64_t length, std::uint64_t protection, std::uint64_t flags);
    // True when [address, address + length) touches memory this space owns.
    [[nodiscard]] bool intersects(std::uint64_t address, std::uint64_t length) const;
    std::uint64_t unmap(std::uint64_t address, std::uint64_t length);
    // mprotect of a range this space owns entirely; false when it does not.
    bool protect(std::uint64_t address, std::uint64_t length, std::uint64_t protection);
    // madvise of a range this space owns entirely; false when it does not.
    bool advise(std::uint64_t address, std::uint64_t length, std::uint64_t advice);
    // Mach vm_remap(VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, copy = FALSE) of
    // memory that can never change (image text) over pages this space owns:
    // fresh pages holding `bytes` with `protection` replace them (an exact
    // copy is what a shared alias of immutable memory looks like). The pages
    // stay this space's (munmap, mprotect). False when it does not own the
    // whole page-aligned range.
    bool replace_with_copy(std::uint64_t address, std::span<const std::uint8_t> bytes, std::uint64_t protection);
    // Mach vm_remap(VM_FLAGS_OVERWRITE, copy = FALSE) of shared memory over
    // pages this space owns: `alias` maps the shared memory there (the engine
    // takes the pages out of their reservation as munmap would) while no other
    // mmap can change the range (the space's lock is held). The pages stay
    // this space's (munmap, mprotect, MAP_FIXED). False, with `alias` not
    // called, when the space does not own the whole page-aligned range.
    bool replace_with_alias(std::uint64_t address, std::uint64_t length, const std::function<void()>& alias);

private:
    [[nodiscard]] bool owns(std::uint64_t begin, std::uint64_t end) const;   // mutex held
    void own(std::uint64_t begin, std::uint64_t end);                        // mutex held
    void disown(std::uint64_t begin, std::uint64_t end);                     // mutex held
    ProcessContext& context_;
    mutable std::mutex mutex_;
    std::map<std::uint64_t, std::uint64_t> owned_;   // begin -> end, disjoint
};

// The process's DarwinVmSpace (libSystem's services own it), or null.
DarwinVmSpace* darwin_vm_space(const ProcessContext& context);

} // namespace alchemy
