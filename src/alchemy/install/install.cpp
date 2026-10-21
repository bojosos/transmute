// `alchemy install`: places an extracted macOS .app into the Alchemy root and
// recreates its symbolic links (see include/alchemy/alchemy_root.hpp).
// Project-authored, MIT.

#include "alchemy/alchemy_root.hpp"
#include "alchemy/path.hpp"
#include "alchemy/install/install_internal.hpp"
#include "alchemy/host/files.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <system_error>


namespace alchemy {

using namespace install_detail;

namespace {

std::filesystem::path child(const std::filesystem::path& base, std::string_view relative) {
    return relative.empty() ? base : (base / utf8_path(relative)).make_preferred();
}

bool reparse_point(const std::filesystem::path& path) {
    const auto attributes = host::files::attributes(path_utf8(path));
    return attributes && attributes->symlink;   // a symbolic link, junction or other reparse point
}

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error("alchemy install: " + message); }

std::string error_text(host::Error error) { return std::string(host::name(error)); }

// An NTFS mount-point reparse point; creating one needs no privilege.
void create_junction(const std::filesystem::path& link, const std::filesystem::path& target) {
    const auto status = host::files::create_junction(path_utf8(link), path_utf8(std::filesystem::absolute(target).lexically_normal()));
    if (status) return;
    if (status.error() == host::Error::invalid_argument) fail("junction target must be a drive path: " + generic_text(target));
    fail("cannot create junction " + generic_text(link) + ": " + error_text(status.error()));
}

// Removes a tree without following any link or junction inside it.
void remove_tree(const std::filesystem::path& path) {
    if (const auto status = host::files::remove_tree(path_utf8(path)); !status)
        fail("cannot remove " + generic_text(path) + ": " + error_text(status.error()));
}

bool native_symlink(const std::filesystem::path& link, std::string_view target, bool directory) {
    return static_cast<bool>(host::files::create_symbolic_link(path_utf8(link), target, directory));
}

std::string native_link_text(const std::filesystem::path& path) {
    std::error_code error;
    const auto target = std::filesystem::read_symlink(path, error);
    if (error) fail("unsupported reparse point in the source bundle: " + generic_text(path));
    if (target.is_absolute() || target.has_root_name() || target.has_root_directory())
        fail("absolute symbolic link targets are unsupported: " + generic_text(path) + " -> " + generic_text(target));
    return generic_text(target);
}

struct Walk {
    const std::filesystem::path& base;
    const LinkMap& links;
    void (*visit)(void*, const std::string&, const std::filesystem::directory_entry&, bool link);
    void* context;

    void run(const std::filesystem::path& directory, const std::string& relative) const {
        std::vector<std::filesystem::directory_entry> entries;
        for (const auto& entry : std::filesystem::directory_iterator(directory)) entries.push_back(entry);
        std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) { return left.path() < right.path(); });
        for (const auto& entry : entries) {
            const auto name = generic_text(entry.path().filename());
            const auto path = relative.empty() ? name : relative + "/" + name;
            const bool link = links.contains(link_key(path)) || reparse_point(entry.path());
            visit(context, path, entry, link);
            if (!link && entry.is_directory()) run(entry.path(), path);
        }
    }
};

void add_link(LinkMap& links, InstalledLink link, const char* origin) {
    if (link.path.empty() || link.target.empty()) fail(std::string("empty link record from ") + origin);
    const auto normalized = normalize_relative(link.path);
    if (!normalized || normalized->empty()) fail("link path leaves the bundle: " + link.path);
    link.path = *normalized;
    if (link.target.front() == '/')
        fail("absolute symbolic link targets are unsupported: " + link.path + " -> " + link.target);
    const auto key = link_key(link.path);
    if (const auto found = links.find(key); found != links.end()) {
        if (found->second.target != link.target)
            fail("conflicting link records for " + link.path + ": " + found->second.target + " and " + link.target);
        return;
    }
    links.emplace(key, std::move(link));
}

