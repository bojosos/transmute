#pragma once

// The Alchemy root: a host directory that holds installed macOS content.
// Guest /Applications and /Library map to <root>/Applications and
// <root>/Library (see path.hpp). Installed bundles keep their macOS symbolic
// links through a project-owned link representation recorded in
// <root>/.alchemy/links/*.tsv:
//   * a native relative symlink when the host lets an unprivileged process
//     create one (Developer Mode);
//   * otherwise an NTFS junction for a directory link and a hard link (or,
//     where hard links are unavailable, a copy) for a file link.
// The manifest preserves each link's original target text, so guest
// readlink/lstat/realpath and dyld-style path resolution see the macOS link
// even when the host representation is a junction or hard link.

#include <cstddef>
#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy {

struct InstalledLink {
    std::string path;            // root-relative, '/'-separated (Applications/X.app/...)
    std::string target;          // original link text, relative to the link's directory
    bool directory{};            // the final target is a directory
    std::string representation;  // symlink | junction | hardlink | copy
};

// Marker file identifying a directory as an Alchemy root.
inline constexpr std::string_view alchemy_root_marker = ".alchemy-root";

// %LOCALAPPDATA%\Alchemy\root (or $HOME/.alchemy/root off Windows).
[[nodiscard]] std::filesystem::path default_alchemy_root();
// Nearest ancestor of start (inclusive) that carries the root marker.
[[nodiscard]] std::optional<std::filesystem::path> find_alchemy_root(const std::filesystem::path& start);
// Creates the root layout (marker, Applications, Library, .alchemy/links).
std::filesystem::path create_alchemy_root(const std::filesystem::path& root);

// Process-wide configuration, made once before guest execution. It enables
// guest /Applications and /Library mapping and loads every link manifest.
// Throws when the directory is not an Alchemy root or a manifest is invalid.
void configure_alchemy_root(const std::filesystem::path& root);
void clear_alchemy_root();
[[nodiscard]] std::optional<std::filesystem::path> configured_alchemy_root();

// Host path with recorded portable links resolved like realpath(3) does for
// native links; paths outside the configured root are returned unchanged.
[[nodiscard]] std::filesystem::path resolve_installed_links(const std::filesystem::path& host);
// std::filesystem::canonical followed by resolve_installed_links.
[[nodiscard]] std::filesystem::path installed_real_path(const std::filesystem::path& host);
// The recorded link whose own path is host (its parent directories resolved);
// nullopt when host is not a recorded link.
[[nodiscard]] std::optional<InstalledLink> installed_link(const std::filesystem::path& host);
[[nodiscard]] std::size_t installed_link_count();

// Run-time links: a guest symlink(2) inside the configured root when the host
// refuses an unprivileged symbolic link. `target` is the link text relative
// to the link's directory; it must resolve to an existing file (hard link, or
// a copy where hard links are unavailable) or directory (junction). The link
// is recorded in <root>/.alchemy/links/runtime.tsv. Dangling targets are not
// representable and fail. Throws std::system_error with a POSIX errno value
// (EEXIST, ENOENT, EXDEV for a link outside the root) or runtime_error.
void create_runtime_link(const std::filesystem::path& link, const std::string& target);
// Forgets the recorded link at `host` after the guest removed it (runtime.tsv
// is rewritten; links of installed bundles are forgotten for this process).
void forget_installed_link(const std::filesystem::path& host);

// Link list file: one "relative/path<TAB>target" line per link, relative to
// the source bundle directory; '#' starts a comment line.
[[nodiscard]] std::vector<InstalledLink> read_link_list(const std::filesystem::path& file);

struct InstallOptions {
    std::filesystem::path source;                 // an extracted .app directory
    std::filesystem::path root;                   // the Alchemy root
    std::optional<std::filesystem::path> links;   // link list recovered from the disk image
    bool infer_framework_links{true};             // repair 0-byte framework placeholders
    bool replace{};                               // replace an existing installation
    bool allow_native_symlinks{true};             // try unprivileged native symlinks first
};

struct InstallResult {
    std::filesystem::path destination;
    std::filesystem::path manifest;
    std::size_t files{}, directories{};
    std::size_t bytes{};
    std::vector<InstalledLink> links;
    std::vector<std::string> inferred;            // link paths repaired by inference
};

// Copies the bundle into <root>/Applications/<name>.app, recreates its links,
// verifies every link resolves, then writes the manifest. Unsupported link
// forms (absolute targets, targets leaving the bundle, ambiguous framework
// versions) fail explicitly before anything is written.
InstallResult install_app(const InstallOptions& options, std::ostream& log);

} // namespace alchemy
