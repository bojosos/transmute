#include "alchemy/module_loader.hpp"
#include "alchemy/alchemy_root.hpp"
#include "alchemy/engine_adapter.hpp"
#include "alchemy/host/files.hpp"
#include "alchemy/path.hpp"
#include "alchemy/perf.hpp"
#include "alchemy/runtime_libraries.hpp"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

namespace alchemy {
namespace {

std::string utf8(const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::uint64_t checked_add(std::uint64_t address, std::uint64_t size) {
    if (address > UINT64_MAX - size) throw std::runtime_error("Mach-O loader address overflow");
    return address + size;
}

std::uint64_t aligned(std::uint64_t address) {
    return checked_add(address, 0x3fff) & ~0x3fffULL;
}

// Dylib bases are host allocation granules (64 KiB, a multiple of Darwin's
// 16 KiB page): an image mapped from its file owns the granules it touches,
// so the next image or allocation must not start inside its first one.
std::uint64_t granule_aligned(std::uint64_t address) {
    return checked_add(address, 0xffff) & ~0xffffULL;
}

// Optional engine capability: Mach-O segments mapped copy-on-write from the
// image file, so every process shares the file's pages until it writes them
// (alchemy-fx). Empty when the engine copies segments instead (the reference
// engine); a refused range throws.
template <class Engine>
MachOImage::FileMapper engine_file_mapper(Engine& engine) {
    if constexpr (requires(std::uint64_t value, void* section) { engine.map_file_copy(value, value, section, value, value); }) {
        return [&engine](std::uint64_t base, std::uint64_t size, void* section, std::uint64_t offset, std::uint64_t file_bytes) {
            engine.map_file_copy(base, size, section, offset, file_bytes);
            return true;
        };
    } else {
        return {};
    }
}

std::string guest_string(const transmute::GuestMemory& memory, std::uint64_t address) {
    if (address == 0) throw std::runtime_error("null dynamic-loader string");
    std::string value;
    for (std::uint64_t index = 0; index < 65536; ++index) {
        const auto byte = memory.read8(checked_add(address, index));
        if (byte == 0) return value;
        value.push_back(static_cast<char>(byte));
    }
    throw std::runtime_error("dynamic-loader string exceeds 65535 bytes");
}

// Allow-listed Apple install names served by project-built guest runtime
// libraries from --runtime-dir (alchemy/runtime_libraries.hpp).
bool runtime_identity(std::string_view identity) {
    return runtime_library_identity(identity) != nullptr;
}

// Runtime identities without a resident library stay declaration-only
// provider identities (LLVM's libc++ profile); the others must be present.
bool runtime_provider_fallback(std::string_view identity) {
    const auto* found = runtime_library_identity(identity);
    return found && found->provider_fallback;
}

bool apple_system_path(std::string_view identity) {
    return identity.starts_with("/usr/lib/") || identity.starts_with("/System/Library/");
}

class InitializationFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

} // namespace

struct ModuleLoader::Implementation {
    struct Module {
        MachOImage image;
        // Names point into image.exports() (never resized after parsing).
        string_view_hash_map<std::size_t> export_index;
        std::uint64_t id{};
        std::uint64_t base{};
        std::vector<Module*> dependencies;
        std::vector<std::filesystem::path> runpaths;
        bool global{};
        bool initialized{};
        std::string initialization_failure;
        std::thread::id initializing;
    };
    struct Handle { Module* module{}; std::string provider; std::uint64_t references{}; bool first{}; bool default_scope{}; };
    struct Found { RuntimeExport value; bool weak{}; };

    ProcessContext& context;
    std::filesystem::path executable;
    std::optional<std::uint64_t> main_base;
    std::vector<std::filesystem::path> directories;
    std::vector<std::filesystem::path> runtime_directories;
    std::recursive_mutex mutex;
    std::condition_variable_any initialization_changed;
    std::map<std::thread::id, std::thread::id> initialization_waits;
    std::vector<std::unique_ptr<Module>> modules;
    std::map<std::filesystem::path, Module*> by_source;
    std::map<std::uint64_t, Handle> handles;
    std::map<std::string, std::uint64_t> guest_strings;
    std::vector<std::uint64_t> add_callbacks;
    std::vector<std::uint64_t> remove_callbacks;
    std::vector<std::uint64_t> startup;
    Module* main{};
    std::optional<MachOImage> preparsed_main;   // load_main(MachOImage)
    // File identity (volume serial, file index) -> module: finds an already
    // loaded image under another path with one handle open per new path,
    // instead of std::filesystem::equivalent against every loaded module.
    // Host file identity (volume, file id): the same file under another path.
    using FileIdentity = std::pair<std::uint64_t, std::array<std::uint8_t, 16>>;
    std::map<FileIdentity, Module*> by_identity;
    std::uint64_t next_base{0x200000000ULL};
    std::uint64_t next_id{1};
    std::uint64_t next_handle{0xa10000000ULL};

    Implementation(ProcessContext& selected, std::filesystem::path source,
                   std::optional<std::uint64_t> base,
                   std::vector<std::filesystem::path> dylib_dirs,
                   std::vector<std::filesystem::path> runtime_dirs)
        : context(selected), executable(std::filesystem::canonical(source)), main_base(base),
          directories(std::move(dylib_dirs)), runtime_directories(std::move(runtime_dirs)) {}

    Module* existing(const std::filesystem::path& source) {
        const auto found = by_source.find(source);
        if (found != by_source.end()) return found->second;
        if (const auto identity = file_identity(source)) {
            const auto known = by_identity.find(*identity);
            return known == by_identity.end() ? nullptr : known->second;
        }
        for (const auto& [path, module] : by_source) {
            std::error_code error;
            if (std::filesystem::equivalent(path, source, error)) return module;
        }
        return nullptr;
    }
    // nullopt where the host has no file identities: existing() then compares paths.
    static std::optional<FileIdentity> file_identity(const std::filesystem::path& path) {
        const auto file = host::files::open_metadata(path_utf8(path));
        if (!file) return std::nullopt;
        host::OwnedHandle owned(*file);
        const auto identity = host::files::identity(owned.get());
        if (!identity) return std::nullopt;
        return FileIdentity{identity->volume, identity->id};
    }

