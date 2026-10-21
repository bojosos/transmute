// Static TLS: per-module TLS indices, per-thread TLS arrays published at
// shadow TEB + 0x58 (ThreadLocalStoragePointer), TLS callbacks and
// DLL_THREAD_ATTACH / DLL_THREAD_DETACH delivery.

#include "fx/loader/loader_internal.hpp"

#include <malloc.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace transmute::fx {
namespace loader {
namespace {

// Bumped whenever a TLS slot is assigned; lets loader_refresh_thread_tls skip
// the lock when the calling thread is current.
std::atomic<std::uint64_t> g_tls_generation{1};
thread_local GuestContext* t_tls_context = nullptr;
// Set once DLL_THREAD_DETACH was delivered on this host thread. The thread is
// exiting: guest code may still run (guest FLS callbacks invoked by ntdll after
// the runtime tore the guest thread down get a fresh context), but it must not
// see a second DLL_THREAD_ATTACH / DLL_THREAD_DETACH.
thread_local bool t_thread_detached = false;
thread_local std::uint64_t t_tls_generation = 0;

void** allocate_array(std::size_t entries) {
    return static_cast<void**>(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, std::max<std::size_t>(entries, 1) * sizeof(void*)));
}

void free_array(void** array) {
    if (array) HeapFree(GetProcessHeap(), 0, array);
}

void* allocate_block(const Module& m) {
    const std::uint64_t template_size = m.tls.end - m.tls.start;
    const std::uint64_t size = template_size + m.tls.zero_fill;
    void* block = _aligned_malloc(static_cast<std::size_t>(std::max<std::uint64_t>(size, 16)),
                                  std::max<std::uint32_t>(m.tls.alignment, 16));
    if (!block) return nullptr;
    if (template_size) {
        std::memcpy(block, reinterpret_cast<const void*>(m.tls.start), static_cast<std::size_t>(template_size));
    }
    std::memset(static_cast<std::uint8_t*>(block) + template_size, 0,
                static_cast<std::size_t>(std::max<std::uint64_t>(size, 16) - template_size));
    return block;
}

void publish_array(ThreadRecord& t) {
    if (!t.ctx || !t.ctx->shadow_teb) return;
    auto* slot = reinterpret_cast<void* volatile*>(static_cast<std::uint8_t*>(t.ctx->shadow_teb) +
                                                    kTebTlsPointerOffset);
    InterlockedExchangePointer(const_cast<void**>(reinterpret_cast<void* volatile*>(slot)), t.array);
}

// Makes `t` cover every TLS slot: allocates missing blocks and grows the
// array. A grown array is published with one atomic pointer store; the old
// array stays valid (other code may still be reading it) until detach.
void build_thread_tls(ThreadRecord& t) {
    auto& s = state();
    const std::size_t n = s.tls_slots.size();
    if (t.blocks.size() < n) t.blocks.resize(n, nullptr);
    if (t.array_size < n || !t.array) {
        const std::size_t capacity = std::max<std::size_t>(16, (n + 15) / 16 * 16);
        void** grown = allocate_array(capacity);
        if (!grown) return;
        if (t.array) {
            std::memcpy(grown, t.array, t.array_size * sizeof(void*));
            t.retired.push_back(t.array);
        }
        for (std::size_t i = 0; i < n; ++i) {
            if (!t.blocks[i] && s.tls_slots[i]) t.blocks[i] = allocate_block(*s.tls_slots[i]);
            grown[i] = t.blocks[i];
        }
        t.array = grown;
        t.array_size = capacity;
        publish_array(t);
        return;
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (!t.blocks[i] && s.tls_slots[i]) {
            t.blocks[i] = allocate_block(*s.tls_slots[i]);
            InterlockedExchangePointer(&t.array[i], t.blocks[i]);
        }
    }
    // The array may have been replaced by guest code or never published.
    if (t.ctx && t.ctx->shadow_teb) {
        void* current = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(t.ctx->shadow_teb) +
                                                  kTebTlsPointerOffset);
        if (current != t.array) publish_array(t);
    }
}

void free_thread_tls(ThreadRecord& t) {
    for (void* block : t.blocks) {
        if (block) _aligned_free(block);
    }
    t.blocks.clear();
    if (t.ctx && t.ctx->shadow_teb) {
        auto** slot = reinterpret_cast<void**>(static_cast<std::uint8_t*>(t.ctx->shadow_teb) +
                                               kTebTlsPointerOffset);
        if (*slot == t.array) InterlockedExchangePointer(slot, nullptr);
    }
    free_array(t.array);
    t.array = nullptr;
    t.array_size = 0;
    for (void** old : t.retired) free_array(old);
    t.retired.clear();
}