// 7-Zip and other unprivileged extractors leave each framework link as an
// empty regular file. The conventional framework layout identifies them.
void infer_framework_links(const std::filesystem::path& source, LinkMap& links, std::vector<std::string>& inferred) {
    struct Context { const std::filesystem::path* source; LinkMap* links; std::vector<std::string> frameworks; } context{&source, &links, {}};
    Walk walk{source, links, [](void* raw, const std::string& path, const std::filesystem::directory_entry& entry, bool link) {
        auto& self = *static_cast<Context*>(raw);
        if (!link && entry.is_directory() && entry.path().extension() == ".framework") self.frameworks.push_back(path);
    }, &context};
    walk.run(source, {});
    const auto placeholder = [](const std::filesystem::path& path) {
        std::error_code error;
        return std::filesystem::is_regular_file(std::filesystem::symlink_status(path, error)) && !reparse_point(path) &&
               std::filesystem::file_size(path, error) == 0 && !error;
    };
    for (const auto& framework : context.frameworks) {
        const auto host = child(source, framework);
        const auto versions = host / "Versions";
        std::error_code error;
        if (!std::filesystem::is_directory(versions, error)) continue;
        const auto current_path = framework + "/Versions/Current";
        if (!links.contains(link_key(current_path)) && placeholder(versions / "Current")) {
            std::vector<std::string> candidates;
            for (const auto& entry : std::filesystem::directory_iterator(versions)) {
                const auto name = generic_text(entry.path().filename());
                if (name != "Current" && entry.is_directory() && !reparse_point(entry.path())) candidates.push_back(name);
            }
            if (candidates.size() != 1)
                fail("cannot infer " + current_path + ": " + std::to_string(candidates.size()) +
                     " version directories; supply the disk image's link list with --links");
            add_link(links, {current_path, candidates.front(), true, {}}, "inference");
            inferred.push_back(current_path);
        }
        const auto current = resolve_relative(current_path, links, true);
        if (!current || !std::filesystem::is_directory(child(source, *current), error)) continue;
        for (const auto& entry : std::filesystem::directory_iterator(host)) {
            const auto name = generic_text(entry.path().filename());
            const auto path = framework + "/" + name;
            if (name == "Versions" || links.contains(link_key(path)) || !placeholder(entry.path())) continue;
            if (!std::filesystem::exists(child(source, *current + "/" + name), error)) continue;
            add_link(links, {path, "Versions/Current/" + name, false, {}}, "inference");
            inferred.push_back(path);
        }
    }
}

void write_manifest(const std::filesystem::path& file, const std::string& prefix, const std::vector<InstalledLink>& links) {
    const auto temporary = std::filesystem::path(file).concat(".tmp");
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << "alchemy-links\t1\n";
        for (const auto& link : links) {
            output << "link\t" << (link.directory ? "dir" : "file") << '\t' << link.representation << '\t'
                   << prefix << link.path << '\t' << link.target << '\n';
        }
        if (!output) fail("cannot write link manifest " + generic_text(temporary));
    }
    std::filesystem::rename(temporary, file);
}

void validate_text(const std::string& value) {
    if (value.find_first_of(std::string_view("\t\n\r\0", 4)) != std::string::npos)
        fail("link paths and targets cannot contain tab, newline or NUL characters");
}

} // namespace

void install_detail::make_junction(const std::filesystem::path& link, const std::filesystem::path& target) {
    create_junction(link, target);
}

