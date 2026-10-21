// Glue to run guest code (language handlers, filters, table callbacks) and to
// hold the structures those handlers read in guest-accessible memory.

#include "fx/seh/dispatch.hpp"
#include "fx/host/vm.hpp"

#include <cstdio>
#include <cstdlib>

namespace transmute::fx::seh {
namespace {

// Per-thread bump allocator with stack discipline: every entry point releases
// what it allocated when it exits, so nested dispatches never free structures
// an outer dispatch is still using.
struct ScratchArena {
    std::uint8_t* base = nullptr;
    std::size_t capacity = 0;
    std::size_t used = 0;
    ~ScratchArena() {
        if (base) host::vm::release(base);
    }
};

thread_local ScratchArena g_scratch;

} // namespace

std::uint64_t invoke_guest_function(std::uint64_t function, std::span<const std::uint64_t> args) {
    return call_guest(function, args);
}

std::uint64_t invoke_guest_function(std::uint64_t function, std::initializer_list<std::uint64_t> args) {
    return call_guest(function, std::span<const std::uint64_t>(args.begin(), args.size()));
}

std::uint64_t allocate_guest_scratch(std::size_t size, std::size_t alignment) {
    ScratchArena& arena = g_scratch;
    if (!arena.base) {
        arena.capacity = 4u << 20;
        arena.base = static_cast<std::uint8_t*>(host::vm::allocate(nullptr, arena.capacity, host::vm::kReadWrite));
        arena.used = 0;
        if (!arena.base) {
            std::fprintf(stderr, "fx seh: cannot allocate exception scratch memory\n");
            std::abort();
        }
    }
    const std::size_t offset = (arena.used + (alignment - 1)) & ~(alignment - 1);
    if (offset + size > arena.capacity) {
        std::fprintf(stderr, "fx seh: exception scratch memory exhausted (nesting too deep)\n");
        std::abort();
    }
    arena.used = offset + size;
    return reinterpret_cast<std::uint64_t>(arena.base + offset);
}

std::size_t guest_scratch_mark() { return g_scratch.used; }

void guest_scratch_release(std::size_t mark) {
    if (mark <= g_scratch.used) g_scratch.used = mark;
}

void free_guest_scratch() { g_scratch.used = 0; }

} // namespace transmute::fx::seh
