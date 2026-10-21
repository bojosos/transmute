#pragma once

// Symbolic links the host cannot represent (no unprivileged Windows symlinks,
// and Alchemy's run-time links need an existing target inside the root).
// Darwin programs create such links as plain data: Chrome's ProcessSingleton
// links SingletonLock to "<hostname>-<pid>" (dangling) and SingletonSocket /
// SingletonCookie to entries of a temporary directory outside the root.
//
// A record keeps the exact link text for a host path. readlink, lstat (an
// S_IFLNK entry), unlink and symlink's EEXIST check use it, and AF_UNIX
// socket addresses follow it. Nothing is created on the host filesystem, so
// file opens through a record do not follow it: a dangling target is what
// these links are (open fails with ENOENT, as on Darwin), and a record whose
// target exists is followed only by the socket layer.
//
// Records are process-wide and, with a configured Alchemy root, persisted in
// <root>/.alchemy/symlinks.tsv (loaded on first use, rewritten on change), so
// a later Alchemy process sees a stale lock link as Darwin would.

#include <filesystem>
#include <optional>
#include <string>

namespace alchemy::libsystem {

// The recorded link text for `host`, if `host` is a record.
[[nodiscard]] std::optional<std::string> symlink_record(const std::filesystem::path& host);
// Records `host` -> `target` (the guest link text, verbatim). False when a
// record already exists for `host` (EEXIST).
bool add_symlink_record(const std::filesystem::path& host, const std::string& target);
// Removes the record for `host`; false when there is none.
bool remove_symlink_record(const std::filesystem::path& host);
// Drops the in-memory table so the next use reloads it (tests).
void reset_symlink_records();

} // namespace alchemy::libsystem