InstallResult install_app(const InstallOptions& options, std::ostream& log) {
    std::error_code error;
    if (!std::filesystem::is_directory(options.source, error)) fail("source is not a directory: " + generic_text(options.source));
    const auto source = std::filesystem::canonical(options.source);
    const auto name = generic_text(source.filename());
    if (link_key(source.extension().generic_string()) != ".app") fail("source must be an .app bundle directory: " + generic_text(source));
    if (!std::filesystem::is_regular_file(source / "Contents" / "Info.plist", error) ||
        std::filesystem::file_size(source / "Contents" / "Info.plist", error) == 0)
        fail("bundle has no Contents/Info.plist: " + generic_text(source));

    // 1. Link set: explicit list, native links in the source, then inference.
    LinkMap links;
    if (options.links) for (auto& link : read_link_list(*options.links)) add_link(links, std::move(link), "the link list");
    {
        struct Context { LinkMap* links; } context{&links};
        LinkMap none;
        Walk walk{source, none, [](void* raw, const std::string& path, const std::filesystem::directory_entry& entry, bool link) {
            if (link) add_link(*static_cast<Context*>(raw)->links, {path, native_link_text(entry.path()), false, {}}, "the source tree");
        }, &context};
        walk.run(source, {});
    }
    InstallResult result;
    if (options.infer_framework_links) infer_framework_links(source, links, result.inferred);

    // 2. Every link must resolve to a real entry inside the bundle.
    std::vector<std::pair<InstalledLink, std::string>> planned;
    for (const auto& [key, record] : links) {
        validate_text(record.path);
        validate_text(record.target);
        const auto resolved = resolve_relative(record.path, links, true);
        if (!resolved) fail("link leaves the bundle or loops: " + record.path + " -> " + record.target);
        const auto host = child(source, *resolved);
        if (!std::filesystem::exists(host, error) || reparse_point(host))
            fail("dangling link: " + record.path + " -> " + record.target);
        auto link = record;
        link.directory = std::filesystem::is_directory(host, error);
        planned.emplace_back(std::move(link), *resolved);
    }

    // 3. Destination.
    const auto root = create_alchemy_root(options.root);
    const auto destination = root / "Applications" / source.filename();
    const auto manifest = root / ".alchemy" / "links" / std::filesystem::path(source.filename()).concat(".tsv");
    if (std::filesystem::exists(std::filesystem::symlink_status(destination, error))) {
        if (!options.replace) fail(generic_text(destination) + " is already installed; pass --replace");
        log << "Removing previous installation " << generic_text(destination) << '\n';
        remove_tree(destination);
        std::filesystem::remove(manifest, error);
    }
    result.destination = destination;
    result.manifest = manifest;

    try {
        // 4. Copy everything except the links.
        std::filesystem::create_directory(destination);
        struct Context { const std::filesystem::path* source; const std::filesystem::path* destination; InstallResult* result; } context{&source, &destination, &result};
        Walk walk{source, links, [](void* raw, const std::string& path, const std::filesystem::directory_entry& entry, bool link) {
            auto& self = *static_cast<Context*>(raw);
            if (link) return;
            const auto target = child(*self.destination, path);
            if (entry.is_directory()) {
                std::filesystem::create_directory(target);
                ++self.result->directories;
            } else if (entry.is_regular_file()) {
                std::filesystem::copy_file(entry.path(), target, std::filesystem::copy_options::overwrite_existing);
                ++self.result->files;
                self.result->bytes += static_cast<std::size_t>(entry.file_size());
            } else fail("unsupported file type in the source bundle: " + path);
        }, &context};
        walk.run(source, {});

        // 5. Links: native symlink when allowed, else junction / hard link / copy.
        bool symlinks = options.allow_native_symlinks;
        for (auto& [link, resolved] : planned) {
            const auto parent = resolve_relative(parent_of(link.path), links, true);
            const auto host = child(destination, (parent && !parent->empty() ? *parent + "/" : std::string()) +
                                                 generic_text(utf8_path(link.path).filename()));
            const auto target = child(destination, resolved);
            if (symlinks && native_symlink(host, link.target, link.directory)) link.representation = "symlink";
            else {
                symlinks = false; // the host refused once; it will refuse again
                if (link.directory) {
                    create_junction(host, target);
                    link.representation = "junction";
                } else if (host::files::create_hard_link(path_utf8(host), path_utf8(target))) link.representation = "hardlink";
                else {
                    std::filesystem::copy_file(target, host);
                    link.representation = "copy";
                }
            }
            result.links.push_back(link);
        }

        // 6. Verify each link through the host filesystem.
        for (const auto& [link, resolved] : planned) {
            const auto host = child(destination, link.path);
            const auto target = child(destination, resolved);
            if (link.directory ? !std::filesystem::is_directory(host, error)
                               : (!std::filesystem::is_regular_file(host, error) ||
                                  std::filesystem::file_size(host, error) != std::filesystem::file_size(target, error)))
                fail("link verification failed: " + link.path + " -> " + link.target);
        }
        std::sort(result.links.begin(), result.links.end(), [](const auto& left, const auto& right) { return left.path < right.path; });
        write_manifest(manifest, "Applications/" + name + "/", result.links);
    } catch (...) {
        try { remove_tree(destination); } catch (...) {}
        throw;
    }

    log << "Installed " << name << " -> " << generic_text(destination) << '\n'
        << "  " << result.files << " files, " << result.directories << " directories, " << result.bytes << " bytes\n"
        << "  " << result.links.size() << " links";
    if (!result.links.empty()) log << " (" << result.links.front().representation << (
        std::any_of(result.links.begin(), result.links.end(), [&](const auto& link) { return link.representation != result.links.front().representation; })
        ? ", mixed)" : ")");
    log << ", " << result.inferred.size() << " inferred from framework layout\n";
    for (const auto& link : result.links) {
        const bool guessed = std::find(result.inferred.begin(), result.inferred.end(), link.path) != result.inferred.end();
        log << "    " << link.path << " -> " << link.target << " [" << (link.directory ? "dir" : "file") << ", "
            << link.representation << (guessed ? ", inferred" : "") << "]\n";
    }
    log << "  manifest " << generic_text(manifest) << '\n';
    return result;
}

} // namespace alchemy
