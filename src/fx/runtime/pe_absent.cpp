// Hosts without the PE guest personality (src/fx/loader and src/fx/abi are
// Windows-only; docs/FX_HOST_SEAMS.md): what the runtime and Alchemy need
// from them.
//
// - The external module registry (register_external_module and its
//   lookups): the Mach-O images Alchemy maps. The same contract as the PE
//   loader's (loader.cpp), without PE modules.
// - The loader's thread and process hooks: nothing to do without PE images
//   (no static TLS, DllMain or activation contexts).
// - The abi's host-call binding: there are no host Win32 DLLs to bind to.
#include "fx/cache/cache.hpp"
#include "transmute/fx/process.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace transmute::fx {

namespace {

struct Table {
    struct Entry {
        std::uint64_t base, end;
        const GuestModule* module;
    };
    std::vector<Entry> entries;   // sorted by base
};

struct Registry {
    std::mutex lock;
    std::vector<std::unique_ptr<GuestModule>> modules;
    std::vector<std::unique_ptr<GuestModule>> retired;   // lookups may still hold them
    std::vector<std::unique_ptr<Table>> retired_tables;
};

Registry& registry() {
    static auto* instance = new Registry;   // never destroyed: lookups run until _exit
    return *instance;
}

std::atomic<const Table*> g_table{nullptr};

// Publishes a new lookup table (registry lock held).
void publish() {
    auto& r = registry();
    auto table = std::make_unique<Table>();
    for (const auto& m : r.modules) table->entries.push_back({m->base, m->base + m->size, m.get()});
    std::sort(table->entries.begin(), table->entries.end(),
              [](const Table::Entry& a, const Table::Entry& b) { return a.base < b.base; });
    g_table.store(table.get(), std::memory_order_release);
    r.retired_tables.push_back(std::move(table));
}

} // namespace

const GuestModule* find_guest_module(std::uint64_t address) noexcept {
    const Table* table = g_table.load(std::memory_order_acquire);
    if (!table) return nullptr;
    const auto& e = table->entries;
    std::size_t lo = 0, hi = e.size();
    while (lo < hi) {   // first entry with base > address
        const std::size_t mid = (lo + hi) / 2;
        if (e[mid].base <= address) lo = mid + 1;
        else hi = mid;
    }
    if (lo == 0) return nullptr;
    const auto& candidate = e[lo - 1];
    return address < candidate.end ? candidate.module : nullptr;
}

std::vector<const GuestModule*> guest_modules() {
    auto& r = registry();
    std::lock_guard guard(r.lock);
    std::vector<const GuestModule*> out;
    for (const auto& m : r.modules) out.push_back(m.get());
    return out;
}

void register_external_module(const GuestCodeModule& module) {
    if (module.base == 0 || module.size == 0 || module.base + module.size < module.base)
        throw std::invalid_argument("register_external_module: empty or wrapping module range");
    auto info = std::make_unique<GuestModule>();
    info->base = module.base;
    info->size = module.size;
    info->path = module.path;
    info->name = module.name;
    info->is_dll = true;
    info->tls_index = UINT32_MAX;
    info->image_hash = module.image_hash;
    info->text_begin = module.text_begin;
    info->text_end = module.text_end;
    if (info->text_begin == 0 && info->text_end == 0) {
        info->text_begin = module.base;
        info->text_end = module.base + module.size;
    }
    if (info->text_begin < module.base || info->text_end > module.base + module.size || info->text_end < info->text_begin)
        throw std::invalid_argument("register_external_module: text range outside the module");
    auto& r = registry();
    GuestModule replaced_copy;
    bool replaced = false;
    const GuestModule* added = nullptr;
    {
        std::lock_guard guard(r.lock);
        for (auto it = r.modules.begin(); it != r.modules.end(); ++it) {
            if ((*it)->base == module.base) {
                replaced_copy = **it;
                replaced = true;
                r.retired.push_back(std::move(*it));
                r.modules.erase(it);
                break;
            }
        }
        for (const auto& m : r.modules)
            if (m->base < module.base + module.size && module.base < m->base + m->size)
                throw std::invalid_argument("register_external_module: overlaps another external module");
        added = info.get();
        r.modules.push_back(std::move(info));
        publish();
    }
    if (replaced) cache_module_unloaded(replaced_copy);
    cache_module_loaded(*added);
}

void unregister_external_module(std::uint64_t base) {
    auto& r = registry();
    GuestModule copy;
    bool found = false;
    {
        std::lock_guard guard(r.lock);
        for (auto it = r.modules.begin(); it != r.modules.end(); ++it) {
            if ((*it)->base != base) continue;
            copy = **it;
            r.retired.push_back(std::move(*it));
            r.modules.erase(it);
            found = true;
            break;
        }
        if (found) publish();
    }
    if (!found) return;
    cache_module_unloaded(copy);
    invalidate_guest_code(copy.base, copy.size);
}

void loader_thread_attach(GuestContext&) {}
void loader_thread_detach(GuestContext&) {}
void* guest_activation_context() noexcept { return nullptr; }
std::uint64_t guest_image_stack_reserve() noexcept { return 0; }   // the runtime default
std::uint64_t guest_image_stack_commit() noexcept { return 0; }
std::uint64_t bind_unknown_host_target(const void*) { return 0; }
bool abi_code_frame(std::uint64_t, AbiCodeFrame&) noexcept { return false; }

} // namespace transmute::fx