bool runs_guest_code() {
    auto& s = state();
    return s.config.run_initializers && !s.exiting.load();
}

} // namespace

ThreadRecord* find_thread(GuestContext* ctx) {
    for (const auto& t : state().threads) {
        if (t->ctx == ctx) return t.get();
    }
    return nullptr;
}

void assign_tls_slot(Module& m) {
    auto& s = state();
    m.info.tls_index = kNoTls;
    m.has_tls = false;
    const PeDirectory dir = m.image.headers.dir(dir_tls);
    if (dir.rva == 0 || dir.size < sizeof(IMAGE_TLS_DIRECTORY64)) return;
    IMAGE_TLS_DIRECTORY64 tls;
    std::memcpy(&tls, m.image.base + dir.rva, sizeof(tls));
    const std::uint64_t lo = m.info.base, hi = m.info.base + m.info.size;
    auto inside = [&](std::uint64_t a, std::uint64_t size) { return a >= lo && a <= hi && size <= hi - a; };
    if (tls.EndAddressOfRawData < tls.StartAddressOfRawData ||
        (tls.StartAddressOfRawData != 0 &&
         !inside(tls.StartAddressOfRawData, tls.EndAddressOfRawData - tls.StartAddressOfRawData)) ||
        (tls.AddressOfIndex != 0 && !inside(tls.AddressOfIndex, 4))) {
        LoaderDiagnostic d;
        d.kind = LoaderDiagnostic::Kind::note;
        d.module = m.info.name;
        d.message = "invalid TLS directory ignored";
        add_diagnostic(std::move(d));
        return;
    }
    m.tls.start = tls.StartAddressOfRawData;
    m.tls.end = tls.EndAddressOfRawData;
    m.tls.zero_fill = tls.SizeOfZeroFill;
    m.tls.index_address = tls.AddressOfIndex;
    m.tls.callbacks = tls.AddressOfCallBacks;
    const std::uint32_t align_code = (tls.Characteristics >> 20) & 0xf;
    m.tls.alignment = align_code ? (1u << (align_code - 1)) : 16;
    std::uint32_t index = 0;
    while (index < s.tls_slots.size() && s.tls_slots[index]) ++index;
    if (index == s.tls_slots.size()) s.tls_slots.push_back(nullptr);
    s.tls_slots[index] = &m;
    m.info.tls_index = index;
    m.has_tls = true;
    g_tls_generation.fetch_add(1, std::memory_order_acq_rel);
    if (m.tls.index_address) {
        std::memcpy(reinterpret_cast<void*>(m.tls.index_address), &index, 4);
    }
}

void release_tls_slot(Module& m) {
    auto& s = state();
    if (!m.has_tls || m.info.tls_index >= s.tls_slots.size()) return;
    const std::uint32_t index = m.info.tls_index;
    std::lock_guard guard(s.threads_lock);
    for (const auto& t : s.threads) {
        if (index < t->blocks.size() && t->blocks[index]) {
            if (t->array && index < t->array_size) InterlockedExchangePointer(&t->array[index], nullptr);
            _aligned_free(t->blocks[index]);
            t->blocks[index] = nullptr;
        }
    }
    if (s.tls_slots[index] == &m) s.tls_slots[index] = nullptr;
    m.has_tls = false;
}

void refresh_all_threads_tls() {
    auto& s = state();
    std::lock_guard guard(s.threads_lock);
    for (const auto& t : s.threads) build_thread_tls(*t);
}

void run_tls_callbacks(const Module& m, std::uint32_t reason) {
    if (!m.has_tls || !m.tls.callbacks || !runs_guest_code()) return;
    for (unsigned i = 0; i < 1024; ++i) {
        std::uint64_t callback;
        std::memcpy(&callback, reinterpret_cast<const void*>(m.tls.callbacks + i * 8ull), 8);
        if (callback == 0) break;
        call_guest_init(callback, m.info.base, reason, 0);
    }
}

void reset_tls_for_testing() {
    auto& s = state();
    std::lock_guard guard(s.threads_lock);
    for (const auto& t : s.threads) {
        for (std::size_t i = 0; i < t->blocks.size(); ++i) {
            if (t->blocks[i]) _aligned_free(t->blocks[i]);
            if (t->array && i < t->array_size) t->array[i] = nullptr;
        }
        t->blocks.clear();
    }
    s.tls_slots.clear();
    g_tls_generation.fetch_add(1, std::memory_order_acq_rel);
}

} // namespace loader

using namespace loader;