    std::filesystem::path expand(std::string_view path, const Module* loader) const {
        if (path == "@loader_path") return loader ? loader->image.source().parent_path() : executable.parent_path();
        if (path == "@executable_path") return executable.parent_path();
        if (path.starts_with("@loader_path/")) {
            return (loader ? loader->image.source().parent_path() : executable.parent_path()) /
                   utf8_path(path.substr(13));
        }
        if (path.starts_with("@executable_path/")) return executable.parent_path() / utf8_path(path.substr(17));
        if (path.starts_with('@')) throw std::runtime_error("unsupported dylib path form: " + std::string(path));
        return utf8_path(path);
    }

    std::optional<std::filesystem::path> find_path(std::string_view name, const Module* loader,
                                                  bool dynamic = false) const {
        std::vector<std::filesystem::path> candidates;
        const auto name_path = utf8_path(name);
        if (const auto* identity = runtime_library_identity(name)) {
            if (auto found = find_runtime_library(*identity, runtime_directories); !found.empty()) return found;
            return std::nullopt;
        } else {
            if (apple_system_path(name)) return std::nullopt;
            if (name.starts_with("@rpath/")) {
                const auto tail = utf8_path(name.substr(7));
                if (loader) for (const auto& directory : loader->runpaths) candidates.push_back(directory / tail);
                for (const auto& directory : directories) candidates.push_back(directory / tail);
            } else if (name.starts_with('@')) {
                candidates.push_back(expand(name, loader));
            } else if (!name.starts_with('/')) {
                if (name_path.is_absolute()) candidates.push_back(name_path);
                else {
                    if (dynamic) candidates.push_back(context.map_guest_path
                        ? context.map_guest_path(name) : context.working_directory / name_path);
                    candidates.push_back((loader ? loader->image.source().parent_path() : executable.parent_path()) / name_path);
                }
            } else if (context.map_guest_path) {
                try { candidates.push_back(context.map_guest_path(name)); }
                catch (const std::runtime_error&) {} // an absolute name no Alchemy path can hold
            }
            for (const auto& directory : directories) candidates.push_back(directory / name_path.filename());
        }
        for (const auto& candidate : candidates) {
            std::error_code error;
            if (std::filesystem::is_regular_file(candidate, error)) return installed_real_path(candidate);
        }
        return std::nullopt;
    }

    bool provider(std::string_view name) const {
        return ProcessContext::is_provider_library(name) || runtime_provider_fallback(name);
    }

    Module* load(const std::filesystem::path& source, const Module* parent,
                 bool global, bool root = false) {
        if (auto* found = existing(source)) {
            return found;
        }
        if (modules.size() >= 256) throw std::runtime_error("guest module limit exceeded");
        auto selected = std::make_unique<Module>();
        if (root && preparsed_main) {
            selected->image = std::move(*preparsed_main);
            preparsed_main.reset();
        } else {
            selected->image = MachOImage::load(source);
        }
        for (std::size_t index = 0; index < selected->image.exports().size(); ++index) {
            selected->export_index.emplace(selected->image.exports()[index].name, index);
        }
        if (apple_system_path(selected->image.install_name())) {
            const auto* identity = runtime_library_identity(selected->image.install_name());
            if (!identity) throw std::runtime_error("Apple system library identity requires an Alchemy provider: " + selected->image.install_name());
            verify_runtime_library(*identity, source, runtime_directories);
        }
        if (root && (selected->image.is_dylib() || selected->image.is_bundle())) {
            throw std::runtime_error("cannot launch a dylib or bundle as an executable");
        }
        if (!root && !selected->image.is_dylib() && !selected->image.is_bundle()) {
            throw std::runtime_error("guest dependency is not a dylib or bundle: " + utf8(source));
        }
        selected->id = next_id++;
        selected->global = global;
        const auto extent = aligned(selected->image.image_size());
        const auto place = [&] {
            next_base = granule_aligned(next_base);
            while (!context.main_engine().is_range_available(next_base, extent))
                next_base = granule_aligned(checked_add(next_base, checked_add(extent, 0x4000)));
            selected->base = next_base;
            next_base = granule_aligned(checked_add(next_base, checked_add(extent, 0x4000)));
        };
        if (root) selected->base = main_base.value_or(selected->image.preferred_base());
        else place();
        for (const auto& path : selected->image.runpaths()) {
            std::filesystem::path expanded;
            if (path.starts_with('/') && context.map_guest_path) {
                // An LC_RPATH with no host location (a build machine's /Volumes/CI/...)
                // is skipped, as dyld skips run paths that do not exist.
                try { expanded = context.map_guest_path(path); } catch (const std::runtime_error&) { continue; }
            } else expanded = expand(path, selected.get());
            if (!expanded.is_absolute() && !path.starts_with('/')) expanded = source.parent_path() / expanded;
            selected->runpaths.push_back(std::move(expanded));
        }
        if (parent) selected->runpaths.insert(selected->runpaths.end(), parent->runpaths.begin(), parent->runpaths.end());
        for (unsigned attempt = 0;; ++attempt) {
            try {
                selected->image.map(context.main_engine(), selected->base, engine_file_mapper(context.main_engine()));
                break;
            } catch (const std::exception&) {
                // A dylib's range taken by another host thread after the
                // availability check: place it at the next free range.
                if (root || attempt >= 8 || context.main_engine().is_range_available(selected->base, extent)) throw;
                place();
            }
        }
        // Everything binding needs is parsed; the segments hold the bytes.
        selected->image.release_file();
        if (root && context.main_image_mapped) context.main_image_mapped();
        auto* const module = selected.get();
        by_source.emplace(source, module);
#ifdef _WIN32
        if (const auto identity = file_identity(source)) by_identity.emplace(*identity, module);
#endif
        modules.push_back(std::move(selected));
        if (root) main = module;
        for (const auto& dependency : module->image.dependencies()) {
            const auto path = find_path(dependency.install_name, module);
            if (!path) {
                if (dependency.install_name == "/usr/lib/libSystem.B.dylib") load_system_unwinder(module);
                if (provider(dependency.install_name) || dependency.weak) { module->dependencies.push_back(nullptr); continue; }
                throw std::runtime_error("missing guest dylib: " + dependency.install_name);
            }
            auto* target = load(*path, module, global);
            if (!target->image.is_dylib()) throw std::runtime_error("static dependency is not a dylib: " + dependency.install_name);
            if (runtime_identity(dependency.install_name) && target->image.install_name() != dependency.install_name) {
                throw std::runtime_error("guest runtime library identity mismatch: " + dependency.install_name);
            }
            module->dependencies.push_back(target);
        }
        return module;
    }

