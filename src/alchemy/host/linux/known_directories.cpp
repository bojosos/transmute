// Linux host backend, area "system": per-user known directories after the XDG Base Directory
// specification ($XDG_DATA_HOME, $XDG_CACHE_HOME; relative values are ignored), $HOME or the
// password database for the home directory, and $TMPDIR or /tmp.
#include "alchemy/host/process.hpp"
#include "alchemy/host/system.hpp"

#include <pwd.h>
#include <unistd.h>

#include <vector>

namespace alchemy::host::system {

namespace {

std::optional<std::string> absolute_variable(std::string_view name) {
    auto value = process::environment(name);
    if (!value || value->empty() || value->front() != '/') return std::nullopt;
    while (value->size() > 1 && value->back() == '/') value->pop_back();
    return value;
}

Result<std::string> home() {
    if (auto value = absolute_variable("HOME")) return std::move(*value);
    const auto suggested = sysconf(_SC_GETPW_R_SIZE_MAX);
    std::vector<char> buffer(suggested > 0 ? static_cast<std::size_t>(suggested) : 16384);
    passwd entry{};
    passwd* found = nullptr;
    if (getpwuid_r(getuid(), &entry, buffer.data(), buffer.size(), &found) != 0 || !found || !found->pw_dir || found->pw_dir[0] != '/')
        return Error::not_found;
    return std::string(found->pw_dir);
}

// $<variable>/alchemy, else ~/<fallback>/alchemy.
Result<std::string> alchemy_directory(std::string_view variable, std::string_view fallback) {
    if (const auto base = absolute_variable(variable)) return *base + "/alchemy";
    auto directory = home();
    if (!directory) return directory.error();
    return *directory + "/" + std::string(fallback) + "/alchemy";
}

} // namespace

Result<std::string> known_directory(Directory which) {
    switch (which) {
    case Directory::home: return home();
    case Directory::data: return alchemy_directory("XDG_DATA_HOME", ".local/share");
    case Directory::cache: return alchemy_directory("XDG_CACHE_HOME", ".cache");
    case Directory::temporary:
        if (auto value = absolute_variable("TMPDIR")) return std::move(*value);
        return std::string("/tmp");
    }
    return Error::invalid_argument;
}

} // namespace alchemy::host::system
