// Alchemy root configuration and the project-owned link table (see
// include/alchemy/alchemy_root.hpp). Project-authored, MIT.

#include "alchemy/alchemy_root.hpp"
#include "alchemy/host/process.hpp"
#include "alchemy/path.hpp"
#include "alchemy/install/install_internal.hpp"

#include <cstdlib>
#include <cstring>

#include <fstream>
#include <map>
#include <stdexcept>
#include <system_error>

namespace alchemy {
namespace install_detail {

std::string generic_text(const std::filesystem::path& path) {
    const auto bytes = path.generic_u8string();
    std::string value(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    while (value.size() > 1 && value.back() == '/' && !(value.size() == 3 && value[1] == ':')) value.pop_back();
    return value;
}

std::string link_key(std::string_view value) {
    std::string result(value);
#ifdef _WIN32
    for (auto& character : result) if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
#endif
    return result;
}

std::vector<std::string> split(std::string_view value) {
    std::vector<std::string> parts;
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const auto end = value.find('/', begin);
        const auto part = value.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin);
        if (!part.empty()) parts.emplace_back(part);
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    return parts;
}

std::string join(const std::vector<std::string>& parts, std::size_t begin, std::size_t end) {
    std::string value;
    for (auto index = begin; index < end && index < parts.size(); ++index) {
        if (!value.empty()) value += '/';
        value += parts[index];
    }
    return value;
}

std::optional<std::string> normalize_relative(std::string_view value) {
    std::vector<std::string> result;
    for (auto& part : split(value)) {
        if (part == ".") continue;
        if (part == "..") {
            if (result.empty()) return std::nullopt;
            result.pop_back();
        } else result.push_back(std::move(part));
    }
    return join(result, 0, result.size());
}

std::string parent_of(std::string_view value) {
    const auto slash = value.rfind('/');
    return slash == std::string_view::npos ? std::string() : std::string(value.substr(0, slash));
}

std::optional<std::string> resolve_relative(std::string value, const LinkMap& links, bool resolve_last) {
    for (int round = 0; round < 40; ++round) {
        const auto parts = split(value);
        const auto limit = resolve_last ? parts.size() : (parts.empty() ? 0 : parts.size() - 1);
        bool changed = false;
        std::string prefix;
        for (std::size_t index = 0; index < limit; ++index) {
            prefix = prefix.empty() ? parts[index] : prefix + "/" + parts[index];
            const auto found = links.find(link_key(prefix));
            if (found == links.end()) continue;
            const auto parent = parent_of(prefix);
            auto joined = parent.empty() ? found->second.target : parent + "/" + found->second.target;
            if (index + 1 < parts.size()) joined += "/" + join(parts, index + 1, parts.size());
            const auto normalized = normalize_relative(joined);
            if (!normalized) return std::nullopt;
            value = *normalized;
            changed = true;
            break;
        }
        if (!changed) return value;
    }
    return std::nullopt; // ELOOP
}

} // namespace install_detail

using namespace install_detail;

namespace {

struct RootState {
    std::optional<std::filesystem::path> root;
    std::string generic;
    LinkMap links;
};

RootState& state() {
    static RootState value;
    return value;
}

std::optional<std::string> root_relative(const std::filesystem::path& host) {
    const auto& current = state();
    if (!current.root) return std::nullopt;
    const auto value = generic_text(host.lexically_normal());
    if (value.size() <= current.generic.size() + 1 || value[current.generic.size()] != '/' ||
        link_key(value.substr(0, current.generic.size())) != link_key(current.generic)) return std::nullopt;
    return value.substr(current.generic.size() + 1);
}

} // namespace

namespace {
std::optional<std::filesystem::path> environment_path(const char* name) {
    const auto value = host::process::environment(name);
    if (!value || value->empty()) return std::nullopt;
    return utf8_path(*value);
}
} // namespace

std::filesystem::path default_alchemy_root() {
#ifdef _WIN32
    if (const auto local = environment_path("LOCALAPPDATA")) return *local / "Alchemy" / "root";
#endif
    if (const auto home = environment_path("HOME")) return *home / ".alchemy" / "root";
    throw std::runtime_error("cannot select a default Alchemy root (LOCALAPPDATA/HOME unset); pass --root");
}

std::optional<std::filesystem::path> find_alchemy_root(const std::filesystem::path& start) {
    std::error_code error;
    auto current = std::filesystem::absolute(start, error);
    if (error) return std::nullopt;
    current = current.lexically_normal();
    for (int depth = 0; depth < 128 && !current.empty(); ++depth) {
        if (std::filesystem::is_regular_file(current / std::filesystem::path(alchemy_root_marker), error)) return current;
        const auto parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }
    return std::nullopt;
}

std::filesystem::path create_alchemy_root(const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    const auto canonical = std::filesystem::canonical(root);
    const auto marker = canonical / std::filesystem::path(alchemy_root_marker);
    if (!std::filesystem::exists(marker)) {
        std::ofstream output(marker, std::ios::binary);
        output << "alchemy-root\t1\n";
        if (!output) throw std::runtime_error("cannot write the Alchemy root marker: " + generic_text(marker));
    }
    for (const auto* directory : {"Applications", "Library", ".alchemy/links", ".alchemy/Volumes", "Users", "etc", "usr/local", "var/log"}) {
        std::filesystem::create_directories(canonical / std::filesystem::path(directory));
    }
    return canonical;
}

LinkMap read_manifest(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read link manifest: " + generic_text(file));
    std::string line;
    if (!std::getline(input, line) || line != "alchemy-links\t1")
        throw std::runtime_error("unsupported link manifest header: " + generic_text(file));
    LinkMap result;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        std::vector<std::string> fields;
        std::size_t begin = 0;
        for (;;) {
            const auto tab = line.find('\t', begin);
            fields.push_back(line.substr(begin, tab == std::string::npos ? std::string::npos : tab - begin));
            if (tab == std::string::npos) break;
            begin = tab + 1;
        }
        if (fields.size() != 5 || fields[0] != "link" || (fields[1] != "dir" && fields[1] != "file") ||
            fields[3].empty() || fields[4].empty() || fields[4].front() == '/' || !normalize_relative(fields[3]))
            throw std::runtime_error("invalid link manifest entry in " + generic_text(file) + ": " + line);
        InstalledLink link{fields[3], fields[4], fields[1] == "dir", fields[2]};
        result.insert_or_assign(link_key(link.path), std::move(link));
    }
    return result;
}

