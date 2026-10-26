#include "alchemy/inspection.hpp"
#include "alchemy/alchemy_root.hpp"
#include "alchemy/bundle.hpp"
#include "alchemy/darwin_providers.hpp"
#include "alchemy/darwin_services.hpp"
#include "alchemy/darwin_dispatch.hpp"
#include "alchemy/darwin_mach_vm.hpp"
#include "alchemy/darwin_native.hpp"
#include "alchemy/darwin_numeric.hpp"
#include "alchemy/darwin_threads.hpp"
#include "alchemy/module_loader.hpp"
#include "alchemy/foundation_bridge.hpp"
#include "alchemy/darwin_frameworks.hpp"
#include "alchemy/path.hpp"
#include "alchemy/registration_audit.hpp"
#include "alchemy/runtime_libraries.hpp"
#ifdef ALCHEMY_WITH_GUI
#include "alchemy/gui/install.hpp"
#endif

#include <algorithm>
#include <iomanip>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>

namespace alchemy {
namespace {

std::string utf8(const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::string quote(std::string_view value) {
    std::ostringstream output;
    output << '"';
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (character == '"' || character == '\\') output << '\\' << character;
        else if (byte < 32) output << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(byte) << std::dec;
        else output << character;
    }
    output << '"';
    return output.str();
}

std::string version(std::uint32_t value) {
    return std::to_string(value >> 16) + '.' + std::to_string((value >> 8) & 255) + '.' + std::to_string(value & 255);
}

// Weak references, objc_retainBlock, @synchronized and Objective-C exceptions (thrown and
// caught through the guest libc++abi) are runtime-supported: nothing is deferred.
std::string_view deferred_object_api(std::string_view) {
    return {};
}

struct Dependency { std::string identity, status, path; bool weak{}; };
struct Import { std::string name, library, status; std::int32_t ordinal{}; bool weak{}; };
struct Item {
    std::filesystem::path path;
    std::optional<MachOImage> image;
    std::map<std::string_view, const MachOExport*, std::less<>> exports;
    std::vector<Dependency> dependencies;
    std::vector<std::optional<std::filesystem::path>> targets;
    std::vector<Import> imports;
    std::vector<std::string> requirements;
    std::vector<std::string> objc_sections;
    std::string error;
};

class Inspector {
public:
    Inspector(const std::filesystem::path& source, const RunOptions& options, ProcessContext& context,
              bool recursive) : root_(source), options_(options), context_(context), recursive_(recursive) {}

    void add(const std::filesystem::path& source, std::vector<std::filesystem::path> inherited = {}) {
        const auto path = installed_real_path(source);
        if (indices_.contains(path)) return;
        if (items_.size() >= 4096) throw std::runtime_error("inspection image limit exceeded");
        const auto index = items_.size();
        indices_.emplace(path, index);
        auto item = std::make_unique<Item>();
        item->path = path;
        items_.push_back(std::move(item));
        auto& selected = *items_[index];
        try { selected.image = MachOImage::load(path); }
        catch (const std::exception& error) { selected.error = error.what(); unsupported_ = true; return; }
        for (const auto& exported : selected.image->exports()) selected.exports.emplace(exported.name, &exported);
        for (const auto& segment : selected.image->segments()) for (const auto& section : segment.sections) {
            if (!section.size || !section.name.starts_with("__objc_")) continue;
            selected.objc_sections.push_back(section.name);
            // Protocol lists and references are canonicalized at load; Swift stub categories are not.
            if (section.name == "__objc_catlist2") {
                selected.requirements.push_back("unsupported Objective-C metadata section " + section.name);
                unsupported_ = true;
            }
        }
        // The engine allocates main and worker stacks of any size up to 1 GiB.
        if (selected.image->stack_size() > 1024ULL * 1024U * 1024U) {
            selected.requirements.push_back("Transmute guest stack of at least " + std::to_string(selected.image->stack_size()) + " bytes");
            unsupported_ = true;
        }
        std::vector<std::filesystem::path> runpaths;
        for (const auto& value : selected.image->runpaths()) {
            if (const auto expanded = expand(value, path)) runpaths.push_back(*expanded);
        }
        runpaths.insert(runpaths.end(), inherited.begin(), inherited.end());
        for (const auto& dependency : selected.image->dependencies()) {
            const auto runtime_failures = runtime_failures_.size();
            auto target = locate(dependency.install_name, path, runpaths);
            for (auto slot = runtime_failures; slot < runtime_failures_.size(); ++slot) {
                selected.requirements.push_back(runtime_failures_[slot]);
                unsupported_ = true;
            }
            Dependency report{dependency.install_name, "missing", {}, dependency.weak};
            if (target) { report.status = "guest"; report.path = utf8(*target); }
            else if (ProcessContext::is_provider_library(dependency.install_name)) report.status = "provider";
            else if (!dependency.weak) unsupported_ = true;
            selected.dependencies.push_back(std::move(report));
            selected.targets.push_back(target);
            if (target && recursive_) add(*target, runpaths);
            // libSystem re-exports libunwind: the loader loads the LLVM
            // libunwind runtime library with the first libSystem dependency
            // (module_loader.cpp load_system_unwinder), so it is inspected too.
            if (!target && recursive_ && dependency.install_name == "/usr/lib/libSystem.B.dylib")
                if (const auto unwinder = locate("/usr/lib/libunwind.dylib", path, runpaths)) add(*unwinder, runpaths);
        }
    }

