#include "transmute/guest_process.hpp"

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace transmute {
namespace {

constexpr std::uint64_t import_bridge_base = 0x0000600000000000ULL;
constexpr std::uint64_t import_bridge_stride = 0x100000ULL;

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open guest module: " + path.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

PeImage parse_file(const std::filesystem::path& path,
                   std::optional<std::uint64_t> load_base = std::nullopt) {
    try {
        auto image = PeImage::parse(read_file(path), load_base);
        image.set_source(path.string());
        return image;
    } catch (const std::exception& error) {
        throw std::runtime_error(path.string() + ": " + error.what());
    }
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool is_host_bridge(const std::string& dll) {
    if (dll == "kernel32.dll" || dll == "kernelbase.dll" ||
        dll.starts_with("api-ms-win-") || dll.starts_with("ext-ms-")) {
        return true;
    }
#ifdef _WIN32
    static const auto system_directory = [] {
        std::wstring path(MAX_PATH, L'\0');
        const auto length = GetSystemDirectoryW(path.data(), static_cast<UINT>(path.size()));
        if (length == 0 || length >= path.size()) {
            throw std::runtime_error("cannot locate Windows system directory");
        }
        path.resize(length);
        return std::filesystem::path(path);
    }();
    return std::filesystem::is_regular_file(system_directory / dll);
#else
    return false;
#endif
}

bool overlaps(const PeImage& left, const PeImage& right) {
    return left.base() < right.base() + right.size() &&
           right.base() < left.base() + left.size();
}

} // namespace

GuestProcess::GuestProcess(const std::filesystem::path& executable,
                           std::optional<std::uint64_t> load_base,
                           std::vector<std::filesystem::path> dll_directories) {
    const auto full_path = std::filesystem::absolute(executable);
    const auto directory = full_path.parent_path();
    std::vector<std::filesystem::path> search_directories{directory};
    for (auto& extra : dll_directories) {
        auto resolved = extra.is_absolute() ? extra : directory / extra;
        if (!std::filesystem::is_directory(resolved)) {
            throw std::runtime_error("DLL search directory does not exist: " +
                                     resolved.string());
        }
        search_directories.push_back(std::move(resolved));
    }
    auto root = std::make_unique<PeImage>(parse_file(full_path, load_base));
    if (root->is_dll()) throw std::runtime_error("cannot launch a DLL as an executable");
    modules_.push_back(std::move(root));
    std::unordered_map<std::string, std::size_t> loaded;
    std::vector<std::vector<std::size_t>> dependencies(1);

    const auto load_guest_dll = [&](const std::string& requested)
        -> std::optional<std::size_t> {
        const auto dll = lowercase(requested);
        if (dll.empty() || dll.find_first_of("\\/:") != std::string::npos ||
            dll.size() < 5 || dll.substr(dll.size() - 4) != ".dll") {
            throw std::runtime_error("unsupported guest DLL name: " + requested);
        }
        if (const auto found = loaded.find(dll); found != loaded.end()) {
            return found->second;
        }
        std::filesystem::path path;
        for (const auto& search_directory : search_directories) {
            const auto candidate = search_directory / requested;
            if (std::filesystem::is_regular_file(candidate)) {
                path = candidate;
                break;
            }
        }
        if (path.empty() && is_host_bridge(dll)) return std::nullopt;
        if (path.empty()) {
            throw std::runtime_error("guest DLL was not found in search directories: " +
                                     requested);
        }
        if (modules_.size() >= 128) throw std::runtime_error("too many guest modules");
        auto candidate = std::make_unique<PeImage>(parse_file(path));
        if (!candidate->is_dll()) {
            throw std::runtime_error("guest dependency is not a DLL: " + requested);
        }
        bool collision = false;
        for (const auto& loaded_module : modules_) {
            collision |= overlaps(*candidate, *loaded_module);
        }
        if (collision) {
            bool placed = false;
            for (std::uint64_t address = 0x180000000ULL;
                 address < 0x200000000ULL; address += 0x10000ULL) {
                bool free = true;
                for (const auto& loaded_module : modules_) {
                    free &= address + candidate->size() <= loaded_module->base() ||
                            loaded_module->base() + loaded_module->size() <= address;
                }
                if (free) {
                    candidate = std::make_unique<PeImage>(parse_file(path, address));
                    placed = true;
                    break;
                }
            }
            if (!placed) throw std::runtime_error("no free guest DLL address range");
        }
        candidate->set_import_bridge_base(import_bridge_base +
                                           modules_.size() * import_bridge_stride);
        const auto index = modules_.size();
        loaded.emplace(dll, index);
        modules_.push_back(std::move(candidate));
        dependencies.emplace_back();
        return index;
    };

    const auto resolve_guest_export = [&](std::size_t initial_module,
                                          const std::string& initial_symbol) {
        std::unordered_set<std::string> seen;
        auto module = initial_module;
        auto symbol = initial_symbol;
        for (std::size_t depth = 0; depth < 32; ++depth) {
            if (!seen.insert(std::to_string(module) + "!" + symbol).second) {
                throw std::runtime_error("cyclic PE export forwarder");
            }
            const auto target = modules_[module]->find_export_target(symbol);
            if (!target) {
                throw std::runtime_error("guest DLL does not export " +
                    std::filesystem::path(modules_[module]->source()).filename().string() +
                    "!" + symbol);
            }
            if (target->address) return *target->address;
            const auto separator = target->forwarder.rfind('.');
            auto dll = target->forwarder.substr(0, separator);
            symbol = target->forwarder.substr(separator + 1);
            if (!lowercase(dll).ends_with(".dll")) dll += ".dll";
            const auto next = load_guest_dll(dll);
            if (!next) {
                throw std::runtime_error("PE export forwarder targets host DLL: " +
                                         target->forwarder);
            }
            dependencies[module].push_back(*next);
            module = *next;
        }
        throw std::runtime_error("PE export forwarder exceeds depth limit");
    };

    for (std::size_t module_index = 0; module_index < modules_.size(); ++module_index) {
        auto& current = *modules_[module_index];
        for (std::size_t import_index = 0; import_index < current.imports().size();
             ++import_index) {
            const auto imported = current.imports()[import_index];
            const auto found = load_guest_dll(imported.dll);
            if (!found) continue;
            const auto target = resolve_guest_export(*found, imported.function);
            current.bind_import(import_index, target);
            dependencies[module_index].push_back(*found);
        }
    }

    std::vector<std::uint8_t> visited(modules_.size(), 0);
    const std::function<void(std::size_t)> visit = [&](std::size_t index) {
        if (visited[index] == 2) return;
        if (visited[index] == 1) {
            throw std::runtime_error("cyclic guest DLL initialization is not yet supported");
        }
        visited[index] = 1;
        for (const auto dependency : dependencies[index]) visit(dependency);
        visited[index] = 2;
        if (index != 0) attachment_order_.push_back(modules_[index].get());
    };
    for (std::size_t index = 1; index < modules_.size(); ++index) visit(index);
}

GuestProcess::DynamicLoadPlan GuestProcess::prepare_dynamic_load(
    const std::filesystem::path& requested,
    const std::vector<std::filesystem::path>& search_directories,
    bool include_requested_directory) const {
    DynamicLoadPlan plan;
    std::unordered_map<std::string, PeImage*> known;
    for (const auto& module : modules_) {
        known.emplace(lowercase(std::filesystem::path(module->source())
                                    .filename().string()), module.get());
    }
    std::vector<std::filesystem::path> directories = search_directories;
    if (include_requested_directory) {
        directories.insert(directories.begin(), requested.parent_path());
    }
    const auto load_file = [&](const std::filesystem::path& path) -> PeImage* {
        const auto name = lowercase(path.filename().string());
        if (const auto found = known.find(name); found != known.end()) {
            return found->second;
        }
        if (modules_.size() + plan.modules.size() >= 128) {
            throw std::runtime_error("too many guest modules");
        }
        auto candidate = std::make_unique<PeImage>(parse_file(path));
        if (!candidate->is_dll()) {
            throw std::runtime_error("runtime guest module is not a DLL: " +
                                     path.string());
        }
        const auto collides = [&](const PeImage& image) {
            const auto overlaps_any = [&](const auto& modules) {
                return std::any_of(modules.begin(), modules.end(),
                    [&](const auto& loaded) { return overlaps(image, *loaded); });
            };
            return overlaps_any(modules_) || overlaps_any(plan.modules);
        };
        if (collides(*candidate)) {
            bool placed = false;
            for (std::uint64_t address = 0x180000000ULL;
                 address < 0x200000000ULL; address += 0x10000ULL) {
                if (candidate->size() > 0x200000000ULL - address) break;
                const auto range_free = [&](const auto& modules) {
                    return std::all_of(modules.begin(), modules.end(),
                        [&](const auto& loaded) {
                            return address + candidate->size() <= loaded->base() ||
                                   loaded->base() + loaded->size() <= address;
                        });
                };
                if (!range_free(modules_) || !range_free(plan.modules)) continue;
                candidate = std::make_unique<PeImage>(parse_file(path, address));
                placed = true;
                break;
            }
            if (!placed) throw std::runtime_error("no free guest DLL address range");
        }
        candidate->set_import_bridge_base(import_bridge_base +
            (modules_.size() + plan.modules.size()) * import_bridge_stride);
        auto* result = candidate.get();
        plan.modules.push_back(std::move(candidate));
        known.emplace(name, result);
        return result;
    };
    const auto require_module = [&](const std::string& requested_name)
        -> PeImage* {
        auto name = lowercase(requested_name);
        if (!name.ends_with(".dll")) name += ".dll";
        if (name.find_first_of("\\/:") != std::string::npos) {
            throw std::runtime_error("unsupported guest DLL dependency name: " +
                                     requested_name);
        }
        if (const auto found = known.find(name); found != known.end()) {
            return found->second;
        }
        for (const auto& directory : directories) {
            const auto candidate = directory / name;
            if (std::filesystem::is_regular_file(candidate)) {
                return load_file(candidate);
            }
        }
        if (is_host_bridge(name)) return nullptr;
        throw std::runtime_error("guest DLL dependency was not found: " +
                                 requested_name);
    };
    std::unordered_map<PeImage*, std::uint8_t> visit_state;
    std::function<void(PeImage*)> visit;
    std::function<std::uint64_t(PeImage*, std::string, unsigned)> resolve_export;
    resolve_export = [&](PeImage* module, std::string symbol,
                         unsigned depth) -> std::uint64_t {
        if (depth >= 32) throw std::runtime_error("PE export forwarder exceeds depth limit");
        visit(module);
        const auto target = module->find_export_target(symbol);
        if (!target) {
            throw std::runtime_error("guest DLL does not export " +
                std::filesystem::path(module->source()).filename().string() +
                "!" + symbol);
        }
        if (target->address) return *target->address;
        const auto separator = target->forwarder.rfind('.');
        if (separator == std::string::npos) {
            throw std::runtime_error("invalid PE export forwarder");
        }
        auto* forwarded = require_module(target->forwarder.substr(0, separator));
        if (!forwarded) {
            throw std::runtime_error("PE export forwarder targets a host DLL");
        }
        return resolve_export(forwarded,
                              target->forwarder.substr(separator + 1), depth + 1);
    };
    visit = [&](PeImage* module) {
        const auto staged = std::any_of(plan.modules.begin(), plan.modules.end(),
            [&](const auto& candidate) { return candidate.get() == module; });
        if (!staged) return;
        auto& status = visit_state[module];
        if (status == 2) return;
        if (status == 1) {
            throw std::runtime_error("cyclic runtime guest DLL dependency");
        }
        status = 1;
        for (std::size_t index = 0; index < module->imports().size(); ++index) {
            const auto imported = module->imports()[index];
            auto* dependency = require_module(imported.dll);
            if (!dependency) continue;
            const auto target = resolve_export(dependency, imported.function, 0);
            module->bind_import(index, target);
        }
        status = 2;
        plan.attachment_order.push_back(module);
    };
    plan.requested = load_file(std::filesystem::absolute(requested));
    visit(plan.requested);
    return plan;
}

void GuestProcess::reserve_dynamic_modules(std::size_t count) {
    modules_.reserve(modules_.size() + count);
}

void GuestProcess::adopt_dynamic_load(DynamicLoadPlan&& plan) {
    if (modules_.capacity() - modules_.size() < plan.modules.size()) {
        throw std::runtime_error("runtime guest module adoption was not reserved");
    }
    for (auto& module : plan.modules) modules_.push_back(std::move(module));
}

} // namespace transmute