    RuntimeExport export_value(const Module& module, const MachOExport& exported) const {
        RuntimeExport result;
        result.address = exported.absolute ? exported.offset : checked_add(module.base, exported.offset);
        if (exported.thread_local_descriptor) result.kind = ExportKind::thread_local_descriptor;
        else {
            result.kind = ExportKind::data;
            for (const auto& segment : module.image.segments()) {
                if (segment.name == "__PAGEZERO") continue;
                const auto begin = checked_add(module.base, segment.address - module.image.preferred_base());
                if (result.address >= begin && result.address - begin < segment.size &&
                    (segment.initial_permissions & 4U) != 0) {
                    result.kind = ExportKind::function;
                    for (const auto& section : segment.sections) {
                        const auto section_begin = module.base + section.address - module.image.preferred_base();
                        if (result.address >= section_begin && result.address - section_begin < section.size) {
                            result.kind = (section.flags & 0x80000400U) != 0 ? ExportKind::function : ExportKind::data;
                            break;
                        }
                    }
                }
            }
        }
        return result;
    }

    std::optional<Found> exported(Module& module, std::string_view name,
                                  std::set<std::pair<std::uint64_t, std::string>>& seen) {
        if (!seen.emplace(module.id, std::string(name)).second || seen.size() > 4096) return std::nullopt;
        const auto indexed = module.export_index.find(name);
        if (indexed != module.export_index.end()) {
            const auto* found = &module.image.exports()[indexed->second];
            if (!found->reexport_ordinal) return Found{export_value(module, *found), found->weak_definition};
            const auto index = static_cast<std::size_t>(*found->reexport_ordinal - 1);
            const auto imported = found->imported_name.empty() ? name : std::string_view(found->imported_name);
            if (module.dependencies[index]) return exported(*module.dependencies[index], imported, seen);
            if (auto value = provider_symbol(module.image.dependencies()[index].install_name, imported)) return Found{*value, false};
            return std::nullopt;
        }
        for (std::size_t index = 0; index < module.dependencies.size(); ++index) {
            if (!module.image.dependencies()[index].reexport) continue;
            if (auto* dependency = module.dependencies[index]) {
                if (auto value = exported(*dependency, name, seen)) return value;
            } else if (auto value = provider_symbol(module.image.dependencies()[index].install_name, name)) return Found{*value, false};
        }
        return std::nullopt;
    }

    std::optional<Found> exported(Module& module, std::string_view name) {
        // Fast path (most lookups during binding): a direct export, or no
        // export and no re-exported dependency to search; no allocation.
        const auto indexed = module.export_index.find(name);
        if (indexed != module.export_index.end()) {
            const auto* found = &module.image.exports()[indexed->second];
            if (!found->reexport_ordinal) return Found{export_value(module, *found), found->weak_definition};
        } else if (std::none_of(module.image.dependencies().begin(), module.image.dependencies().end(),
                                [](const auto& dependency) { return dependency.reexport; })) {
            return std::nullopt;
        }
        std::set<std::pair<std::uint64_t, std::string>> seen;
        return exported(module, name, seen);
    }

    // Darwin's libSystem re-exports libunwind (LC_REEXPORT_DYLIB of its
    // libunwind), so an image that links only libSystem binds _Unwind_* (an
    // Objective-C @finally or C++ cleanup's _Unwind_Resume) through it. The
    // LLVM libunwind runtime library (from --runtime-dir, provenance-checked)
    // is therefore loaded with the first libSystem dependency, when no
    // libunwind is resident yet; without one the names stay unresolved.
    void load_system_unwinder(const Module* loader) {
        for (const auto& module : modules) if (module->image.install_name() == "/usr/lib/libunwind.dylib") return;
        if (const auto path = find_path("/usr/lib/libunwind.dylib", loader)) static_cast<void>(load(*path, loader, false));
    }

    // Darwin's libSystem re-exports libunwind. Alchemy's libSystem provider has
    // no unwinder, so libSystem lookups take the exports of the resident guest
    // LLVM libunwind (identity /usr/lib/libunwind.dylib) first, before the
    // provider and its diagnostic probe fallback. No resident libunwind leaves
    // unwinder names unresolved.
    std::optional<RuntimeExport> provider_symbol(std::string_view library, std::string_view name) {
        if (library == "/usr/lib/libSystem.B.dylib" || library.starts_with("/usr/lib/system/")) {
            for (const auto& module : modules) if (module->image.install_name() == "/usr/lib/libunwind.dylib") {
                if (auto found = exported(*module, name)) return found->value;
                break;
            }
        }
        return context.resolve(library, name);
    }

