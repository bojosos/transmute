#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace alchemy {

inline std::filesystem::path utf8_path(std::string_view value) {
    std::u8string bytes;
    bytes.reserve(value.size());
    for (const auto byte : value) bytes.push_back(static_cast<char8_t>(static_cast<unsigned char>(byte)));
    return std::filesystem::path(bytes);
}

// The host path in UTF-8, as the host API (alchemy/host/) takes paths.
inline std::string path_utf8(const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

// Alchemy root mapping (alchemy_root.hpp configures it once before guest
// execution). Guest /Applications, /Library and /Users, and the system
// directories installers write (/etc, /opt, /private, /usr, /var), live under
// the root; an empty host_generic keeps the default drive-relative mapping.
struct GuestRootMapping {
    std::string host_generic;  // absolute generic UTF-8 root, no trailing '/'
};
inline GuestRootMapping& guest_root_mapping() {
    static GuestRootMapping value;
    return value;
}
inline constexpr std::array<std::string_view, 8> guest_root_directories{
    "Applications", "Library", "Users", "etc", "opt", "private", "usr", "var"};

namespace path_detail {
inline bool same_ascii_prefix(std::string_view value, std::string_view prefix) {
    if (value.size() < prefix.size()) return false;
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        auto left = value[index], right = prefix[index];
#ifdef _WIN32
        if (left >= 'A' && left <= 'Z') left = static_cast<char>(left - 'A' + 'a');
        if (right >= 'A' && right <= 'Z') right = static_cast<char>(right - 'A' + 'a');
#endif
        if (left != right) return false;
    }
    return true;
}
// Length of "/<directory>" when value (a '/'-rooted generic path) starts with
// one of the root directories as a whole component.
inline std::size_t root_directory_prefix(std::string_view value) {
    for (const auto directory : guest_root_directories) {
        if (value.size() < directory.size() + 1 || value[0] != '/') continue;
        if (!same_ascii_prefix(value.substr(1), directory)) continue;
        if (value.size() == directory.size() + 1 || value[directory.size() + 1] == '/') return directory.size() + 1;
    }
    return 0;
}
} // namespace path_detail

// Host location of a normalized absolute guest path inside the Alchemy root.
inline std::optional<std::filesystem::path> guest_root_host_path(std::string_view guest) {
    const auto& root = guest_root_mapping().host_generic;
    if (root.empty() || !path_detail::root_directory_prefix(guest)) return std::nullopt;
    return utf8_path(root + std::string(guest));
}

inline std::string guest_path_string(const std::filesystem::path& path) {
    const auto bytes = path.generic_u8string();
    std::string value(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (const auto& root = guest_root_mapping().host_generic; !root.empty() &&
        path_detail::same_ascii_prefix(value, root) && value.size() > root.size() &&
        path_detail::root_directory_prefix(std::string_view(value).substr(root.size()))) {
        return value.substr(root.size());
    }
    if (value.size() >= 3 && value[1] == ':' && value[2] == '/' &&
        ((value[0] >= 'A' && value[0] <= 'Z') || (value[0] >= 'a' && value[0] <= 'z'))) {
        const auto drive = value[0] >= 'a' && value[0] <= 'z' ? static_cast<char>(value[0] - 'a' + 'A') : value[0];
        return std::string("/Volumes/") + drive + value.substr(2);
    }
    return value;
}

} // namespace alchemy
