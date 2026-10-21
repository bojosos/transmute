#include "alchemy/darwin/libsystem/fs/symlink_records.hpp"

#include "alchemy/alchemy_root.hpp"

#include <fstream>
#include <map>
#include <mutex>

namespace alchemy::libsystem {
namespace {

struct Table {
    std::mutex mutex;
    bool loaded{};
    std::optional<std::filesystem::path> file;
    std::map<std::string, std::pair<std::string, std::string>> links;   // key -> (host text, target)
};
Table& table() {
    static Table value;
    return value;
}

std::string host_text(const std::filesystem::path& host) {
    const auto bytes = host.lexically_normal().generic_u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
std::string key_of(const std::string& text) {
    auto key = text;
#ifdef _WIN32
    for (auto& character : key) if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
#endif
    return key;
}

// Fields are tab-separated; link text with a tab or newline is never recorded.
void load(Table& t) {
    if (t.loaded) return;
    t.loaded = true;
    t.links.clear();
    const auto root = configured_alchemy_root();
    t.file = root ? std::optional(*root / ".alchemy" / "symlinks.tsv") : std::nullopt;
    if (!t.file) return;
    std::ifstream input(*t.file, std::ios::binary);
    std::string line;
    if (!input || !std::getline(input, line) || line != "alchemy-symlinks\t1") return;
    while (std::getline(input, line)) {
        const auto tab = line.find('\t');
        if (tab == std::string::npos || tab == 0 || tab + 1 >= line.size()) continue;
        auto host = line.substr(0, tab);
        t.links.insert_or_assign(key_of(host), std::pair(host, line.substr(tab + 1)));
    }
}

void save(const Table& t) {
    if (!t.file) return;
    std::error_code error;
    std::filesystem::create_directories(t.file->parent_path(), error);
    const auto temporary = std::filesystem::path(*t.file).concat(".tmp");
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << "alchemy-symlinks\t1\n";
        for (const auto& [key, value] : t.links) {
            static_cast<void>(key);
            output << value.first << '\t' << value.second << '\n';
        }
        if (!output) return;   // the in-memory table stays authoritative for this process
    }
    std::filesystem::rename(temporary, *t.file, error);
}

} // namespace

std::optional<std::string> symlink_record(const std::filesystem::path& host) {
    auto& t = table();
    std::lock_guard lock(t.mutex);
    load(t);
    const auto found = t.links.find(key_of(host_text(host)));
    if (found == t.links.end()) return std::nullopt;
    return found->second.second;
}

bool add_symlink_record(const std::filesystem::path& host, const std::string& target) {
    if (target.empty() || target.find_first_of(std::string_view("\t\n\r\0", 4)) != std::string::npos) return false;
    auto& t = table();
    std::lock_guard lock(t.mutex);
    load(t);
    const auto text = host_text(host);
    if (!t.links.emplace(key_of(text), std::pair(text, target)).second) return false;
    save(t);
    return true;
}

bool remove_symlink_record(const std::filesystem::path& host) {
    auto& t = table();
    std::lock_guard lock(t.mutex);
    load(t);
    if (!t.links.erase(key_of(host_text(host)))) return false;
    save(t);
    return true;
}

void reset_symlink_records() {
    auto& t = table();
    std::lock_guard lock(t.mutex);
    t.loaded = false;
    t.links.clear();
}

} // namespace alchemy::libsystem