void configure_alchemy_root(const std::filesystem::path& root) {
    const auto canonical = std::filesystem::canonical(root);
    if (!std::filesystem::is_regular_file(canonical / std::filesystem::path(alchemy_root_marker)))
        throw std::runtime_error("not an Alchemy root (missing " + std::string(alchemy_root_marker) + "): " + generic_text(canonical));
    LinkMap links;
    const auto directory = canonical / ".alchemy" / "links";
    std::error_code error;
    if (std::filesystem::is_directory(directory, error)) {
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            if (entry.path().extension() != ".tsv") continue;
            for (auto& [key, link] : read_manifest(entry.path())) links.insert_or_assign(key, std::move(link));
        }
    }
    auto& current = state();
    current.root = canonical;
    current.generic = generic_text(canonical);
    current.links = std::move(links);
    guest_root_mapping().host_generic = current.generic;
}

void clear_alchemy_root() {
    state() = {};
    guest_root_mapping().host_generic.clear();
}

std::optional<std::filesystem::path> configured_alchemy_root() { return state().root; }

std::size_t installed_link_count() { return state().links.size(); }

namespace {
std::filesystem::path runtime_manifest() {
    return *state().root / ".alchemy" / "links" / "runtime.tsv";
}
void write_runtime_manifest() {
    const auto file = runtime_manifest();
    std::filesystem::create_directories(file.parent_path());
    const auto temporary = std::filesystem::path(file).concat(".tmp");
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << "alchemy-links\t1\n";
        for (const auto& [key, link] : state().links) {
            static_cast<void>(key);
            if (link.representation.rfind("runtime-", 0) != 0) continue;
            output << "link\t" << (link.directory ? "dir" : "file") << '\t' << link.representation << '\t'
                   << link.path << '\t' << link.target << '\n';
        }
        if (!output) throw std::runtime_error("cannot write link manifest " + generic_text(temporary));
    }
    std::filesystem::rename(temporary, file);
}
[[noreturn]] void link_error(int code, const std::string& what) {
    throw std::system_error(code, std::generic_category(), "Alchemy run-time link: " + what);
}
} // namespace