    std::optional<RuntimeExport> global_symbol(std::string_view name, bool all = false,
                                               std::size_t start = 0) {
        std::optional<RuntimeExport> weak;
        for (std::size_t index = start; index < modules.size(); ++index) {
            if (!all && !modules[index]->global) continue;
            if (auto found = exported(*modules[index], name)) {
                if (!found->weak) return found->value;
                if (!weak) weak = found->value;
            }
        }
        if (weak) return weak;
        for (const auto* library : {"/usr/lib/libSystem.B.dylib", "/usr/lib/libc++.1.dylib", "/usr/lib/libc++abi.dylib", "/usr/lib/libunwind.dylib",
                "/usr/lib/libobjc.A.dylib", "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation",
                "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation",
                "/System/Library/Frameworks/Accelerate.framework/Versions/A/Accelerate"}) {
            if (auto value = provider_symbol(library, name)) return value;
        }
        // As dyld's flat namespace searches every loaded image, also the
        // provider libraries the loaded images link (CoreServices, Carbon, ...),
        // in load order: a Node add-on finds FSEvents through Electron's links.
        std::set<std::string_view> searched;
        for (std::size_t index = start; index < modules.size(); ++index) {
            if (!all && !modules[index]->global) continue;
            const auto& dependencies = modules[index]->image.dependencies();
            for (std::size_t dependency = 0; dependency < dependencies.size() && dependency < modules[index]->dependencies.size(); ++dependency) {
                if (modules[index]->dependencies[dependency]) continue;   // a guest image: searched above
                const std::string_view library = dependencies[dependency].install_name;
                if (!searched.insert(library).second) continue;
                if (auto value = provider_symbol(library, name)) return value;
            }
        }
        return std::nullopt;
    }

    std::optional<RuntimeExport> scope_symbol(Module& source, std::string_view name,
                                               bool include_self, bool first = false) {
        struct Item { Module* module{}; std::string provider; };
        std::vector<Item> queue;
        std::set<std::uint64_t> queued_modules;
        std::set<std::string> queued_providers;
        const auto enqueue = [&](Module* module, std::string_view identity) {
            if (module) {
                if (queued_modules.insert(module->id).second) queue.push_back({module, {}});
            } else if (queued_providers.emplace(identity).second) queue.push_back({nullptr, std::string(identity)});
        };
        if (include_self) enqueue(&source, {});
        else {
            queued_modules.insert(source.id);
            for (std::size_t index = 0; index < source.dependencies.size(); ++index) {
                enqueue(source.dependencies[index], source.image.dependencies()[index].install_name);
            }
        }
        for (std::size_t index = 0; index < queue.size(); ++index) {
            const auto item = queue[index];
            if (item.module) {
                if (auto found = exported(*item.module, name)) {
                    if (found->weak) if (auto coalesced = global_symbol(name, true)) return coalesced;
                    return found->value;
                }
                if (!first) for (std::size_t dependency = 0; dependency < item.module->dependencies.size(); ++dependency) {
                    enqueue(item.module->dependencies[dependency], item.module->image.dependencies()[dependency].install_name);
                }
            } else if (auto found = provider_symbol(item.provider, name)) return found;
        }
        return std::nullopt;
    }

    std::optional<std::uint64_t> binding(Module& module, std::int32_t ordinal,
                                        std::string_view name) {
        std::optional<Found> found;
        if (module.image.flat_namespace() && ordinal >= 0) {
            if (auto value = global_symbol(name)) return value->address;
            if (auto value = scope_symbol(module, name, true)) return value->address;
            return std::nullopt;
        }
        if (ordinal == -2 || ordinal == -3) {
            if (auto value = global_symbol(name, ordinal == -3)) return value->address;
            return std::nullopt;
        }
        if (ordinal == 0) found = exported(module, name);
        else if (ordinal == -1 && main) found = exported(*main, name);
        else if (ordinal > 0) {
            const auto index = static_cast<std::size_t>(ordinal - 1);
            if (index >= module.dependencies.size()) throw std::runtime_error("invalid dylib ordinal");
            if (module.dependencies[index]) found = exported(*module.dependencies[index], name);
            else if (auto value = provider_symbol(module.image.dependencies()[index].install_name, name)) return value->address;
        } else throw std::runtime_error("unsupported special dylib ordinal");
        if (!found) return std::nullopt;
        if (found->weak) if (auto coalesced = global_symbol(name, true)) return coalesced->address;
        return found->value.address;
    }

