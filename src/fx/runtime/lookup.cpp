// Process-wide guest pc -> host entry lookup table read by stub_indirect.
//
// Direct-mapped, 2^kLookupIndexBits 8-byte slots. A slot packs the pc tag
// (pc >> kLookupTagShift, compared as a full 64-bit value so non-canonical
// pcs never alias) in the high half and the entry's offset from the arena base
// in the low half. Slots are written with single aligned 8-byte stores, so a
// reader sees either the old or the new entry, never a torn mix. Writers hold
// the region map lock (shared for re-installs after a miss, exclusive for
// removal), so a removal can never race with a re-install of the same entry.

#include "fx/runtime/runtime_internal.hpp"
#include "fx/host/vm.hpp"

#include <atomic>

namespace transmute::fx::rt {
namespace {

std::uint64_t* g_table{};

bool encode(std::uint64_t pc, const void* host, std::uint64_t& value) noexcept {
    const auto tag = pc >> kLookupTagShift;
    const auto offset = reinterpret_cast<std::uint64_t>(host) - reinterpret_cast<std::uint64_t>(arena_base());
    if ((pc & 3) != 0 || tag > 0xffffffffULL || offset == 0 || offset >= kArenaReserve) return false;
    value = (tag << 32) | offset;
    return true;
}

std::atomic_ref<std::uint64_t> slot(std::uint64_t pc) noexcept {
    return std::atomic_ref<std::uint64_t>(g_table[lookup_index(pc)]);
}

} // namespace

void lookup_initialize() {
    g_table = static_cast<std::uint64_t*>(host::vm::allocate(nullptr, (kLookupMask + 1) * sizeof(std::uint64_t),
                                                             host::vm::kReadWrite));
    if (!g_table) fatal("fx runtime: cannot allocate the lookup table");
}

std::uint64_t* lookup_table() noexcept { return g_table; }

void lookup_install(std::uint64_t pc, const void* host) noexcept {
    std::uint64_t value;
    if (encode(pc, host, value)) slot(pc).store(value, std::memory_order_release);
}

void lookup_remove(std::uint64_t pc, const void* host) noexcept {
    std::uint64_t value;
    if (!encode(pc, host, value)) return;
    auto s = slot(pc);
    if (s.load(std::memory_order_relaxed) == value) s.store(0, std::memory_order_release);
}

const void* lookup_probe(std::uint64_t pc) noexcept {
    if ((pc & 3) != 0) return nullptr;
    const auto value = slot(pc).load(std::memory_order_acquire);
    const auto offset = static_cast<std::uint32_t>(value);
    if (offset == 0 || (value >> 32) != (pc >> kLookupTagShift)) return nullptr;
    return arena_base() + offset;
}

} // namespace transmute::fx::rt
