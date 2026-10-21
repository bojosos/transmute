// Guest module registry, load transactions, import binding, exports and the
// module APIs of transmute/fx/process.hpp.

#include "fx/loader/loader_internal.hpp"

#include "fx/cache/cache.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <intrin.h>
#include <cstdio>
#include <cstring>

namespace transmute::fx {
namespace loader {
namespace {

// Lock-free lookup table (immutable once published).
std::atomic<const ModuleTable*> g_table{nullptr};

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

std::string ordinal_display(std::uint32_t ordinal) { return "#" + std::to_string(ordinal); }

const char* c_string_in(const Module& m, std::uint64_t rva) {
    if (rva == 0 || rva >= m.image.headers.size_of_image) return nullptr;
    const char* s = reinterpret_cast<const char*>(m.image.base + rva);
    if (!std::memchr(s, 0, m.image.headers.size_of_image - rva)) return nullptr;
    return s;
}

ExportView& exports_of(Module& m) {
    if (!m.exports_opened) {
        m.exports_opened = true;
        m.exports.open(m.image.base);
    }
    return m.exports;
}

void print_diagnostic(const LoaderDiagnostic& d) {
    static const char* kinds[] = {"missing DLL", "missing function", "import cycle", "bad image",
                                  "DllMain failed", "note"};
    std::fprintf(stderr, "transmute-fx loader: %s: %s%s%s%s%s%s\n",
                 kinds[static_cast<unsigned>(d.kind)], narrow(d.module).c_str(),
                 d.dll.empty() ? "" : " -> ", d.dll.c_str(), d.function.empty() ? "" : "!",
                 d.function.c_str(), d.message.empty() ? "" : (" (" + d.message + ")").c_str());
}

bool owned_by(const Module* m, const LoadTransaction& tx) { return m && m->tx == &tx; }

void add_edge(Module& from, Module* to, LoadTransaction& tx) {
    if (!to || to == &from) return;
    if (std::find(from.deps.begin(), from.deps.end(), to) != from.deps.end()) return;
    from.deps.push_back(to);
    ++to->refcount;
    if (!owned_by(to, tx)) tx.retained.push_back(to);
}

Module* load_module_tx(const std::wstring& path, const SearchContext& search, LoadTransaction& tx,
                       std::string& error, bool is_exe);
std::uint64_t resolve_guest_export(Module& module, const char* name, std::uint32_t ordinal,
                                   std::uint32_t hint, LoadTransaction& tx,
                                   const SearchContext& search, unsigned depth);
bool finish_transaction(LoadTransaction& tx);

std::string host_module_name(HMODULE h) {
    auto& s = state();
    if (const auto it = s.host_names.find(h); it != s.host_names.end()) return it->second;
    wchar_t buffer[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(h, buffer, static_cast<DWORD>(std::size(buffer)));
    std::string name = n ? narrow(file_name_of(std::wstring_view(buffer, n))) : std::string{};
    s.host_names.emplace(h, name);
    return name;
}

std::string host_export_name(HMODULE h, std::uint32_t ordinal) {
    ExportView view;
    if (view.open(reinterpret_cast<const std::uint8_t*>(h))) {
        const auto name = view.name_of_ordinal(ordinal);
        if (!name.empty()) return std::string(name);
    }
    return ordinal_display(ordinal);
}

HMODULE load_host_dll(const std::wstring& name) {
    HostActivation activation;
    HMODULE h = LoadLibraryExW(name.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!h) h = LoadLibraryExW(name.c_str(), nullptr, 0);
    return h;
}

// Resolves an imported DLL name to a guest module (loading it within `tx`)
// or a host module.
Dependency resolve_dependency(std::string_view dll, const SearchContext& search,
                              LoadTransaction& tx, const Module* importer) {
    auto& s = state();
    const std::wstring normalized = normalize_dll_name(widen(dll));
    const std::wstring key = lower(normalized);
    Dependency dep;
    if (Module* g = find_loaded(normalized)) {
        if (std::find(tx.loading.begin(), tx.loading.end(), g) != tx.loading.end() && importer) {
            LoaderDiagnostic d;
            d.kind = LoaderDiagnostic::Kind::import_cycle;
            d.module = importer->info.name;
            d.dll = std::string(dll);
            d.message = "circular import resolved against the partially loaded module";
            add_diagnostic(std::move(d));
        }
        dep.guest = g;
        return dep;
    }
    if (const auto it = s.host_by_name.find(key); it != s.host_by_name.end()) return it->second;
    const bool host_only = !has_path_separator(normalized) &&
                           (is_api_set_name(normalized) || is_known_dll(key));
    if (!host_only) {
        const std::wstring path = find_guest_file(normalized, search);
        if (!path.empty()) {
            if (Module* g = find_loaded(path)) {
                dep.guest = g;
                return dep;
            }
            std::string error;
            if (Module* g = load_module_tx(path, search, tx, error, false)) {
                dep.guest = g;
                return dep;
            }
            LoaderDiagnostic d;
            d.kind = LoaderDiagnostic::Kind::bad_image;
            d.module = importer ? importer->info.name : std::wstring{};
            d.dll = narrow(path);
            d.message = error;
            add_diagnostic(std::move(d));
        }
    }
    dep.host = load_host_dll(normalized);
    if (dep.host) {
        dep.host_name = narrow(normalized);
        s.host_names.emplace(dep.host, dep.host_name);
        s.host_by_name.emplace(key, dep);
        ++s.stats.host_dlls;
    }
    return dep;
}

std::uint64_t bind_host(const Dependency& dep, std::string_view dll, const char* name,
                        std::uint32_t ordinal) {
    FARPROC p = name ? GetProcAddress(dep.host, name)
                     : GetProcAddress(dep.host, MAKEINTRESOURCEA(ordinal));
    if (!p) {
        // Exports the x64 DLL lacks (ARM64-only CRT exports such as fabsf)
        // may still have a native implementation in the abi component.
        if (!name) return 0;
        const std::uint64_t replacement = abi_guest_import_replacement(dll, name);
        if (replacement) ++state().stats.replaced_imports;
        return replacement;
    }
    const std::string function = name ? std::string(name) : host_export_name(dep.host, ordinal);
    return bind_host_function(dll, function, reinterpret_cast<const void*>(p));
}

std::uint64_t resolve_forwarder(std::string_view forwarder, Module& owner, LoadTransaction& tx,
                                const SearchContext& search, unsigned depth) {
    auto& s = state();
    if (depth > 16) return 0;
    const auto dot = forwarder.rfind('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 >= forwarder.size()) return 0;
    const std::string dll = std::string(forwarder.substr(0, dot)) + ".dll";
    const std::string function(forwarder.substr(dot + 1));
    ++s.stats.forwarders;
    std::uint32_t ordinal = 0;
    const bool by_ordinal = function[0] == '#';
    if (by_ordinal) ordinal = static_cast<std::uint32_t>(std::strtoul(function.c_str() + 1, nullptr, 10));
    Dependency dep = resolve_dependency(dll, search, tx, &owner);
    if (dep.guest) {
        add_edge(owner, dep.guest, tx);
        if (!by_ordinal) {
            if (const auto r = abi_guest_import_replacement(dll, function)) return r;
        }
        return resolve_guest_export(*dep.guest, by_ordinal ? nullptr : function.c_str(), ordinal,
                                    0xffffffffu, tx, search, depth + 1);
    }
    if (dep.host) return bind_host(dep, dll, by_ordinal ? nullptr : function.c_str(), ordinal);
    return 0;
}

std::uint64_t resolve_guest_export(Module& module, const char* name, std::uint32_t ordinal,
                                   std::uint32_t hint, LoadTransaction& tx,
                                   const SearchContext& search, unsigned depth) {
    const ExportView& view = exports_of(module);
    const ExportLookup r = name ? view.by_name(name, hint) : view.by_ordinal(ordinal);
    if (r.kind == ExportLookup::Kind::address) return r.address;
    if (r.kind == ExportLookup::Kind::forwarder) {
        return resolve_forwarder(r.forwarder, module, tx, search, depth);
    }
    return 0;
}

std::uint64_t bind_missing(const Module& importer, std::string_view dll, const std::string& function,
                           bool report) {
    auto& s = state();
    ++s.stats.missing_imports;
    if (report) {
        LoaderDiagnostic d;
        d.kind = LoaderDiagnostic::Kind::missing_function;
        d.module = importer.info.name;
        d.dll = std::string(dll);
        d.function = function;
        add_diagnostic(std::move(d));
    }
    return missing_import_thunk(std::string(dll), function);
}

void resolve_imports(Module& m, const SearchContext& search, LoadTransaction& tx) {
    auto& s = state();
    const PeDirectory dir = m.image.headers.dir(dir_import);
    if (dir.rva == 0 || dir.size == 0) return;
    const std::uint32_t image_size = m.image.headers.size_of_image;
    std::uint8_t* base = m.image.base;
    for (std::uint64_t at = dir.rva; at + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= image_size;
         at += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        IMAGE_IMPORT_DESCRIPTOR desc;
        std::memcpy(&desc, base + at, sizeof(desc));
        if (desc.Name == 0 && desc.FirstThunk == 0) break;
        const char* dll_name = c_string_in(m, desc.Name);
        if (!dll_name || desc.FirstThunk == 0) continue;
        const std::string_view dll(dll_name);
        Dependency dep = resolve_dependency(dll, search, tx, &m);
        if (dep.guest) add_edge(m, dep.guest, tx);
        const bool missing_dll = !dep.guest && !dep.host;
        if (missing_dll) {
            LoaderDiagnostic d;
            d.kind = LoaderDiagnostic::Kind::missing_dll;
            d.module = m.info.name;
            d.dll = std::string(dll);
            add_diagnostic(std::move(d));
        }
        const std::uint32_t ilt = desc.OriginalFirstThunk ? desc.OriginalFirstThunk : desc.FirstThunk;
        const std::uint32_t iat = desc.FirstThunk;
        for (std::uint64_t i = 0;; ++i) {
            const std::uint64_t ilt_at = ilt + i * 8, iat_at = iat + i * 8;
            if (ilt_at + 8 > image_size || iat_at + 8 > image_size) break;
            std::uint64_t thunk;
            std::memcpy(&thunk, base + ilt_at, 8);
            if (thunk == 0) break;
            const char* name = nullptr;
            std::uint32_t ordinal = 0;
            std::uint32_t hint = 0xffffffffu;
            if (thunk & IMAGE_ORDINAL_FLAG64) {
                ordinal = static_cast<std::uint32_t>(thunk & 0xffff);
            } else {
                const std::uint64_t by_name = thunk & 0x7fffffffu;
                if (by_name + 2 >= image_size) continue;
                std::uint16_t h;
                std::memcpy(&h, base + by_name, 2);
                hint = h;
                name = c_string_in(m, by_name + 2);
                if (!name) continue;
            }
            std::uint64_t target = 0;
            if (dep.guest) {
                std::string function = name ? std::string(name) : std::string{};
                if (!name) function = std::string(exports_of(*dep.guest).name_of_ordinal(ordinal));
                if (!function.empty()) target = abi_guest_import_replacement(dll, function);
                if (target) {
                    ++s.stats.replaced_imports;
                } else {
                    target = resolve_guest_export(*dep.guest, name, ordinal, hint, tx, search, 0);
                    if (target) ++s.stats.guest_imports;
                }
                if (!target) {
                    target = bind_missing(m, dll, name ? std::string(name) : ordinal_display(ordinal), true);
                }
            } else if (dep.host) {
                const std::uint64_t replaced_before = s.stats.replaced_imports;
                target = bind_host(dep, dll, name, ordinal);
                if (!target) {
                    target = bind_missing(m, dll, name ? std::string(name) : ordinal_display(ordinal), true);
                } else if (s.stats.replaced_imports == replaced_before) {
                    ++s.stats.host_imports;
                }
            } else {
                target = bind_missing(m, dll, name ? std::string(name) : ordinal_display(ordinal), false);
            }
            std::memcpy(base + iat_at, &target, 8);
        }
    }
}

Module* load_module_tx(const std::wstring& raw_path, const SearchContext& search, LoadTransaction& tx,
                       std::string& error, bool is_exe) {
    auto& s = state();
    const std::wstring path = full_path(raw_path);
    auto owned = std::make_unique<Module>();
    Module& m = *owned;
    MapOptions options;
    options.force_relocation = s.config.force_relocation;
    const auto start = Clock::now();
    if (!map_image(path, options, m.image, error)) return nullptr;
    s.stats.map_seconds += seconds_since(start);
    ++s.stats.modules_mapped;
    if (m.image.delta != 0) ++s.stats.modules_relocated;
    m.info.base = reinterpret_cast<std::uint64_t>(m.image.base);
    m.info.size = m.image.size;
    m.info.entry = m.image.headers.entry_rva ? m.info.base + m.image.headers.entry_rva : 0;
    m.info.path = path;
    m.info.name = file_name_of(path);
    m.info.is_dll = m.image.headers.is_dll();
    m.info.pdata_rva = m.image.headers.dir(dir_exception).rva;
    m.info.pdata_size = m.image.headers.dir(dir_exception).size;
    m.info.tls_index = kNoTls;
    m.info.image_hash = m.image.image_hash;
    m.info.text_begin = m.image.text_begin;
    m.info.text_end = m.image.text_end;
    m.info.stack_reserve = m.image.headers.stack_reserve;
    m.info.stack_commit = m.image.headers.stack_commit;
    m.name_utf8 = narrow(m.info.name);
    m.key = lower(m.info.name);
    m.path_key = lower(path);
    m.directory = directory_of(path);
    m.is_exe = is_exe;
    m.static_load = tx.static_load;
    m.tx = &tx;
    // Start opening the module's cache file now, while its imports are bound
    // (published by finish_transaction's cache_module_loaded).
    cache_module_mapped(m.info);
    s.modules.push_back(std::move(owned));
    if (is_exe) {
        s.exe = &m;
        load_sxs_for_exe(path, &m.image);
    }
    assign_tls_slot(m);
    if (!tx.skip_imports) {
        const bool root = tx.loading.empty();
        const double map_before = s.stats.map_seconds;
        const auto bind_start = Clock::now();
        tx.loading.push_back(&m);
        resolve_imports(m, search, tx);
        tx.loading.pop_back();
        if (root) s.stats.bind_seconds += seconds_since(bind_start) - (s.stats.map_seconds - map_before);
    }
    m.imports_bound = true;
    tx.created.push_back(&m);
    return &m;
}

void remove_module(Module& m) {
    auto& s = state();
    s.init_order.erase(std::remove(s.init_order.begin(), s.init_order.end(), &m), s.init_order.end());
    release_tls_slot(m);
    for (auto it = s.modules.begin(); it != s.modules.end(); ++it) {
        if (it->get() == &m) {
            s.retired.push_back(std::move(*it));
            s.modules.erase(it);
            break;
        }
    }
    if (s.exe == &m) s.exe = nullptr;
}

void discard_module_memory(Module& m, bool notify) {
    if (notify) cache_module_unloaded(m.info);
    if (m.image.base) {
        invalidate_guest_code(m.info.base, m.info.size);
        unmap_image(m.image);
    }
}

bool initialize_module(Module& m, bool static_load) {
    auto& s = state();
    if (m.initialized) return true;
    run_tls_callbacks(m, DLL_PROCESS_ATTACH);
    if (m.info.is_dll && m.info.entry) {
        m.attach_called = true;
        if (!call_guest_init(m.info.entry, m.info.base, DLL_PROCESS_ATTACH, static_load ? 1 : 0)) {
            LoaderDiagnostic d;
            d.kind = LoaderDiagnostic::Kind::init_failed;
            d.module = m.info.name;
            d.message = "DllMain(DLL_PROCESS_ATTACH) returned FALSE";
            add_diagnostic(std::move(d));
            return false;
        }
    }
    m.initialized = true;
    s.init_order.push_back(&m);
    return true;
}

void rollback(LoadTransaction& tx, Module* failed) {
    if (failed && failed->attach_called) {
        call_guest_init(failed->info.entry, failed->info.base, DLL_PROCESS_DETACH, 0);
    }
    for (auto it = tx.created.rbegin(); it != tx.created.rend(); ++it) {
        Module& m = **it;
        if (&m != failed && m.initialized) {
            run_tls_callbacks(m, DLL_PROCESS_DETACH);
            if (m.info.is_dll && m.info.entry) {
                call_guest_init(m.info.entry, m.info.base, DLL_PROCESS_DETACH, 0);
            }
            m.initialized = false;
        }
    }
    for (Module* r : tx.retained) {
        if (r->refcount > 0) --r->refcount;
    }
    std::vector<Module*> created = tx.created;
    for (Module* m : created) remove_module(*m);
    publish_table();
    for (Module* m : created) discard_module_memory(*m, true);
    tx.created.clear();
    tx.retained.clear();
}

bool finish_transaction(LoadTransaction& tx) {
    auto& s = state();
    if (tx.created.empty()) return true;
    const auto protect_start = Clock::now();
    for (Module* m : tx.created) {
        std::string error;
        if (!protect_image(m->image, error)) {
            LoaderDiagnostic d;
            d.kind = LoaderDiagnostic::Kind::note;
            d.module = m->info.name;
            d.message = error;
            add_diagnostic(std::move(d));
        }
        m->tx = nullptr;
        if (tx.static_load) m->pinned = true;
    }
    s.stats.protect_seconds += seconds_since(protect_start);
    publish_table();
    for (Module* m : tx.created) cache_module_loaded(m->info);
    refresh_all_threads_tls();
    if (!s.config.run_initializers || tx.skip_imports) return true;
    // Initializers may load more modules (nested transactions); iterate a copy.
    static thread_local unsigned init_depth = 0;
    const auto init_start = Clock::now();
    ++init_depth;
    const std::vector<Module*> order = tx.created;
    for (Module* m : order) {
        if (!initialize_module(*m, tx.static_load)) {
            --init_depth;
            rollback(tx, m);
            return false;
        }
    }
    if (--init_depth == 0) s.stats.init_seconds += seconds_since(init_start);
    return true;
}

void unload_module(Module& m);

void release_reference(Module& m) {
    if (m.refcount > 0) --m.refcount;
    if (m.refcount == 0 && !m.pinned) unload_module(m);
}

void unload_module(Module& m) {
    auto& s = state();
    if (m.initialized && s.config.run_initializers && !s.exiting.load()) {
        if (m.info.is_dll && m.info.entry) {
            call_guest_init(m.info.entry, m.info.base, DLL_PROCESS_DETACH, 0);
        }
        run_tls_callbacks(m, DLL_PROCESS_DETACH);
    }
    m.initialized = false;
    std::vector<Module*> deps = m.deps;
    m.deps.clear();
    remove_module(m);
    publish_table();
    discard_module_memory(m, true);
    for (Module* d : deps) release_reference(*d);
}

} // namespace

// ---- loader lock -----------------------------------------------------------------

namespace {
using IsLockedFn = BOOLEAN(NTAPI*)(void* critical_section);
} // namespace

void LoaderMutex::lock() { inner_.lock(); }
bool LoaderMutex::try_lock() { return inner_.try_lock(); }
void LoaderMutex::unlock() { inner_.unlock(); }

bool LoaderMutex::lock_bounded(unsigned milliseconds) {
    const ULONGLONG deadline = GetTickCount64() + milliseconds;
    for (unsigned spin = 0;; ++spin) {
        if (inner_.try_lock()) return true;
        if (GetTickCount64() >= deadline) return false;
        if (spin < 64) YieldProcessor();
        else Sleep(1);
    }
}

bool host_loader_lock_held() noexcept {
    static const auto is_locked = reinterpret_cast<IsLockedFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlIsCriticalSectionLockedByThread"));
    if (!is_locked) return false;
    // PEB->LoaderLock (x64 offset 0x110).
    auto* peb = reinterpret_cast<std::uint8_t*>(__readgsqword(0x60));
    void* loader_lock = *reinterpret_cast<void**>(peb + 0x110);
    return loader_lock && is_locked(loader_lock) != 0;
}

// ---- registry -----------------------------------------------------------------

State& state() {
    static State* instance = new State;
    return *instance;
}

void add_diagnostic(LoaderDiagnostic diagnostic) {
    auto& s = state();
    if (s.config.verbose) print_diagnostic(diagnostic);
    s.diagnostics.push_back(std::move(diagnostic));
}

void publish_table() {
    auto& s = state();
    auto table = std::make_unique<ModuleTable>();
    table->entries.reserve(s.modules.size());
    for (const auto& m : s.modules) {
        if (!m->image.base) continue;
        table->entries.push_back({m->info.base, m->info.base + m->info.size, &m->info});
    }
    for (const auto& m : s.external) table->entries.push_back({m->base, m->base + m->size, m.get()});
    std::sort(table->entries.begin(), table->entries.end(),
              [](const ModuleTable::Entry& a, const ModuleTable::Entry& b) { return a.base < b.base; });
    g_table.store(table.get(), std::memory_order_release);
    // Readers may still hold older tables; they are kept for the process lifetime.
    s.retired_tables.push_back(std::move(table));
}

Module* find_loaded(std::wstring_view name) {
    auto& s = state();
    if (name.empty()) return nullptr;
    const std::wstring normalized = normalize_dll_name(name);
    if (has_path_separator(normalized)) {
        const std::wstring key = lower(full_path(normalized));
        for (const auto& m : s.modules) {
            if (m->path_key == key) return m.get();
        }
        return nullptr;
    }
    const std::wstring key = lower(normalized);
    for (const auto& m : s.modules) {
        if (m->key == key) return m.get();
    }
    return nullptr;
}

Module* module_by_base(std::uint64_t base) {
    for (const auto& m : state().modules) {
        if (m->info.base == base) return m.get();
    }
    return nullptr;
}

bool call_guest_init(std::uint64_t entry, std::uint64_t base, std::uint32_t reason,
                     std::uint64_t reserved) {
    const std::uint64_t args[3] = {base, reason, reserved};
    try {
        return static_cast<std::uint32_t>(call_guest(entry, args)) != 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "transmute-fx loader: initializer at 0x%llx failed: %s\n",
                     static_cast<unsigned long long>(entry), error.what());
        return false;
    }
}

std::uint64_t guest_load_internal(std::wstring_view name, std::uint32_t flags, bool static_load,
                                  std::string* error_out) {
    auto& s = state();
    std::lock_guard lock(s.lock);
    if (name.empty()) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    constexpr std::uint32_t kResourceFlags = LOAD_LIBRARY_AS_DATAFILE |
                                             LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE |
                                             LOAD_LIBRARY_AS_IMAGE_RESOURCE;
    SearchContext search;
    search.flags = flags;
    if (has_path_separator(name)) search.dll_load_dir = directory_of(full_path(normalize_dll_name(name)));
    if (flags & kResourceFlags) {
        if (!(flags & (LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE))) {
            if (Module* m = find_loaded(name)) {
                ++m->refcount;
                return m->info.base;
            }
        }
        std::wstring path = find_any_file(name, search);
        if (path.empty()) path = std::wstring(name);
        HostActivation activation;
        HMODULE h = LoadLibraryExW(path.c_str(), nullptr, flags & (kResourceFlags | 0xff00u));
        return reinterpret_cast<std::uint64_t>(h);
    }
    if (Module* m = find_loaded(name)) {
        ++m->refcount;
        return m->info.base;
    }
    const std::wstring normalized = normalize_dll_name(name);
    const std::wstring key = lower(file_name_of(normalized));
    const bool by_path = has_path_separator(normalized);
    const bool host_only = !by_path && (is_api_set_name(normalized) || is_known_dll(key));
    if (!host_only) {
        const std::wstring path = find_guest_file(normalized, search);
        if (!path.empty()) {
            if (Module* m = find_loaded(path)) {
                ++m->refcount;
                return m->info.base;
            }
            LoadTransaction tx;
            tx.static_load = static_load;
            tx.skip_imports = (flags & DONT_RESOLVE_DLL_REFERENCES) != 0;
            std::string error;
            Module* m = load_module_tx(path, search, tx, error, false);
            if (!m) {
                // Dependencies created before the failure are discarded too.
                rollback(tx, nullptr);
                if (error_out) *error_out = error;
                LoaderDiagnostic d;
                d.kind = LoaderDiagnostic::Kind::bad_image;
                d.module = file_name_of(path);
                d.message = error;
                add_diagnostic(std::move(d));
                SetLastError(ERROR_BAD_EXE_FORMAT);
                return 0;
            }
            ++m->refcount;
            const std::uint64_t base = m->info.base;
            if (!finish_transaction(tx)) {
                if (error_out) *error_out = "DllMain failed";
                SetLastError(ERROR_DLL_INIT_FAILED);
                return 0;
            }
            return base;
        }
    }
    HostActivation activation;
    HMODULE h = by_path ? LoadLibraryExW(full_path(normalized).c_str(), nullptr, flags)
                        : LoadLibraryExW(normalized.c_str(), nullptr, flags);
    if (!h) {
        if (error_out) *error_out = "host LoadLibraryExW failed";
        return 0;
    }
    ++s.host_guest_refs[h];
    return reinterpret_cast<std::uint64_t>(h);
}

std::uint64_t load_exe(const std::wstring& path, std::string& error, bool& init_failed) {
    auto& s = state();
    std::lock_guard lock(s.lock);
    init_failed = false;
    LoadTransaction tx;
    tx.static_load = true;
    SearchContext search;
    Module* exe = load_module_tx(path, search, tx, error, true);
    if (!exe) {
        rollback(tx, nullptr);
        return 0;
    }
    exe->refcount = 1;
    const std::uint64_t base = exe->info.base;
    if (!finish_transaction(tx)) {
        init_failed = true;
        error = "a DLL initialization routine failed";
        return 0;
    }
    return base;
}

void reset_registry_for_testing() {
    auto& s = state();
    std::vector<Module*> all;
    for (const auto& m : s.modules) all.push_back(m.get());
    for (auto it = all.rbegin(); it != all.rend(); ++it) {
        (*it)->initialized = false;
        remove_module(**it);
    }
    for (auto& m : s.external) s.external_retired.push_back(std::move(m));
    s.external.clear();
    publish_table();
    for (Module* m : all) discard_module_memory(*m, false);
    s.init_order.clear();
    s.exe = nullptr;
    s.host_by_name.clear();
    s.host_guest_refs.clear();
    s.host_names.clear();
}

} // namespace loader

using namespace loader;

// ---- process.hpp: loader section ---------------------------------------------------

const GuestModule* find_guest_module(std::uint64_t address) noexcept {
    const ModuleTable* table = g_table.load(std::memory_order_acquire);
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
    auto& s = state();
    std::lock_guard lock(s.lock);
    std::vector<const GuestModule*> out;
    out.reserve(s.modules.size());
    for (const auto& m : s.modules) {
        if (!m->tx) out.push_back(&m->info);
    }
    for (const auto& m : s.external) out.push_back(m.get());
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
    if (info->text_begin < module.base || info->text_end > module.base + module.size ||
        info->text_end < info->text_begin)
        throw std::invalid_argument("register_external_module: text range outside the module");
    const auto overlaps = [&](std::uint64_t base, std::uint64_t size) {
        return base < module.base + module.size && module.base < base + size;
    };
    auto& s = state();
    const GuestModule* replaced = nullptr;
    const GuestModule* added = nullptr;   // owned by the registry; never freed
    GuestModule copy;
    {
        std::lock_guard lock(s.lock);
        for (const auto& m : s.modules)
            if (m->image.base && overlaps(m->info.base, m->info.size))
                throw std::invalid_argument("register_external_module: overlaps a PE guest module");
        for (auto it = s.external.begin(); it != s.external.end(); ++it) {
            if ((*it)->base == module.base) {
                replaced = it->get();
                copy = **it;
                s.external_retired.push_back(std::move(*it));
                s.external.erase(it);
                break;
            }
        }
        for (const auto& m : s.external)
            if (overlaps(m->base, m->size))
                throw std::invalid_argument("register_external_module: overlaps another external module");
        added = info.get();
        s.external.push_back(std::move(info));
        publish_table();
    }
    if (replaced) cache_module_unloaded(copy);
    // The cache keys it by image_hash (no PE headers); 0 = not cached.
    cache_module_loaded(*added);
}

void unregister_external_module(std::uint64_t base) {
    auto& s = state();
    GuestModule copy;
    bool found = false;
    {
        std::lock_guard lock(s.lock);
        for (auto it = s.external.begin(); it != s.external.end(); ++it) {
            if ((*it)->base != base) continue;
            copy = **it;
            s.external_retired.push_back(std::move(*it));
            s.external.erase(it);
            found = true;
            break;
        }
        if (found) publish_table();
    }
    if (!found) return;
    cache_module_unloaded(copy);
    invalidate_guest_code(copy.base, copy.size);
}

std::uint64_t guest_load_library(std::wstring_view name, std::uint32_t flags) {
    return guest_load_internal(name, flags, false, nullptr);
}

std::uint64_t guest_get_module_handle(std::wstring_view name) {
    auto& s = state();
    std::lock_guard lock(s.lock);
    if (name.empty()) return s.exe ? s.exe->info.base : 0;
    if (Module* m = find_loaded(name)) return m->info.base;
    const std::wstring copy(name);
    HostActivation activation;
    return reinterpret_cast<std::uint64_t>(GetModuleHandleW(copy.c_str()));
}

std::uint64_t guest_get_proc_address(std::uint64_t module, const char* name) {
    auto& s = state();
    std::lock_guard lock(s.lock);
    if (module == 0 && s.exe) module = s.exe->info.base;
    const bool by_ordinal = reinterpret_cast<std::uintptr_t>(name) < 0x10000;
    const std::uint32_t ordinal =
        by_ordinal ? static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(name)) : 0;
    if (!by_ordinal && !name) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (Module* m = module_by_base(module)) {
        if (!by_ordinal) {
            if (const auto r = abi_guest_import_replacement(m->name_utf8, name)) return r;
        }
        LoadTransaction tx;
        SearchContext search;
        const std::uint64_t address =
            resolve_guest_export(*m, by_ordinal ? nullptr : name, ordinal, 0xffffffffu, tx, search, 0);
        if (!finish_transaction(tx)) return 0;
        if (!address) SetLastError(ERROR_PROC_NOT_FOUND);
        return address;
    }
    if (module == 0 || (module & 3) != 0) {
        SetLastError(module == 0 ? ERROR_MOD_NOT_FOUND : ERROR_PROC_NOT_FOUND);
        return 0;
    }
    HMODULE h = reinterpret_cast<HMODULE>(module);
    FARPROC p = GetProcAddress(h, name);
    if (!p) {
        if (by_ordinal) return 0;
        const DWORD error = GetLastError();
        const std::uint64_t replacement = abi_guest_import_replacement(host_module_name(h), name);
        if (!replacement) SetLastError(error);
        return replacement;
    }
    const std::string function = by_ordinal ? host_export_name(h, ordinal) : std::string(name);
    return bind_host_function(host_module_name(h), function, reinterpret_cast<const void*>(p));
}

bool guest_pin_library(std::uint64_t module) {
    auto& s = state();
    std::lock_guard lock(s.lock);
    if (Module* m = module_by_base(module)) {
        m->pinned = true;
        return true;
    }
    return false;
}

bool guest_free_library(std::uint64_t module) {
    auto& s = state();
    std::lock_guard lock(s.lock);
    if (Module* m = module_by_base(module)) {
        release_reference(*m);
        return true;
    }
    HMODULE h = reinterpret_cast<HMODULE>(module);
    if (const auto it = s.host_guest_refs.find(h); it != s.host_guest_refs.end() && it->second > 0) {
        --it->second;
        return FreeLibrary(h) != FALSE;
    }
    for (const auto& [key, dep] : s.host_by_name) {
        if (dep.host == h) return true;   // pinned by guest imports
    }
    if (module & 3) return FreeLibrary(h) != FALSE;   // datafile / image-resource handle
    SetLastError(ERROR_MOD_NOT_FOUND);
    return false;
}

// ---- loader.hpp ------------------------------------------------------------------

void loader_configure(const LoaderConfig& config) {
    auto& s = state();
    prepare_image_readers();   // threads must be created outside the loader lock
    std::lock_guard lock(s.lock);
    s.config = config;
    s.configured = true;
    s.dll_dirs.clear();
    for (const auto& d : config.dll_directories) {
        if (!d.empty()) s.dll_dirs.push_back(full_path(d));
    }
    if (!config.application_path.empty()) {
        const std::wstring path = full_path(config.application_path);
        s.app_dir = directory_of(path);
        s.image_path = path;
        s.sxs.clear();
        s.sxs_loaded = false;
        release_host_activation_context();
    }
}

std::vector<LoaderDiagnostic> loader_diagnostics() {
    auto& s = state();
    std::lock_guard lock(s.lock);
    return s.diagnostics;
}

void loader_clear_diagnostics() {
    auto& s = state();
    std::lock_guard lock(s.lock);
    s.diagnostics.clear();
}

LoaderStats loader_stats() {
    auto& s = state();
    std::lock_guard lock(s.lock);
    return s.stats;
}

bool guest_disable_thread_library_calls(std::uint64_t module) {
    auto& s = state();
    std::lock_guard lock(s.lock);
    if (Module* m = module_by_base(module)) {
        m->thread_calls = false;
        return true;
    }
    SetLastError(ERROR_INVALID_HANDLE);
    return false;
}

std::uint64_t guest_add_dll_directory(std::wstring_view directory) {
    auto& s = state();
    std::lock_guard lock(s.lock);
    const std::wstring dir(directory);
    const DWORD attributes = GetFileAttributesW(dir.c_str());
    if (dir.empty() || !has_path_separator(dir) || attributes == INVALID_FILE_ATTRIBUTES ||
        !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    const std::uint64_t cookie = s.next_cookie++;
    s.user_dirs.emplace_back(cookie, full_path(dir));
    return cookie;
}

bool guest_remove_dll_directory(std::uint64_t cookie) {
    auto& s = state();
    std::lock_guard lock(s.lock);
    for (auto it = s.user_dirs.begin(); it != s.user_dirs.end(); ++it) {
        if (it->first == cookie) {
            s.user_dirs.erase(it);
            return true;
        }
    }
    SetLastError(ERROR_INVALID_PARAMETER);
    return false;
}

bool guest_set_dll_directory(std::wstring_view directory) {
    auto& s = state();
    std::lock_guard lock(s.lock);
    s.set_dll_directory = directory.empty() ? std::wstring{} : full_path(directory);
    return true;
}

std::wstring guest_module_file_name(std::uint64_t module) {
    auto& s = state();
    std::lock_guard lock(s.lock);
    if (module == 0) return s.exe ? s.exe->info.path : s.image_path;
    if (Module* m = module_by_base(module)) return m->info.path;
    wchar_t buffer[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(reinterpret_cast<HMODULE>(module), buffer,
                                       static_cast<DWORD>(std::size(buffer)));
    return std::wstring(buffer, n);
}

void* guest_activation_context() noexcept {
    const HANDLE context = state().host_actctx;
    return context == INVALID_HANDLE_VALUE ? nullptr : context;
}

std::uint64_t guest_module_from_address(std::uint64_t address) noexcept {
    const GuestModule* m = find_guest_module(address);
    return m ? m->base : 0;
}

std::uint64_t guest_resolve_delay_loaded_api(std::uint64_t parent_module, std::uint64_t descriptor,
                                             std::uint64_t failure_dll_hook,
                                             std::uint64_t failure_system_hook,
                                             std::uint64_t thunk_address, std::uint32_t flags) {
    (void)failure_dll_hook;
    (void)flags;
    IMAGE_DELAYLOAD_DESCRIPTOR desc;
    std::memcpy(&desc, reinterpret_cast<const void*>(descriptor), sizeof(desc));
    const bool rva_based = (desc.Attributes.AllAttributes & 1u) != 0;
    auto address = [&](std::uint64_t value) { return rva_based ? parent_module + value : value; };
    const char* dll = reinterpret_cast<const char*>(address(desc.DllNameRVA));
    auto* handle_slot = reinterpret_cast<std::uint64_t*>(address(desc.ModuleHandleRVA));
    const std::uint64_t iat = address(desc.ImportAddressTableRVA);
    const std::uint64_t names = address(desc.ImportNameTableRVA);
    const std::uint64_t index = (thunk_address - iat) / 8;
    std::uint64_t name_thunk;
    std::memcpy(&name_thunk, reinterpret_cast<const void*>(names + index * 8), 8);
    const char* name = nullptr;
    if (name_thunk & IMAGE_ORDINAL_FLAG64) {
        name = reinterpret_cast<const char*>(static_cast<std::uintptr_t>(name_thunk & 0xffff));
    } else {
        name = reinterpret_cast<const char*>(address(name_thunk & 0x7fffffffffffffffull) + 2);
    }
    std::uint64_t module = desc.ModuleHandleRVA ? *handle_slot : 0;
    if (!module) {
        module = guest_load_library(widen(dll), 0);
        if (module && desc.ModuleHandleRVA) {
            InterlockedExchange64(reinterpret_cast<volatile LONG64*>(handle_slot),
                                  static_cast<LONG64>(module));
        }
    }
    std::uint64_t target = module ? guest_get_proc_address(module, name) : 0;
    if (!target && failure_system_hook) {
        const std::uint64_t args[2] = {reinterpret_cast<std::uint64_t>(dll),
                                       reinterpret_cast<std::uint64_t>(name)};
        target = call_guest(failure_system_hook, args);
    }
    if (target) {
        auto* slot = reinterpret_cast<std::uint64_t*>(thunk_address);
        MEMORY_BASIC_INFORMATION mbi{};
        DWORD old = 0;
        bool reprotect = false;
        if (VirtualQuery(slot, &mbi, sizeof(mbi)) == sizeof(mbi) &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE)) == 0) {
            reprotect = VirtualProtect(slot, 8, PAGE_READWRITE, &old) != FALSE;
        }
        InterlockedExchange64(reinterpret_cast<volatile LONG64*>(slot), static_cast<LONG64>(target));
        if (reprotect) VirtualProtect(slot, 8, old, &old);
    }
    return target;
}

void loader_reset_for_testing() {
    auto& s = state();
    std::lock_guard lock(s.lock);
    reset_registry_for_testing();
    reset_tls_for_testing();
    reset_missing_for_testing();
    s.config = LoaderConfig{};
    s.configured = false;
    s.app_dir.clear();
    s.dll_dirs.clear();
    s.user_dirs.clear();
    s.set_dll_directory.clear();
    s.sxs.clear();
    s.sxs_loaded = false;
    release_host_activation_context();
    s.diagnostics.clear();
    s.stats = LoaderStats{};
    s.command_line.clear();
    s.image_path.clear();
    s.exiting = false;
    s.detach_at_exit = false;
}

} // namespace transmute::fx