    ImageRuntimeInfo metadata(Module& module) {
        ImageRuntimeInfo info;
        info.module_id = module.id;
        info.header = module.base;
        info.begin = module.base;
        info.end = checked_add(module.base, module.image.image_size());
        if (module.base >= module.image.preferred_base()) {
            const auto difference = module.base - module.image.preferred_base();
            if (difference > INT64_MAX) throw std::runtime_error("guest module slide exceeds signed range");
            info.slide = static_cast<std::int64_t>(difference);
        } else {
            const auto difference = module.image.preferred_base() - module.base;
            if (difference > INT64_MAX) throw std::runtime_error("guest module slide exceeds signed range");
            info.slide = -static_cast<std::int64_t>(difference);
        }
        info.name = utf8(module.image.source());
        for (const auto& segment : module.image.segments()) {
            if (segment.name == "__PAGEZERO" || segment.size == 0 || (segment.initial_permissions & 4U) == 0) continue;
            const auto begin = checked_add(module.base, segment.address - module.image.preferred_base());
            info.code_ranges.push_back({begin, checked_add(begin, segment.size)});
        }
        transmute::GuestMemory& memory = context.main_engine();
        std::uint64_t tls_begin = UINT64_MAX, initialized_end = 0, tls_end = 0;
        TlsTemplate tls;
        tls.module_id = module.id;
        for (const auto& segment : module.image.segments()) for (const auto& section : segment.sections) {
            const auto address = checked_add(module.base, section.address - module.image.preferred_base());
            info.sections.push_back({section.segment, section.name, address, section.size, section.flags});
            const auto type = section.flags & 0xffU;
            if (section.name == "__eh_frame") { info.eh_frame = address; info.eh_frame_size = section.size; }
            if (section.name == "__unwind_info") { info.compact_unwind = address; info.compact_unwind_size = section.size; }
            if (type == 0x11 || type == 0x12) {
                tls_begin = std::min(tls_begin, address);
                tls_end = std::max(tls_end, checked_add(address, section.size));
                if (type == 0x11) initialized_end = std::max(initialized_end, checked_add(address, section.size));
                tls.alignment = std::max(tls.alignment, 1ULL << section.alignment);
            }
            if (type == 0x13) for (std::uint64_t offset = 0; offset < section.size; offset += 24) tls.descriptors.push_back(address + offset);
            if (type == 0x15) for (std::uint64_t offset = 0; offset < section.size; offset += 8) {
                const auto function = memory.read64(address + offset);
                const auto is_executable = std::any_of(module.image.segments().begin(), module.image.segments().end(),
                    [&](const auto& candidate) {
                        if (candidate.name == "__PAGEZERO" || (candidate.initial_permissions & 4U) == 0) return false;
                        const auto begin = module.base + candidate.address - module.image.preferred_base();
                        return function >= begin && function - begin < candidate.size;
                    });
                if (!is_executable || (function & 3U) != 0) throw std::runtime_error("TLS initializer is not executable code");
                tls.initializers.push_back(function);
            }
            if (type == 0xa) for (std::uint64_t offset = 0; offset < section.size; offset += 8) {
                const auto function = memory.read64(address + offset);
                const auto target = memory.query_region(function);
                // Executable permissions are installed after this metadata pass.
                if (!target || (function & 3U) != 0) throw std::runtime_error("invalid module finalizer address");
            }
        }
        if (tls_begin != UINT64_MAX || !tls.descriptors.empty()) {
            if (tls_begin == UINT64_MAX || tls_end - tls_begin > 64 * 1024 * 1024) throw std::runtime_error("invalid or excessive TLS template");
            initialized_end = std::max(initialized_end, tls_begin);
            tls.initial_data.resize(static_cast<std::size_t>(initialized_end - tls_begin));
            for (std::size_t offset = 0; offset < tls.initial_data.size(); ++offset) tls.initial_data[offset] = memory.read8(tls_begin + offset);
            tls.zero_fill_size = tls_end - initialized_end;
            const auto bootstrap = context.resolve("/usr/lib/libSystem.B.dylib", "__tlv_bootstrap");
            if (!bootstrap && !tls.descriptors.empty()) throw std::runtime_error("TLS requires __tlv_bootstrap provider");
            for (const auto descriptor : tls.descriptors) {
                auto offset = memory.read64(descriptor + 16);
                // The Mach-O descriptor's third word is a template-relative
                // offset, not a slid pointer. Preserve it after validation.
                if (offset >= tls_end - tls_begin) throw std::runtime_error("TLS descriptor offset exceeds template");
                memory.write64(descriptor, bootstrap->address);
                memory.write64(descriptor + 8, module.id);
            }
            info.tls.push_back(std::move(tls));
        }
        return info;
    }

    void rollback(std::size_t first) noexcept {
        if (context.rollback_images) {
            try {
                std::vector<std::uint64_t> ids;
                for (std::size_t index = first; index < modules.size(); ++index) ids.push_back(modules[index]->id);
                context.rollback_images(ids);
            } catch (...) { }
        }
        while (modules.size() > first) {
            auto& module = *modules.back();
            by_source.erase(module.image.source());
            std::erase_if(by_identity, [&](const auto& entry) { return entry.second == &module; });
            for (const auto& segment : module.image.segments()) {
                if (segment.size == 0) continue;
                const auto address = segment.name == "__PAGEZERO" ? segment.address : module.base + segment.address - module.image.preferred_base();
                try { context.main_engine().unmap_memory(address); } catch (...) {}
            }
            if (main == &module) main = nullptr;
            modules.pop_back();
        }
    }

    std::vector<Module*> finish(std::size_t first) {
        std::vector<ImageRuntimeInfo> images;
        std::vector<Module*> added;
        // Startup phases (the first load only: later phases keep the first time).
        perf::phase("loader: images mapped, binding starts");
        for (std::size_t index = first; index < modules.size(); ++index) {
            auto& module = *modules[index];
            module.image.bind(context.main_engine(), module.base,
                [&](std::int32_t ordinal, std::string_view symbol_name, bool) { return binding(module, ordinal, symbol_name); });
            if (perf::enabled() && utf8(module.image.source()).find("cpython-") != std::string::npos)
                perf::phase("first Python extension module (dlopen)");
        }
        perf::phase("loader: bound");
        for (std::size_t index = first; index < modules.size(); ++index) images.push_back(metadata(*modules[index]));
        perf::phase("loader: image metadata read");
        if (context.prepare_images) context.prepare_images(images);
        perf::phase("loader: Objective-C/CF images prepared");
        for (std::size_t index = first; index < modules.size(); ++index) {
            auto& module = *modules[index];
            module.image.protect(context.main_engine(), module.base);
            // Validate constructor addresses before publishing any module.
            static_cast<void>(module.image.initializers(context.main_engine(), module.base));
            for (const auto& segment : module.image.segments()) for (const auto& section : segment.sections) {
                if ((section.flags & 0xffU) != 0xa) continue;
                const auto address = module.base + section.address - module.image.preferred_base();
                for (std::uint64_t offset = 0; offset < section.size; offset += 8) {
                    const auto target = context.main_engine().query_region(context.main_engine().read64(address + offset));
                    if (!target || !target->executable) throw std::runtime_error("module finalizer is not executable code");
                }
            }
            added.push_back(&module);
        }
        perf::phase("loader: protected and validated");
        for (auto& image : images) context.publish_image(std::move(image));
        perf::phase("loader: images published (fx modules registered)");
        return added;
    }

    void announce(std::span<Module* const> added) {
        std::vector<std::uint64_t> callbacks;
        { std::lock_guard lock(mutex); callbacks = add_callbacks; }
        for (auto* module : added) for (const auto callback : callbacks) {
            const std::array<std::uint64_t, 2> arguments{module->base, module->base - module->image.preferred_base()};
            static_cast<void>(EngineAdapter(context.calling_engine()).call(callback, arguments));
            if (context.requested_exit()) return;
        }
    }

