// Code arena: one RWX reservation holding every stub, translated region and
// thunk, so all of them are within rel32 reach of each other. It is cut into
// kArenaChunk chunks (see runtime_internal.hpp, "code arena"):
//   * permanent chunks: the stubs (arena offset 0), abi thunks, trampolines,
//     host callbacks and adapters (arena_allocate). Bump allocated, never freed.
//   * code chunks: translated regions (code_allocate), in two streams. Code
//     grows up from the chunk's bottom, tier-0 entry counter pages down from
//     its top, and the regions' metadata lives in read-write blocks the chunk
//     owns. A code chunk is freed by reclaim (regions.cpp) once all its
//     regions are dead and no thread can reach them any more.
//   * free chunks: decommitted, reused by either kind.

#include "fx/runtime/runtime_internal.hpp"
#include "fx/host/env.hpp"
#include "fx/host/vm.hpp"

#include <algorithm>
#include <cstdlib>
#include <new>
#include <stdexcept>

namespace transmute::fx {

void* allocate_code(std::size_t size, std::size_t alignment) {
    rt::ensure_initialized();
    return rt::arena_allocate(size, alignment);
}

const void* runtime_arena_base() noexcept { return rt::arena_base(); }

} // namespace transmute::fx

namespace transmute::fx::rt {
namespace {

constexpr std::size_t kPage = 4096;
constexpr std::size_t kMetaBlock = 64 * 1024;   // the host's allocation granularity
constexpr std::uint32_t kNone = ~std::uint32_t{0};

struct ArenaState {
    std::uint8_t* base{};
    host::RwLock lock;
    ArenaChunk chunks[kArenaChunks];
    std::uint32_t permanent = kNone;              // chunk permanent allocations bump in
    std::uint32_t current[kCodeStreams] = {kNone, kNone};
    std::uint64_t sequence = 0;
    std::size_t free_chunks = 0;
    std::size_t budget = kArenaChunks;            // chunks that may be in use (TRANSMUTE_FX_ARENA_MB)
};
ArenaState g_arena;

// Unwind description of the arena (see arena_install_unwind).
std::vector<host::UnwindEntry> g_precise;
host::UnwindEntry* g_unwind_chunks{};

// Results for abi frames vary per thunk: a small per-thread ring keeps each
// returned entry valid while the unwinder uses it.
thread_local host::UnwindEntry t_abi_entries[16];
thread_local unsigned t_abi_next = 0;

// Unwind description of an arena address:
//  * runtime stubs with frames (enter, host-call bridge): precise entries;
//  * abi thunks and callback trampolines (abi_code_frame): jmp-entered thunks
//    run on the enter-stub frame, plus `frame_bytes` in their body; called
//    trampolines are Win64 functions ([rsp] = return address outside the body);
//  * everything else (translated code, other stubs, rel32 trampolines): the
//    enter-stub frame, i.e. rsp == ctx->host_rsp.
host::UnwindEntry* arena_unwind_callback(std::uint64_t control_pc) {
    const auto base = reinterpret_cast<std::uint64_t>(g_arena.base);
    const auto offset = control_pc - base;
    if (offset >= kArenaReserve) return nullptr;
    for (auto& function : g_precise) {
        if (offset >= function.begin && offset < function.end) return &function;
    }
    AbiCodeFrame frame;
    if (abi_code_frame(control_pc, frame)) {
        // The frame is allocated from the end of the prolog through the first
        // epilog instruction (inclusive: a call's return address is often the
        // epilog start, which has not executed yet).
        const bool body = control_pc >= frame.begin + frame.prolog_bytes && control_pc <= frame.begin + frame.epilog_offset;
        const auto rva = body && frame.frame_bytes ? abi_frame_unwind_rva(frame.frame_bytes, frame.entered_by_call) : 0;
        if (rva == 0) return frame.entered_by_call ? nullptr : &g_unwind_chunks[offset / kUnwindChunk];
        auto& entry = t_abi_entries[t_abi_next++ % 16];
        entry.begin = static_cast<std::uint32_t>(frame.begin + frame.prolog_bytes - base);
        entry.end = static_cast<std::uint32_t>(frame.begin + frame.epilog_offset + 1 - base);
        entry.unwind = rva;
        return &entry;
    }
    return &g_unwind_chunks[offset / kUnwindChunk];
}

std::uint8_t* chunk_memory(std::uint32_t index) noexcept { return g_arena.base + std::size_t{index} * kArenaChunk; }

std::size_t round_up(std::size_t value, std::size_t granularity) noexcept {
    return (value + granularity - 1) & ~(granularity - 1);
}

// Arena lock held. Commits the head chunk's bytes [0, end) (bottom-up, in
// kArenaCommitChunk steps, never into its counter pages).
bool commit_bottom(ArenaChunk& c, std::uint32_t index, std::size_t end) {
    if (end <= c.committed) return true;
    const std::size_t limit = std::size_t{c.span} * kArenaChunk;
    const std::size_t target = std::min(std::min(round_up(end, kArenaCommitChunk), limit), c.floor);
    if (!host::vm::commit(chunk_memory(index) + c.committed, target - c.committed, host::vm::kExecuteReadWrite))
        return false;
    c.committed = target;
    return true;
}

// Arena lock held. The first run of `span` free chunks, or kNone (also
// when taking them would exceed the budget).
std::uint32_t find_free_run(std::uint32_t span) {
    if (kArenaChunks - g_arena.free_chunks + span > g_arena.budget) return kNone;
    for (std::uint32_t i = 0; i + span <= kArenaChunks; ++i) {
        std::uint32_t n = 0;
        while (n < span && g_arena.chunks[i + n].state == ChunkState::free) ++n;
        if (n == span) return i;
        i += n;   // skip past the chunk that is not free
    }
    return kNone;
}

// Arena lock held. Takes `span` free chunks starting at `index` for `state`.
void take_run(std::uint32_t index, std::uint32_t span, ChunkState state, CodeStream stream) {
    ArenaChunk& head = g_arena.chunks[index];
    head.state = state;
    head.stream = stream;
    head.span = span;
    head.head = index;
    head.sequence = ++g_arena.sequence;
    head.top = 0;
    head.floor = std::size_t{span} * kArenaChunk;
    head.committed = 0;
    head.counter_pages = 0;
    head.counter_next = head.counter_end = nullptr;
    head.meta_next = head.meta_end = nullptr;
    for (std::uint32_t i = 1; i < span; ++i) {
        ArenaChunk& tail = g_arena.chunks[index + i];
        tail.head = index;   // before the state: region_at reads both without the arena lock
        tail.state = ChunkState::tail;
    }
    g_arena.free_chunks -= span;
}

// Arena lock held. Room for `bytes` of code (aligned) and, with `counter`, an
// entry counter in head chunk `c`.
bool fits(const ArenaChunk& c, std::size_t offset, std::size_t bytes, bool counter) {
    std::size_t floor = c.floor;
    if (counter && c.counter_next == c.counter_end) {
        if (floor < kPage) return false;
        floor -= kPage;   // a new counter page below the current ones
    }
    return offset <= floor && bytes <= floor - offset;
}

// Arena lock held. The next entry counter of code chunk `c` (on counter pages
// of their own at its top: translated code decrements them on every tier-0
// region entry, and stores near code would invalidate it in the instruction
// cache). fits() was checked.
std::uint32_t* take_counter(ArenaChunk& c, std::uint32_t index) {
    if (c.counter_next == c.counter_end) {
        c.floor -= kPage;
        auto* page = chunk_memory(index) + c.floor;
        if (!host::vm::commit(page, kPage, host::vm::kExecuteReadWrite)) throw std::bad_alloc();
        ++c.counter_pages;
        auto* words = reinterpret_cast<std::uint32_t*>(page);
        c.counter_next = words + 16;          // keep the page's first and last cache lines empty
        c.counter_end = words + kPage / 4 - 16;
    }
    return c.counter_next++;
}

// Arena lock held. `bytes` of region metadata from the blocks chunk `c` owns.
void* take_metadata(ArenaChunk& c, std::size_t bytes) {
    bytes = round_up(bytes, 16);
    if (static_cast<std::size_t>(c.meta_end - c.meta_next) < bytes) {
        const std::size_t size = round_up(std::max(bytes, kMetaBlock), kMetaBlock);
        auto* block = static_cast<std::uint8_t*>(host::vm::allocate(nullptr, size, host::vm::kReadWrite));
        if (!block) throw std::bad_alloc();
        c.meta_blocks.push_back(block);
        c.meta_bytes += size;
        c.meta_next = block;
        c.meta_end = block + size;
    }
    void* p = c.meta_next;
    c.meta_next += bytes;
    return p;
}

// Arena lock held. Retires stream `stream`'s current chunk (it is full).
void retire_current(CodeStream stream) {
    auto& current = g_arena.current[static_cast<unsigned>(stream)];
    if (current != kNone) g_arena.chunks[current].current = false;
    current = kNone;
}

} // namespace

void arena_initialize() {
    auto* base = static_cast<std::uint8_t*>(host::vm::reserve(nullptr, kArenaReserve, host::vm::kExecuteReadWrite,
                                                              /*top_down=*/true));
    if (!base) fatal("fx runtime: cannot reserve the code arena");
    g_arena.base = base;
    g_arena.free_chunks = kArenaChunks;
    // TRANSMUTE_FX_ARENA_MB=<MiB>: at most that much of the arena in use
    // (16 MiB up to the whole reservation); reclaim keeps code within it.
    char value[16] = {};
    if (host::get_env("TRANSMUTE_FX_ARENA_MB", value, sizeof(value)) > 0) {
        const auto mb = std::clamp<unsigned long long>(std::strtoull(value, nullptr, 10), 16, kArenaReserve >> 20);
        g_arena.budget = static_cast<std::size_t>((mb << 20) / kArenaChunk);
    }
    // Chunk 0 is permanent: the stubs are written at offset 0 by generate_stubs.
    take_run(0, 1, ChunkState::permanent, CodeStream::stable);
    g_arena.permanent = 0;
    if (!commit_bottom(g_arena.chunks[0], 0, kStubAreaSize)) fatal("fx runtime: cannot commit the code arena");
    g_arena.chunks[0].top = kStubAreaSize;
}

std::uint8_t* arena_base() noexcept { return g_arena.base; }

ArenaChunk& arena_chunk(std::uint32_t index) noexcept { return g_arena.chunks[index]; }

std::uint32_t arena_chunk_of(std::uint64_t address) noexcept {
    const auto offset = address - reinterpret_cast<std::uint64_t>(g_arena.base);
    if (offset >= kArenaReserve) return kNone;
    const auto index = static_cast<std::uint32_t>(offset / kArenaChunk);
    const ArenaChunk& c = g_arena.chunks[index];
    return c.state == ChunkState::tail ? c.head : index;
}

ArenaUsage arena_usage() noexcept {
    SharedLock lock(g_arena.lock);
    ArenaUsage u;
    for (std::uint32_t i = 0; i < kArenaChunks; ++i) {
        const ArenaChunk& c = g_arena.chunks[i];
        const std::size_t counters = std::size_t{c.counter_pages} * kPage;
        switch (c.state) {
        case ChunkState::free: ++u.free_chunks; continue;
        case ChunkState::tail: continue;
        case ChunkState::permanent: u.permanent_bytes += c.top; break;
        case ChunkState::code: u.code_bytes += c.top + counters; break;
        }
        u.used_chunks += c.span;
        u.committed_bytes += c.committed + counters;
        u.metadata_bytes += c.meta_bytes;
    }
    return u;
}

std::size_t arena_used() noexcept {
    const ArenaUsage u = arena_usage();
    return u.permanent_bytes + u.code_bytes;
}

bool arena_lock_free() noexcept {
    if (!g_arena.lock.try_lock()) return false;
    g_arena.lock.unlock();
    return true;
}

void* arena_allocate(std::size_t size, std::size_t alignment) {
    if (alignment < 16) alignment = 16;
    if ((alignment & (alignment - 1)) != 0) throw std::invalid_argument("allocate_code: alignment must be a power of two");
    if (size > kArenaReserve - kArenaChunk) throw std::bad_alloc();
    ExclusiveLock lock(g_arena.lock);
    std::uint32_t index = g_arena.permanent;
    std::size_t offset = round_up(g_arena.chunks[index].top, alignment);
    if (offset > g_arena.chunks[index].floor || size > g_arena.chunks[index].floor - offset) {
        // Never reclaims: callers may hold the region map lock. A permanent
        // chunk that is not full stays so (its rest is lost; allocations are small).
        const auto span = static_cast<std::uint32_t>(round_up(std::max<std::size_t>(size, 1), kArenaChunk) / kArenaChunk);
        index = find_free_run(span);
        if (index == kNone) throw std::bad_alloc();
        take_run(index, span, ChunkState::permanent, CodeStream::stable);
        g_arena.permanent = index;
        offset = 0;
    }
    ArenaChunk& c = g_arena.chunks[index];
    if (!commit_bottom(c, index, offset + size)) throw std::bad_alloc();
    c.top = offset + size;
    return chunk_memory(index) + offset;
}

CodeBlock code_allocate(std::size_t code_bytes, std::size_t meta_bytes, bool counter, CodeStream stream) {
    code_bytes = round_up(std::max<std::size_t>(code_bytes, 1), 16);
    if (code_bytes > kArenaReserve / 4) throw std::bad_alloc();
    const auto s = static_cast<unsigned>(stream);
    for (unsigned attempt = 0;; ++attempt) {
        CodeBlock block;
        bool took = false;
        std::size_t free_chunks = 0;
        {
            ExclusiveLock lock(g_arena.lock);
            std::uint32_t index = g_arena.current[s];
            if (index == kNone || !fits(g_arena.chunks[index], g_arena.chunks[index].top, code_bytes, counter)) {
                retire_current(stream);
                const std::size_t need = code_bytes + (counter ? kPage : 0);
                const auto span = static_cast<std::uint32_t>(round_up(need, kArenaChunk) / kArenaChunk);
                index = find_free_run(span);
                if (index != kNone) {
                    take_run(index, span, ChunkState::code, stream);
                    g_arena.chunks[index].current = true;
                    g_arena.current[s] = index;
                    took = true;
                }
            }
            if (index != kNone) {
                ArenaChunk& c = g_arena.chunks[index];
                block.chunk = index;
                block.reused = c.reused;
                if (counter) block.counter = take_counter(c, index);
                if (!commit_bottom(c, index, c.top + code_bytes)) throw std::bad_alloc();
                block.code = chunk_memory(index) + c.top;
                c.top += code_bytes;
                block.meta = take_metadata(c, meta_bytes);
                c.pending.fetch_add(1, std::memory_order_relaxed);
                const std::size_t used = kArenaChunks - g_arena.free_chunks;
                free_chunks = used < g_arena.budget ? g_arena.budget - used : 0;
            }
        }
        if (block.code) {
            // A full chunk just went out of use: a reclaim pass may find dead chunks now.
            if (took) reclaim_notify(free_chunks);
            return block;
        }
        // No free chunk: wait for a reclaim pass (with eviction), then retry.
        if (attempt >= 3 || !reclaim_wait()) throw std::bad_alloc();
    }
}

void code_release(const CodeBlock& block) noexcept {
    g_arena.chunks[block.chunk].pending.fetch_sub(1, std::memory_order_release);
}

void arena_code_chunks(std::vector<ChunkSummary>& out) {
    SharedLock lock(g_arena.lock);
    out.clear();
    for (std::uint32_t i = 0; i < kArenaChunks; ++i) {
        const ArenaChunk& c = g_arena.chunks[i];
        if (c.state != ChunkState::code) continue;
        out.push_back({i, c.span, c.sequence, c.current || c.pending.load(std::memory_order_acquire) != 0, c.stream,
                       c.top + std::size_t{c.counter_pages} * kPage});
    }
}

std::size_t arena_free_chunks() noexcept {
    SharedLock lock(g_arena.lock);
    const std::size_t used = kArenaChunks - g_arena.free_chunks;
    return used < g_arena.budget ? g_arena.budget - used : 0;
}

std::size_t arena_budget() noexcept {
    SharedLock lock(g_arena.lock);
    return g_arena.budget;
}

void arena_set_budget_for_tests(std::size_t chunks) noexcept {
    ExclusiveLock lock(g_arena.lock);
    g_arena.budget = std::clamp<std::size_t>(chunks, 1, kArenaChunks);
}

void arena_free_chunk(std::uint32_t index) {
    ExclusiveLock lock(g_arena.lock);
    ArenaChunk& c = g_arena.chunks[index];
    if (c.state != ChunkState::code || c.current) return;
    const std::uint32_t span = c.span;
    host::vm::decommit(chunk_memory(index), std::size_t{span} * kArenaChunk);
    for (void* block : c.meta_blocks) host::vm::release(block);
    c.meta_blocks.clear();
    c.meta_blocks.shrink_to_fit();
    c.meta_bytes = 0;
    for (std::uint32_t i = 0; i < span; ++i) {
        ArenaChunk& part = g_arena.chunks[index + i];
        part.state = ChunkState::free;
        part.reused = true;
        part.head = index + i;
        part.span = 1;
    }
    g_arena.free_chunks += span;
}

void arena_install_unwind(const std::vector<host::UnwindEntry>& precise, std::uint32_t generic_unwind_rva) {
    g_precise = precise;
    const std::size_t chunks = kArenaReserve / kUnwindChunk;
    g_unwind_chunks = static_cast<host::UnwindEntry*>(host::vm::allocate(nullptr, chunks * sizeof(host::UnwindEntry),
                                                                         host::vm::kReadWrite));
    if (!g_unwind_chunks) fatal("fx runtime: cannot allocate the unwind table");
    for (std::size_t i = 0; i < chunks; ++i) {
        g_unwind_chunks[i].begin = static_cast<std::uint32_t>(i * kUnwindChunk);
        g_unwind_chunks[i].end = static_cast<std::uint32_t>((i + 1) * kUnwindChunk);
        g_unwind_chunks[i].unwind = generic_unwind_rva;
    }
    if (!host::register_unwind_lookup(reinterpret_cast<std::uint64_t>(g_arena.base),
                                      static_cast<std::uint32_t>(kArenaReserve), &arena_unwind_callback)) {
        fatal("fx runtime: RtlInstallFunctionTableCallback failed");
    }
}

} // namespace transmute::fx::rt
