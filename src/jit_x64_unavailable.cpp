// X64BlockCache on hosts without the reference JIT (jit_x64_windows.cpp is
// Windows x64 only). ReferenceEngine::set_jit_enabled refuses the JIT there,
// so no cache is ever created; these definitions only satisfy the linker.
#include "transmute/jit.hpp"

namespace transmute {

struct X64BlockCache::Impl {};

X64BlockCache::X64BlockCache() = default;
X64BlockCache::~X64BlockCache() = default;

std::size_t X64BlockCache::inherit_immutable(X64BlockCache&, std::uint64_t, std::span<PeImage* const>) { return 0; }
bool X64BlockCache::execute(CpuState&, std::uint64_t, std::span<const std::uint32_t>, std::uint64_t, GuestMemory*) {
    return false;
}
std::size_t X64BlockCache::execute_native_region(CpuState&, std::uint64_t, std::span<const std::uint32_t>,
                                                 std::uint64_t, std::size_t, GuestMemory*, bool*) {
    return 0;
}
std::size_t X64BlockCache::execute_cached(CpuState&, std::uint64_t, std::uint64_t, std::size_t, bool*, GuestMemory*,
                                          bool*) {
    return 0;
}
bool X64BlockCache::has_read_only_native_region(std::uint64_t, std::uint64_t) const noexcept { return false; }
bool X64BlockCache::has_read_only_cached_block(std::uint64_t, std::uint64_t) const noexcept { return false; }
bool X64BlockCache::has_parallel_cached_block(std::uint64_t, std::uint64_t) const noexcept { return false; }
void X64BlockCache::remember_fallback(std::uint64_t, std::uint64_t) {}
void X64BlockCache::record_fallback(std::uint64_t, std::uint32_t) {}
void X64BlockCache::invalidate_range(std::uint64_t, std::uint64_t) {}
void X64BlockCache::invalidate_all() {}
std::size_t X64BlockCache::cache_size() const { return 0; }
const void* X64BlockCache::last_code_address() const noexcept { return nullptr; }
void X64BlockCache::set_profiling(bool) {}
JitStats X64BlockCache::stats(std::size_t) const { return {}; }

} // namespace transmute