namespace loader {
namespace {

// How long thread attach/detach running inside the host loader lock waits
// for the loader lock before giving up (see LoaderMutex).
constexpr unsigned kInsideHostLoaderLockWaitMs = 2000;

// Acquires the loader lock; bounded when the caller is inside the host loader
// lock (a holder may be waiting for it: e.g. guest_load_library calling
// LoadLibraryExW while this thread runs DLL_THREAD_DETACH in LdrShutdownThread).
bool acquire_loader_lock(const char* what) {
    auto& s = state();
    if (!host_loader_lock_held()) {
        s.lock.lock();
        return true;
    }
    if (s.lock.lock_bounded(kInsideHostLoaderLockWaitMs)) return true;
    std::fprintf(stderr,
                 "transmute-fx loader: %s skipped on thread %lu: the loader lock stayed busy while this "
                 "thread held the host loader lock\n",
                 what, GetCurrentThreadId());
    return false;
}

// Registers `ctx` (if needed) and makes its static TLS current. Caller holds
// the loader lock. Returns true if DLL_THREAD_ATTACH must be delivered now.
bool register_thread(GuestContext& ctx, bool lazy) {
    auto& s = state();
    std::lock_guard guard(s.threads_lock);
    ThreadRecord* t = find_thread(&ctx);
    if (!t) {
        s.threads.push_back(std::make_unique<ThreadRecord>());
        t = s.threads.back().get();
        t->ctx = &ctx;
        if (lazy) t->attached = true;   // a lazy check never delivers DLL_THREAD_ATTACH
    }
    build_thread_tls(*t);
    if (t->attached) return false;
    t->attached = true;
    if (t_thread_detached) {
        t->notify = false;   // re-entry during thread exit: static TLS only
        return false;
    }
    return true;
}

} // namespace
} // namespace loader

void loader_thread_attach(GuestContext& ctx) {
    auto& s = state();
    if (!acquire_loader_lock("DLL_THREAD_ATTACH")) return;
    std::unique_lock<LoaderMutex> lock(s.lock, std::adopt_lock);
    if (!register_thread(ctx, false) || !runs_guest_code()) return;
    const std::vector<Module*> order = s.init_order;
    for (Module* m : order) {
        if (!m->initialized) continue;
        run_tls_callbacks(*m, DLL_THREAD_ATTACH);
        if (m->info.is_dll && m->info.entry && m->thread_calls) {
            call_guest_init(m->info.entry, m->info.base, DLL_THREAD_ATTACH, 0);
        }
    }
}

void loader_thread_detach(GuestContext& ctx) {
    auto& s = state();
    const bool on_this_thread = !ctx.host_teb || ctx.host_teb == NtCurrentTeb();
    if (on_this_thread) t_thread_detached = true;
    if (t_tls_context == &ctx) t_tls_context = nullptr;
    bool notify = false;
    {
        std::lock_guard guard(s.threads_lock);
        const ThreadRecord* t = find_thread(&ctx);
        if (!t) return;
        notify = t->attached && t->notify;
    }
    // Once process exit started, no guest code runs here: ExitProcess delivers
    // DLL_PROCESS_DETACH after terminating the other threads.
    if (notify && runs_guest_code() && acquire_loader_lock("DLL_THREAD_DETACH")) {
        std::unique_lock<LoaderMutex> lock(s.lock, std::adopt_lock);
        const std::vector<Module*> order = s.init_order;
        for (auto it = order.rbegin(); it != order.rend(); ++it) {
            Module* m = *it;
            if (!m->initialized) continue;
            run_tls_callbacks(*m, DLL_THREAD_DETACH);
            if (m->info.is_dll && m->info.entry && m->thread_calls) {
                call_guest_init(m->info.entry, m->info.base, DLL_THREAD_DETACH, 0);
            }
        }
    }
    // The runtime frees the context and its shadow TEB after this returns:
    // the record must go even when the callbacks were skipped.
    std::lock_guard guard(s.threads_lock);
    for (auto it = s.threads.begin(); it != s.threads.end(); ++it) {
        if ((*it)->ctx == &ctx) {
            free_thread_tls(**it);
            s.threads.erase(it);
            break;
        }
    }
}

void loader_refresh_thread_tls(GuestContext& ctx) {
    // Lock-free fast path: this thread already saw the current slot layout.
    const std::uint64_t generation = g_tls_generation.load(std::memory_order_acquire);
    if (t_tls_context == &ctx && t_tls_generation == generation) return;
    auto& s = state();
    if (!acquire_loader_lock("TLS refresh")) return;
    std::unique_lock<LoaderMutex> lock(s.lock, std::adopt_lock);
    register_thread(ctx, true);
    t_tls_context = &ctx;
    t_tls_generation = g_tls_generation.load(std::memory_order_acquire);
}

} // namespace transmute::fx