    void initialize_module(Module& module) {
        std::vector<Module*> dependencies;
        std::vector<std::uint64_t> arguments;
        {
            std::unique_lock lock(mutex);
            while (module.initializing != std::thread::id{} && module.initializing != std::this_thread::get_id()) {
                const auto current = std::this_thread::get_id();
                auto owner = module.initializing;
                std::set<std::thread::id> visited;
                while (visited.insert(owner).second) {
                    if (owner == current) return; // Concurrent initialization of a dependency cycle.
                    const auto waiting = initialization_waits.find(owner);
                    if (waiting == initialization_waits.end()) break;
                    owner = waiting->second;
                }
                initialization_waits[current] = module.initializing;
                initialization_changed.wait(lock);
                initialization_waits.erase(current);
            }
            if (!module.initialization_failure.empty()) throw InitializationFailure(module.initialization_failure);
            if (module.initialized || module.initializing == std::this_thread::get_id() || context.requested_exit()) return;
            module.initializing = std::this_thread::get_id();
            dependencies = module.dependencies;
            arguments = startup;
        }
        try {
            for (auto* dependency : dependencies) if (dependency) initialize_module(*dependency);
            if (!context.requested_exit() && context.initialize_image) context.initialize_image(module.id);
            for (const auto function : module.image.initializers(context.calling_engine(), module.base)) {
                if (context.requested_exit()) break;
                static_cast<void>(EngineAdapter(context.calling_engine()).call(function, arguments));
            }
            if (!context.requested_exit()) {
                // Register in dependency-first initialization order, so process
                // teardown invokes the main module before its dependencies.
                for (const auto& segment : module.image.segments()) for (const auto& section : segment.sections) {
                    if ((section.flags & 0xffU) != 0xa) continue;
                    const auto begin = module.base + section.address - module.image.preferred_base();
                    for (std::uint64_t offset = 0; offset < section.size; offset += 8) {
                        context.register_finalizer(context.calling_engine().read64(begin + offset), 0, module.base);
                    }
                }
            }
            std::lock_guard lock(mutex);
            module.initialized = true;
            module.initializing = {};
            initialization_changed.notify_all();
        } catch (const std::exception& error) {
            std::lock_guard lock(mutex);
            module.initialization_failure = "guest module initialization failed for " + utf8(module.image.source()) + ": " + error.what();
            module.initializing = {};
            initialization_changed.notify_all();
            throw InitializationFailure(module.initialization_failure);
        }
    }

    Module* containing(std::uint64_t address) const {
        for (const auto& module : modules) if (address >= module->base && address - module->base < module->image.image_size()) return module.get();
        return main;
    }

    std::uint64_t string_pointer(std::string_view value) {
        const auto found = guest_strings.find(std::string(value));
        if (found != guest_strings.end()) return found->second;
        const auto address = context.allocate_guest(value.size() + 1);
        transmute::GuestMemory& memory = context.main_engine();
        for (std::size_t offset = 0; offset < value.size(); ++offset) memory.write8(address + offset, static_cast<std::uint8_t>(value[offset]));
        memory.write8(address + value.size(), 0);
        guest_strings.emplace(value, address);
        return address;
    }

    std::uint64_t error_pointer() {
        auto thread = context.current_thread();
        if (thread->dlerror.empty()) return 0;
        if (thread->dlerror_address == 0) thread->dlerror_address = context.allocate_guest(65536);
        const auto length = std::min<std::size_t>(thread->dlerror.size(), 65535);
        transmute::GuestMemory& memory = context.calling_engine();
        for (std::size_t offset = 0; offset < length; ++offset) memory.write8(thread->dlerror_address + offset, static_cast<std::uint8_t>(thread->dlerror[offset]));
        memory.write8(thread->dlerror_address + length, 0);
        thread->dlerror.clear();
        return thread->dlerror_address;
    }