    void imports() {
        for (auto& selected : items_) {
            if (!selected->image) continue;
            for (const auto& imported : selected->image->imports()) {
                Import report{imported.symbol, {}, "unresolved", imported.library_ordinal, imported.weak};
                if (imported.library_ordinal > 0 &&
                    static_cast<std::size_t>(imported.library_ordinal) <= selected->dependencies.size()) {
                    const auto slot = static_cast<std::size_t>(imported.library_ordinal - 1);
                    report.library = selected->dependencies[slot].identity;
                    if (selected->targets[slot]) {
                        if (!recursive_) report.status = "not-inspected";
                        else if (has_export(*selected->targets[slot], imported.symbol, {})) report.status = "guest";
                    } else if (resident_unwinder(report.library, imported.symbol)) report.status = "guest";
                    else if (context_.resolve(report.library, imported.symbol)) report.status = "provider";
                } else if (imported.library_ordinal == 0) {
                    report.library = "self";
                    if (has_export(selected->path, imported.symbol, {})) report.status = "guest";
                } else if (imported.library_ordinal == -1) {
                    report.library = "main-executable";
                    if (has_export(root_, imported.symbol, {})) report.status = "guest";
                } else if (imported.library_ordinal == -2 || imported.library_ordinal == -3) {
                    report.library = "flat-lookup";
                    for (const auto& candidate : items_) {
                        if (has_export(candidate->path, imported.symbol, {})) { report.status = "guest"; break; }
                    }
                    if (report.status == "unresolved") {
                        for (const auto* library : {"/usr/lib/libSystem.B.dylib", "/usr/lib/libc++.1.dylib", "/usr/lib/libc++abi.dylib",
                                "/usr/lib/libunwind.dylib", objc_library, core_foundation_library, foundation_library,
                                "/System/Library/Frameworks/Accelerate.framework/Versions/A/Accelerate"}) {
                            if (context_.resolve(library, imported.symbol)) { report.status = "provider"; break; }
                        }
                    }
                } else report.status = "invalid-ordinal";
                if (report.status == "provider") {
                    if (const auto feature = deferred_object_api(imported.symbol); !feature.empty()) {
                        report.status = "unsupported";
                        selected->requirements.push_back("unsupported " + std::string(feature));
                        unsupported_ = true;
                    }
                    const auto numeric = deferred_numeric_api(imported.symbol);
                    const auto native = imported.symbol == "_vm_remap"
                        ? std::string_view("Mach VM shared guest-memory alias mapping")
                        : deferred_native_api(imported.symbol);
                    if (const auto feature = numeric.empty() ? native : numeric; !feature.empty()) {
                        report.status = "unsupported";
                        selected->requirements.push_back("unsupported " + std::string(feature));
                        unsupported_ = true;
                    }
                }
                if (report.status == "unresolved" && imported.weak) report.status = "weak-null";
                if ((report.status == "unresolved" || report.status == "invalid-ordinal") && !imported.weak) unsupported_ = true;
                selected->imports.push_back(std::move(report));
            }
        }
    }

