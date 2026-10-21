#pragma once

// Shared guest memory internals used by regions.cpp (owner: runtime;
// shared_memory.cpp). A guest page that is also mapped at other guest
// addresses (alias_guest_memory) must be write-protected at every address
// while it holds translations, and a write through any of them invalidates
// the translations of all of them.

#include <atomic>
#include <cstdint>
#include <vector>

namespace transmute::fx::rt {

// True once any shared memory is mapped at more than one address.
extern std::atomic<bool> g_shared_aliases;

// The other guest addresses (4 KiB pages) that map the same page as `page`.
// Appends to `out`; nothing for unshared pages. Callable under the region
// map lock (the shared-memory lock nests inside it).
void shared_memory_aliases(std::uint64_t page, std::vector<std::uint64_t>& out);

} // namespace transmute::fx::rt
