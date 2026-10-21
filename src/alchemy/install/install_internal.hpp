#pragma once

// Shared helpers of the Alchemy root and installer (owner: install).

#include "alchemy/alchemy_root.hpp"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy {

// Key: link_key(path); path relative to a root or bundle, '/'-separated.
using LinkMap = std::map<std::string, InstalledLink>;

LinkMap read_manifest(const std::filesystem::path& file);

namespace install_detail {

std::string generic_text(const std::filesystem::path& path);
std::string link_key(std::string_view value);
std::vector<std::string> split(std::string_view value);
std::string join(const std::vector<std::string>& parts, std::size_t begin, std::size_t end);
// Lexical normalization of a relative '/' path; nullopt when ".." leaves it.
std::optional<std::string> normalize_relative(std::string_view value);
std::string parent_of(std::string_view value);
// Substitutes recorded links component by component (like realpath(3));
// resolve_last=false keeps the final component. nullopt on escape or loop.
std::optional<std::string> resolve_relative(std::string value, const LinkMap& links, bool resolve_last);
// A directory link at `link` to the directory `target` that needs no privilege
// (host::files::create_junction: an NTFS junction on Windows).
void make_junction(const std::filesystem::path& link, const std::filesystem::path& target);

} // namespace install_detail
} // namespace alchemy