    int write(bool json, std::ostream& output) const {
        if (json) output << "{\"schema\":1,\"profile\":\"Alchemy headless runtime\",\"root\":" << quote(utf8(root_))
                         << ",\"supported\":" << (unsupported_ ? "false" : "true")
                         << ",\"execution_verified\":false,\"recursive\":" << (recursive_ ? "true" : "false")
                         << ",\"providers\":[{\"identity\":" << quote(objc_library)
                         << ",\"profile\":\"bounded Objective-C runtime\",\"complete\":false},{\"identity\":"
                         << quote(core_foundation_library) << ",\"profile\":\"CoreFoundation value types\",\"complete\":false},{\"identity\":"
                         << quote(foundation_library) << ",\"profile\":\"Foundation value bridge\",\"complete\":false},{\"identity\":"
                         << quote("/System/Library/Frameworks/Accelerate.framework/Versions/A/Accelerate")
                         << ",\"profile\":\"bounded LP64 BLAS/LU/Cholesky/least-squares\",\"complete\":false}],\"images\":[";
        else output << "Alchemy profile: independent ARM64 headless runtime\n"
                    << "Object providers: bounded Objective-C, CoreFoundation values, Foundation value bridge\n"
                    << "Objective-C metadata is validated during loading; providers are partial.\nRoot: " << utf8(root_) << '\n';
        for (std::size_t index = 0; index < items_.size(); ++index) {
            const auto& item = *items_[index];
            if (json) {
                if (index) output << ',';
                output << "{\"path\":" << quote(utf8(item.path)) << ",\"architecture\":" << quote(item.image ? "arm64" : "unverified") << ",\"error\":" << quote(item.error);
            } else output << "\nImage: " << utf8(item.path) << '\n';
            if (!item.image) {
                if (json) output << '}';
                else output << "  Unsupported image: " << item.error << '\n';
                continue;
            }
            const auto& image = *item.image;
            const auto type = image.is_dylib() ? "dylib" : image.is_bundle() ? "bundle" : "executable";
            const auto base = options_.load_base.value_or(image.preferred_base());
            if (json) {
                output << ",\"type\":" << quote(type) << ",\"minimum_os\":" << quote(version(image.minimum_os()))
                       << ",\"sdk\":" << quote(version(image.sdk_version())) << ",\"preferred_base\":" << image.preferred_base()
                       << ",\"stack_size\":" << image.stack_size() << ",\"requirements\":[";
                for (std::size_t slot = 0; slot < item.requirements.size(); ++slot) {
                    if (slot) output << ',';
                    output << quote(item.requirements[slot]);
                }
                output << "],\"objc_metadata_verified\":false,\"objc_sections\":[";
                for (std::size_t slot = 0; slot < item.objc_sections.size(); ++slot) {
                    if (slot) output << ',';
                    output << quote(item.objc_sections[slot]);
                }
                output << ']';
                if (!image.is_dylib() && !image.is_bundle()) output << ",\"entry\":" << image.entry(base);
                output << ",\"classic_fixups\":" << (image.has_classic_fixups() ? "true" : "false") << ",\"pointer_formats\":[";
                for (std::size_t slot = 0; slot < image.pointer_formats().size(); ++slot) {
                    if (slot) output << ',';
                    output << image.pointer_formats()[slot];
                }
                output << "],\"runpaths\":[";
                for (std::size_t slot = 0; slot < image.runpaths().size(); ++slot) {
                    if (slot) output << ',';
                    output << quote(image.runpaths()[slot]);
                }
                output << "],\"dependencies\":[";
            } else {
                output << "  Architecture: ARM64, type: " << type << "\n  Minimum OS metadata: " << version(image.minimum_os())
                       << ", SDK: " << version(image.sdk_version()) << "\n  Dependencies:\n";
                for (const auto& requirement : item.requirements) output << "    Required: " << requirement << '\n';
            }
            for (std::size_t slot = 0; slot < item.dependencies.size(); ++slot) {
                const auto& dependency = item.dependencies[slot];
                if (json) {
                    if (slot) output << ',';
                    output << "{\"identity\":" << quote(dependency.identity) << ",\"status\":" << quote(dependency.status)
                           << ",\"path\":" << quote(dependency.path) << ",\"weak\":" << (dependency.weak ? "true" : "false") << '}';
                } else output << "    " << dependency.identity << " [" << dependency.status << (dependency.weak ? ", weak" : "") << "]\n";
            }
            if (json) output << "],\"imports\":[";
            else output << "  Imports:\n";
            for (std::size_t slot = 0; slot < item.imports.size(); ++slot) {
                const auto& imported = item.imports[slot];
                if (json) {
                    if (slot) output << ',';
                    output << "{\"symbol\":" << quote(imported.name) << ",\"library\":" << quote(imported.library)
                           << ",\"ordinal\":" << imported.ordinal << ",\"weak\":" << (imported.weak ? "true" : "false")
                           << ",\"status\":" << quote(imported.status) << '}';
                } else output << "    " << imported.name << " [" << imported.library << ", " << imported.status << "]\n";
            }
            if (json) output << "]}";
        }
        if (json) output << "]}\n";
        else output << "\nVersion metadata is not a full macOS compatibility claim.\n";
        return unsupported_ ? 1 : 0;
    }

private:
    std::optional<std::filesystem::path> expand(std::string_view value, const std::filesystem::path& loader) const {
        if (value == "@loader_path") return loader.parent_path();
        if (value == "@executable_path") return root_.parent_path();
        if (value.starts_with("@loader_path/")) return loader.parent_path() / utf8_path(value.substr(13));
        if (value.starts_with("@executable_path/")) return root_.parent_path() / utf8_path(value.substr(17));
        if (value.starts_with('@') || value.starts_with('/')) return {};
        return loader.parent_path() / utf8_path(value);
    }
    std::optional<std::filesystem::path> locate(std::string_view name, const std::filesystem::path& loader,
        const std::vector<std::filesystem::path>& runpaths) const {
        std::vector<std::filesystem::path> candidates;
        if (name.starts_with("@rpath/")) {
            for (const auto& directory : runpaths) candidates.push_back(directory / utf8_path(name.substr(7)));
        } else if (const auto value = expand(name, loader)) candidates.push_back(*value);
        const auto filename = utf8_path(name).filename();
        if (const auto* identity = runtime_library_identity(name)) {
            // The loader accepts an allow-listed runtime identity only with
            // matching provenance; report the same decision.
            auto found = find_runtime_library(*identity, options_.runtime_directories);
            if (found.empty()) return {};
            try { verify_runtime_library(*identity, found, options_.runtime_directories); }
            catch (const std::exception& error) { runtime_failures_.push_back(error.what()); return {}; }
            return found;
        } else if (!ProcessContext::is_provider_library(name) && !name.starts_with("/System/")) {
            for (const auto& directory : options_.dylib_directories) candidates.push_back(directory / filename);
        }
        for (const auto& candidate : candidates) {
            std::error_code error;
            if (std::filesystem::is_regular_file(candidate, error)) return installed_real_path(candidate);
        }
        return {};
    }
    // Mirrors the loader (module_loader.cpp provider_symbol): libSystem
    // re-exports libunwind, so libSystem imports bind to an inspected
    // resident LLVM libunwind (from --runtime-dir) before the provider.
    bool resident_unwinder(std::string_view library, std::string_view name) const {
        if (library != "/usr/lib/libSystem.B.dylib" && !library.starts_with("/usr/lib/system/")) return false;
        for (const auto& candidate : items_) {
            if (candidate->image && candidate->image->install_name() == "/usr/lib/libunwind.dylib")
                return has_export(candidate->path, name, {});
        }
        return false;
    }
    bool has_export(const std::filesystem::path& path, std::string_view name, std::set<std::filesystem::path> visited) const {
        if (!visited.insert(path).second) return false;
        const auto found = indices_.find(path);
        if (found == indices_.end()) return false;
        const auto& item = *items_[found->second];
        if (!item.image) return false;
        if (const auto record = item.exports.find(name); record != item.exports.end()) {
            const auto& exported = *record->second;
            if (!exported.reexport_ordinal) return true;
            const auto ordinal = *exported.reexport_ordinal;
            if (ordinal == 0 || ordinal > item.targets.size() || !item.targets[ordinal - 1]) return false;
            const auto target = exported.imported_name.empty() ? name : std::string_view(exported.imported_name);
            return has_export(*item.targets[ordinal - 1], target, std::move(visited));
        }
        for (std::size_t slot = 0; slot < item.targets.size(); ++slot) {
            if (item.image->dependencies()[slot].reexport && item.targets[slot] && has_export(*item.targets[slot], name, visited)) return true;
        }
        return false;
    }
    std::filesystem::path root_;
    const RunOptions& options_;
    ProcessContext& context_;
    bool recursive_{}, unsupported_{};
    mutable std::vector<std::string> runtime_failures_;
    std::map<std::filesystem::path, std::size_t> indices_;
    std::vector<std::unique_ptr<Item>> items_;
};

} // namespace

int inspect_program(const std::filesystem::path& path, const RunOptions& options,
    bool recursive, bool scan_bundle, bool json, std::ostream& output) {
    const auto source = discover_program(path);
    transmute::ReferenceEngine engine(transmute::ExecutionStart{0});
    DarwinRuntime runtime(engine);
    ProcessContext context(engine, runtime.output_writer());
    context.executable = source;
    context.base_resolver = [&](std::string_view library, std::string_view symbol) { return runtime.resolve(library, symbol); };
    // Doctor reports the same provider exports a run binds.
    DarwinProviders providers(context);
    ModuleLoader loader(context, source, options.load_base, options.dylib_directories, options.runtime_directories);
    Inspector inspector(source, options, context, recursive || scan_bundle);
    inspector.add(source);
    if (scan_bundle) for (const auto& image : bundle_macho_files(path)) inspector.add(image);
    inspector.imports();
    return inspector.write(json, output);
}

} // namespace alchemy