void create_runtime_link(const std::filesystem::path& link, const std::string& target) {
    auto& current = state();
    const auto relative = root_relative(link);
    if (!current.root || !relative) link_error(EXDEV, "link outside the Alchemy root: " + generic_text(link));
    if (target.empty() || target.front() == '/' || target.find_first_of(std::string_view("\t\n\r\0", 4)) != std::string::npos)
        link_error(EINVAL, "unsupported link text: " + target);
    std::error_code error;
    if (std::filesystem::exists(std::filesystem::symlink_status(link, error)) || installed_link(link))
        link_error(EEXIST, generic_text(link));
    const auto parent = parent_of(*relative);
    const auto joined = normalize_relative(parent.empty() ? target : parent + "/" + target);
    if (!joined) link_error(EXDEV, "link target leaves the Alchemy root: " + target);
    const auto resolved = resolve_relative(*joined, current.links, true);
    if (!resolved) link_error(ELOOP, target);
    const auto host_target = utf8_path(current.generic + "/" + *resolved).make_preferred();
    const auto status = std::filesystem::status(host_target, error);
    if (!std::filesystem::exists(status)) link_error(ENOENT, "dangling link target " + target);
    InstalledLink record{*relative, target, std::filesystem::is_directory(status), {}};
    if (record.directory) {
#ifdef _WIN32
        make_junction(link, host_target);
        record.representation = "runtime-junction";
#else
        std::filesystem::create_directory_symlink(host_target, link);
        record.representation = "runtime-symlink";
#endif
    } else {
        std::filesystem::create_hard_link(host_target, link, error);
        if (!error) record.representation = "runtime-hardlink";
        else {
            std::filesystem::copy_file(host_target, link);
            record.representation = "runtime-copy";
        }
    }
    current.links.insert_or_assign(link_key(record.path), record);
    write_runtime_manifest();
}

void forget_installed_link(const std::filesystem::path& host) {
    auto& current = state();
    const auto relative = root_relative(host);
    if (!current.root || !relative) return;
    const auto parent = parent_of(*relative);
    const auto resolved_parent = parent.empty() ? std::optional<std::string>(std::string())
                                                : resolve_relative(parent, current.links, true);
    if (!resolved_parent) return;
    const auto name = relative->substr(parent.empty() ? 0 : parent.size() + 1);
    const auto key = link_key(resolved_parent->empty() ? name : *resolved_parent + "/" + name);
    const auto found = current.links.find(key);
    if (found == current.links.end()) return;
    const bool runtime = found->second.representation.rfind("runtime-", 0) == 0;
    current.links.erase(found);
    if (runtime) write_runtime_manifest();
}

std::filesystem::path resolve_installed_links(const std::filesystem::path& host) {
    const auto relative = root_relative(host);
    if (!relative || state().links.empty()) return host;
    const auto resolved = resolve_relative(*relative, state().links, true);
    if (!resolved || *resolved == *relative) return host;
    return utf8_path(state().generic + "/" + *resolved).make_preferred();
}

std::filesystem::path installed_real_path(const std::filesystem::path& host) {
    std::error_code error;
    auto canonical = std::filesystem::canonical(host, error);
    if (error) canonical = host.lexically_normal();
    return resolve_installed_links(canonical);
}

std::optional<InstalledLink> installed_link(const std::filesystem::path& host) {
    const auto relative = root_relative(host);
    if (!relative || state().links.empty()) return std::nullopt;
    const auto resolved = resolve_relative(*relative, state().links, false);
    if (!resolved) return std::nullopt;
    const auto found = state().links.find(link_key(*resolved));
    if (found == state().links.end()) return std::nullopt;
    return found->second;
}

std::vector<InstalledLink> read_link_list(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read link list: " + generic_text(file));
    std::vector<InstalledLink> result;
    std::string line;
    std::size_t number = 0;
    while (std::getline(input, line)) {
        ++number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (number == 1 && line.starts_with("\xEF\xBB\xBF")) line.erase(0, 3);
        if (line.empty() || line.front() == '#') continue;
        const auto tab = line.find('\t');
        if (tab == std::string::npos || tab == 0 || tab + 1 == line.size() || line.find('\t', tab + 1) != std::string::npos)
            throw std::runtime_error("invalid link list line " + std::to_string(number) + ": expected path<TAB>target");
        InstalledLink link;
        link.path = line.substr(0, tab);
        link.target = line.substr(tab + 1);
        for (auto& character : link.path) if (character == '\\') character = '/';
        result.push_back(std::move(link));
    }
    return result;
}

} // namespace alchemy