    void install(ModuleLoader& facade) {
        static_cast<void>(context.install_function("_dlopen", [&facade, this](auto& state, auto& memory) {
            try {
                const auto path = state.x[0] ? guest_string(memory, state.x[0]) : std::string{};
                state.x[0] = facade.open(path, static_cast<unsigned>(state.x[1]), state.x[30]);
            } catch (const InitializationFailure&) { throw; }
            catch (const std::exception& error) { context.set_dlerror(error.what()); state.x[0] = 0; }
        }));
        static_cast<void>(context.install_function("_dlsym", [&facade, this](auto& state, auto& memory) {
            try {
                const ProcessContext::RuntimeLookup lookup;   // no probe stubs for dlsym
                const auto result = facade.symbol(state.x[0], guest_string(memory, state.x[1]), state.x[30]);
                if (result && result->kind == ExportKind::thread_local_descriptor) {
                    const std::array<std::uint64_t, 1> arguments{result->address};
                    state.x[0] = EngineAdapter(context.calling_engine()).call(memory.read64(result->address), arguments);
                } else state.x[0] = result ? result->address : 0;
            } catch (const std::exception& error) { context.set_dlerror(error.what()); state.x[0] = 0; }
        }));
        static_cast<void>(context.install_function("_dlclose", [&facade](auto& state, auto&) {
            state.x[0] = facade.close(state.x[0]) ? 0 : UINT64_MAX;
        }));
        static_cast<void>(context.install_function("_dlerror", [this](auto& state, auto&) { state.x[0] = error_pointer(); }));
        static_cast<void>(context.install_function("_dladdr", [this](auto& state, auto& memory) {
            std::lock_guard lock(mutex);
            const auto address = state.x[0], destination = state.x[1];
            Module* selected = nullptr;
            for (const auto& module : modules) if (address >= module->base && address - module->base < module->image.image_size()) selected = module.get();
            if (!selected || destination == 0) { state.x[0] = 0; return; }
            std::optional<RuntimeExport> best;
            std::string symbol_name;
            for (const auto& item : selected->image.exports()) {
                if (item.reexport_ordinal || item.absolute) continue;
                const auto value = export_value(*selected, item);
                if (value.address <= address && (!best || value.address > best->address)) {
                    best = value; symbol_name = item.name.starts_with('_') ? item.name.substr(1) : item.name;
                }
            }
            memory.write64(destination, string_pointer(guest_path_string(selected->image.source())));
            memory.write64(destination + 8, selected->base);
            memory.write64(destination + 16, best ? string_pointer(symbol_name) : 0);
            memory.write64(destination + 24, best ? best->address : 0);
            state.x[0] = 1;
        }));
        static_cast<void>(context.install_function("__dyld_image_count", [this](auto& state, auto&) {
            std::lock_guard lock(mutex); state.x[0] = modules.size();
        }));
        static_cast<void>(context.install_function("__dyld_get_image_header", [this](auto& state, auto&) {
            std::lock_guard lock(mutex); state.x[0] = state.x[0] < modules.size() ? modules[static_cast<std::size_t>(state.x[0])]->base : 0;
        }));
        static_cast<void>(context.install_function("__dyld_get_image_name", [this](auto& state, auto&) {
            std::lock_guard lock(mutex); state.x[0] = state.x[0] < modules.size() ? string_pointer(guest_path_string(modules[static_cast<std::size_t>(state.x[0])]->image.source())) : 0;
        }));
        static_cast<void>(context.install_function("__NSGetExecutablePath", [this](auto& state, auto& memory) {
            const auto buffer = state.x[0], size_address = state.x[1];
            const auto path = guest_path_string(executable);
            const auto required = static_cast<std::uint32_t>(path.size() + 1);
            const auto capacity = memory.read32(size_address);
            if (capacity < required) { memory.write32(size_address, required); state.x[0] = UINT64_MAX; return; }
            for (std::size_t offset = 0; offset < path.size(); ++offset) memory.write8(checked_add(buffer, offset), static_cast<std::uint8_t>(path[offset]));
            memory.write8(checked_add(buffer, path.size()), 0);
            state.x[0] = 0;
        }));
        // The main executable's mach_header (crt's _NSGetMachExecuteHeader).
        static_cast<void>(context.install_function("__NSGetMachExecuteHeader", [this](auto& state, auto&) {
            std::lock_guard lock(mutex); state.x[0] = main ? main->base : 0;
        }));
        // __progname: the executable's base name (crt1 sets it from argv[0]).
        {
            const auto cell = context.allocate_guest(8);
            const auto name = guest_path_string(executable);
            const auto slash = name.find_last_of('/');
            context.main_engine().write64(cell, string_pointer(slash == std::string::npos ? name : name.substr(slash + 1)));
            context.install_export("/usr/lib/libSystem.B.dylib", "___progname", {cell, ExportKind::data});
        }
        static_cast<void>(context.install_function("__dyld_get_image_vmaddr_slide", [this](auto& state, auto&) {
            std::lock_guard lock(mutex);
            state.x[0] = state.x[0] < modules.size() ? modules[static_cast<std::size_t>(state.x[0])]->base - modules[static_cast<std::size_t>(state.x[0])]->image.preferred_base() : 0;
        }));
        static_cast<void>(context.install_function("__dyld_find_unwind_sections", [this](auto& state, auto& memory) {
            const auto address = state.x[0], destination = state.x[1];
            state.x[0] = 0;
            for (const auto& image : context.images()) if (address >= image.begin && address < image.end) {
                memory.write64(destination, image.header);
                memory.write64(destination + 8, image.eh_frame);
                memory.write64(destination + 16, image.eh_frame_size);
                memory.write64(destination + 24, image.compact_unwind);
                memory.write64(destination + 32, image.compact_unwind_size);
                state.x[0] = 1; break;
            }
        }));
        static_cast<void>(context.install_function("__dyld_register_func_for_add_image", [this](auto& state, auto&) {
            const auto callback = state.x[0];
            if (callback == 0) throw std::runtime_error("null dyld image callback");
            std::vector<Module*> loaded;
            { std::lock_guard lock(mutex); add_callbacks.push_back(callback); for (const auto& module : modules) loaded.push_back(module.get()); }
            for (auto* module : loaded) {
                const std::array<std::uint64_t, 2> arguments{module->base, module->base - module->image.preferred_base()};
                static_cast<void>(EngineAdapter(context.calling_engine()).call(callback, arguments));
            }
        }));
        static_cast<void>(context.install_function("__dyld_register_func_for_remove_image", [this](auto& state, auto&) {
            if (state.x[0] == 0) throw std::runtime_error("null dyld image callback");
            std::lock_guard lock(mutex); remove_callbacks.push_back(state.x[0]);
        }));
        static_cast<void>(context.install_function("dyld_stub_binder", [](auto&, auto&) {
            throw std::runtime_error("lazy stub reached after Alchemy eager binding");
        }));
    }
};

ModuleLoader::ModuleLoader(ProcessContext& context, std::filesystem::path executable,
                           std::optional<std::uint64_t> load_base,
                           std::vector<std::filesystem::path> dylib_directories,
                           std::vector<std::filesystem::path> runtime_directories)
    : implementation_(std::make_unique<Implementation>(context, std::move(executable), load_base,
          std::move(dylib_directories), std::move(runtime_directories))) {
    implementation_->install(*this);
}

ModuleLoader::~ModuleLoader() = default;

std::uint64_t ModuleLoader::load_main(MachOImage parsed) {
    {
        std::lock_guard lock(implementation_->mutex);
        if (!implementation_->main) implementation_->preparsed_main = std::move(parsed);
    }
    return load_main();
}

std::uint64_t ModuleLoader::load_main() {
    auto& loader = *implementation_;
    std::vector<Implementation::Module*> added;
    std::uint64_t entry;
    {
        std::lock_guard lock(loader.mutex);
        if (loader.main) return loader.main->image.entry(loader.main->base);
        const auto first = loader.modules.size();
        const auto previous_base = loader.next_base, previous_id = loader.next_id;
        try {
            auto* module = loader.load(loader.executable, nullptr, true, true);
            const auto [stack_begin, stack_end] = loader.context.main_engine().stack_bounds();
            if (module->image.stack_size() > stack_end - stack_begin) {
                loader.context.main_engine().resize_stack(module->image.stack_size());
            }
            added = loader.finish(first);
            entry = module->image.entry(module->base);
        } catch (...) { loader.rollback(first); loader.next_base = previous_base; loader.next_id = previous_id; throw; }
    }
    loader.announce(added);
    return entry;
}

void ModuleLoader::initialize(std::span<const std::uint64_t> startup) {
    auto& loader = *implementation_;
    { std::lock_guard lock(loader.mutex); loader.startup.assign(startup.begin(), startup.end()); }
    if (!loader.main) throw std::runtime_error("main module has not been loaded");
    loader.initialize_module(*loader.main);
}

std::uint64_t ModuleLoader::open(std::string_view path, unsigned flags, std::uint64_t caller) {
    constexpr unsigned allowed = 1U | 2U | 4U | 8U | 0x10U | 0x80U | 0x100U;
    if ((flags & ~allowed) != 0 || (flags & 3U) == 3U || (flags & 12U) == 12U) {
        throw std::runtime_error("unsupported or conflicting dlopen flags");
    }
    auto& loader = *implementation_;
    Implementation::Module* selected = nullptr;
    std::vector<Implementation::Module*> added;
    std::string provider;
    {
        std::lock_guard lock(loader.mutex);
        if (path.empty()) selected = loader.main;
        else {
            const auto source = loader.find_path(path, loader.containing(caller), true);
            if (!source) {
                if (!loader.provider(path)) throw std::runtime_error("missing guest dylib: " + std::string(path));
                provider = path;
            } else {
                selected = loader.existing(*source);
                if (!selected && (flags & 0x10U) != 0) throw std::runtime_error("RTLD_NOLOAD module is not resident");
                if (!selected) {
                    const auto first = loader.modules.size();
                    const auto previous_base = loader.next_base, previous_id = loader.next_id;
                    try {
                        selected = loader.load(*source, loader.containing(caller), (flags & 4U) == 0);
                        added = loader.finish(first);
                    } catch (...) { loader.rollback(first); loader.next_base = previous_base; loader.next_id = previous_id; throw; }
                }
                if ((flags & 4U) == 0) {
                    std::set<std::uint64_t> seen;
                    const auto promote = [&](auto&& self, Implementation::Module& module) -> void {
                        if (!seen.insert(module.id).second) return;
                        module.global = true;
                        for (auto* dependency : module.dependencies) if (dependency) self(self, *dependency);
                    };
                    promote(promote, *selected);
                }
            }
        }
        if (!selected && provider.empty()) throw std::runtime_error("main module has not been loaded");
    }
    loader.announce(added);
    if (selected) loader.initialize_module(*selected);
    std::lock_guard lock(loader.mutex);
    for (auto& [handle, value] : loader.handles) {
        if (value.module == selected && value.provider == provider && value.first == ((flags & 0x100U) != 0) && value.default_scope == path.empty()) {
            ++value.references; return handle;
        }
    }
    const auto handle = loader.next_handle++;
    loader.handles.emplace(handle, Implementation::Handle{selected, std::move(provider), 1, (flags & 0x100U) != 0, path.empty()});
    return handle;
}

std::optional<RuntimeExport> ModuleLoader::symbol(std::uint64_t handle, std::string_view name,
                                                 std::uint64_t caller) {
    auto& loader = *implementation_;
    std::lock_guard lock(loader.mutex);
    const std::string mangled = "_" + std::string(name);
    std::optional<RuntimeExport> result;
    if (handle == UINT64_MAX - 1) result = loader.global_symbol(mangled); // RTLD_DEFAULT
    else if (handle == UINT64_MAX) { // RTLD_NEXT
        auto* source = loader.containing(caller);
        if (source && !source->image.flat_namespace()) result = loader.scope_symbol(*source, mangled, false);
        else {
            std::size_t index = 0;
            while (index < loader.modules.size() && loader.modules[index].get() != source) ++index;
            result = loader.global_symbol(mangled, false, index + 1);
        }
    } else if (handle == UINT64_MAX - 2 || handle == UINT64_MAX - 4) { // RTLD_SELF / MAIN_ONLY
        auto* module = handle == UINT64_MAX - 4 ? loader.main : loader.containing(caller);
        if (module) {
            if (handle == UINT64_MAX - 4) { if (auto found = loader.exported(*module, mangled)) result = found->value; }
            else if (!module->image.flat_namespace()) result = loader.scope_symbol(*module, mangled, true);
            else {
                result = loader.scope_symbol(*module, mangled, true, true);
                std::size_t index = 0;
                while (index < loader.modules.size() && loader.modules[index].get() != module) ++index;
                if (!result) result = loader.global_symbol(mangled, false, index + 1);
            }
        }
    } else {
        const auto found = loader.handles.find(handle);
        if (found == loader.handles.end() || found->second.references == 0) throw std::runtime_error("invalid or closed dynamic-library handle");
        if (!found->second.provider.empty()) result = loader.provider_symbol(found->second.provider, mangled);
        else if (found->second.module) {
            if (found->second.default_scope && !found->second.first) {
                result = loader.global_symbol(mangled);
                if (!result) loader.context.set_dlerror("symbol not found: " + std::string(name));
                return result;
            }
            result = loader.scope_symbol(*found->second.module, mangled, true, found->second.first);
        }
    }
    if (!result) loader.context.set_dlerror("symbol not found: " + std::string(name));
    return result;
}

bool ModuleLoader::close(std::uint64_t handle) {
    auto& loader = *implementation_;
    std::lock_guard lock(loader.mutex);
    const auto found = loader.handles.find(handle);
    if (found == loader.handles.end() || found->second.references == 0) {
        loader.context.set_dlerror("invalid or closed dynamic-library handle"); return false;
    }
    --found->second.references;
    return true;
}

} // namespace alchemy
