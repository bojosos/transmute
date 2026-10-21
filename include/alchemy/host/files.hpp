#pragma once

// Host area "files" (owner: r1). The rules are in include/alchemy/host/host.hpp. Files and
// directories: open, read and write, metadata, directories, links, extended attributes, locks and
// change watching. Paths are host paths in UTF-8 (the macOS layer maps guest paths first,
// alchemy/path.hpp).

#include "alchemy/host/host.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <string_view>
#include <vector>

namespace alchemy::host::files {

enum class Access : std::uint8_t { read = 1, write = 2, read_write = 3 };
enum class Create : std::uint8_t { open_existing, create_new, open_or_create, truncate_existing, create_or_truncate };
// Options for open: a file the call creates is accessible to its owner only
// (mode 0600), or read-only (a mode without 0200); open_deletable lets
// remove_open delete the open file. Files and directories both open; the
// handle is not inherited by child processes.
// open_append: writes go to the end of the file, atomically (O_APPEND of a
// shell's >> redirection; the handle cannot write elsewhere).
// open_sequential: the file is read front to back (a read-ahead hint).
inline constexpr std::uint32_t open_owner_only = 1, open_create_read_only = 2, open_deletable = 4, open_append = 8, open_sequential = 16;
[[nodiscard]] Result<Handle> open(std::string_view path, Access access, Create create, std::uint32_t options = 0);
// The null device (/dev/null).
[[nodiscard]] Result<Handle> open_null(Access access);
// Positional I/O; returns the bytes transferred (0 at end of file).
[[nodiscard]] Result<std::size_t> read_at(Handle file, Bytes buffer, std::uint64_t offset);
[[nodiscard]] Result<std::size_t> write_at(Handle file, ConstBytes data, std::uint64_t offset);
[[nodiscard]] Result<std::uint64_t> size(Handle file);
// Sequential I/O at the handle's current position (pipes, consoles, files);
// returns the bytes transferred (a read returns 0 at end of file).
[[nodiscard]] Result<std::size_t> read(Handle file, Bytes buffer);
[[nodiscard]] Result<std::size_t> write(Handle file, ConstBytes data);
// The handle's position (lseek); handles duplicated from one open share it.
enum class Whence : std::uint8_t { set, current, end };
[[nodiscard]] Result<std::uint64_t> seek(Handle file, std::int64_t offset, Whence whence);
// Moves the end of file without moving the position (ftruncate); growing
// fills with zeros. A shrink the host refuses while the file is mapped fails
// with Error::busy.
[[nodiscard]] Status set_size(Handle file, std::uint64_t size);
// Writes the file's data and metadata to the disk (fsync).
[[nodiscard]] Status flush(Handle file);
// fsync of a directory: its entries reach the disk. Error::not_found when it
// is missing; a host that cannot flush a directory succeeds (its file system
// journals directory metadata).
[[nodiscard]] Status sync_directory(std::string_view path);
// A character device: a console or the null device (isatty).
[[nodiscard]] bool is_character_device(Handle file) noexcept;
// What an open handle is (a descriptor inherited from another process).
enum class Kind : std::uint8_t { file, pipe, character, unknown };
[[nodiscard]] Kind kind(Handle file) noexcept;
// unlink(2) of an open file (the handle needs open_deletable): the name can no
// longer be opened at once, and is gone at once where the file system has
// POSIX deletion, else when the last handle closes.
[[nodiscard]] Status remove_open(Handle file);
[[nodiscard]] Status create_directory(std::string_view path, bool owner_only = false);
// chmod as far as the host's attributes go: write permission or none.
[[nodiscard]] Status set_read_only(std::string_view path, bool read_only);

// An anonymous pipe (pipe(2)) with a buffer of at least 64 KiB; neither end is
// inherited by child processes.
struct Pipe {
    Handle read, write;
};
[[nodiscard]] Result<Pipe> create_pipe();
// Bytes waiting in a pipe without reading them (FIONREAD). A pipe whose
// writers are gone fails with Error::broken_pipe.
[[nodiscard]] Result<std::size_t> pipe_available(Handle pipe);

// stat(2): what the host records for a path (following a final symbolic
// link unless `follow` is false) or an open file.
struct Metadata {
    std::uint64_t device{}, inode{}, size{}, allocated{}, links{};
    std::uint32_t mode{};                      // the host's attribute or mode bits
    bool directory{}, symlink{}, read_only{}, owner_only{};
    std::int64_t accessed_ns{}, modified_ns{}, changed_ns{}, created_ns{};   // Unix epoch
};
[[nodiscard]] Result<Metadata> metadata(std::string_view path, bool follow = true);
[[nodiscard]] Result<Metadata> metadata(Handle file);
// The file's identity: the same for every handle to one file, on any process.
struct Identity {
    std::uint64_t volume{};
    std::array<std::uint8_t, 16> id{};
};
[[nodiscard]] Result<Identity> identity(Handle file);
// Byte-range locks on the host file (flock emulation). lock never waits: a
// conflicting holder makes it fail with Error::would_block.
[[nodiscard]] Status lock(Handle file, std::uint64_t offset, std::uint64_t length, bool exclusive);
[[nodiscard]] Status unlock(Handle file, std::uint64_t offset, std::uint64_t length);
// utimes: sets a path's access and modification times (Unix-epoch
// nanoseconds); an absent time is left as it is. A time the host cannot
// represent fails with Error::invalid_argument.
[[nodiscard]] Status set_times(std::string_view path, std::optional<std::int64_t> accessed_ns, std::optional<std::int64_t> modified_ns);
// rename(2); without `replace` an existing `to` fails with Error::exists,
// checked atomically by the host.
[[nodiscard]] Status rename(std::string_view from, std::string_view to, bool replace);
// access(2) as far as the host's attributes go: the path exists, and with
// `write` it is not read-only (Error::access_denied).
[[nodiscard]] Status access(std::string_view path, bool write);
// A console (an interactive terminal): isatty of the standard streams.
[[nodiscard]] bool is_console(Handle file) noexcept;

// Extended attributes: named byte strings attached to a file or directory
// (getxattr and friends). Names are UTF-8 without '/' or '\'; the host stores
// them as it can (Windows: NTFS alternate data streams). A missing attribute
// fails with Error::not_found, as does a missing file: check the file first.
[[nodiscard]] Result<std::vector<std::byte>> get_attribute(std::string_view path, std::string_view name);
[[nodiscard]] Result<bool> has_attribute(std::string_view path, std::string_view name);
[[nodiscard]] Status set_attribute(std::string_view path, std::string_view name, ConstBytes value);
[[nodiscard]] Status remove_attribute(std::string_view path, std::string_view name);
[[nodiscard]] Result<std::vector<std::string>> list_attributes(std::string_view path);

// Links (the `alchemy install` root builder). A symbolic link to `target`
// ('/' separated; relative or absolute); `directory` marks a link to a
// directory, as the host needs to know.
[[nodiscard]] Status create_symbolic_link(std::string_view link, std::string_view target, bool directory);
// A directory junction (an NTFS mount point; needs no privilege) to an
// absolute directory path on a local drive; a directory symbolic link
// where the host has no junctions. A target that is no absolute drive path
// fails with Error::invalid_argument, one too long with Error::name_too_long.
[[nodiscard]] Status create_junction(std::string_view link, std::string_view absolute_target);
[[nodiscard]] Status create_hard_link(std::string_view link, std::string_view target);
// Removes `path` and everything below it without following any link or
// junction inside it, clearing read-only attributes on the way. A missing
// path succeeds.
[[nodiscard]] Status remove_tree(std::string_view path);

// A std::filesystem error as a host error.
[[nodiscard]] Error error(const std::error_code& code) noexcept;

// What the host records about a path itself (a symbolic link is not
// followed). A missing path fails with Error::not_found.
struct Attributes {
    bool directory{}, read_only{}, hidden{}, symlink{};
};
[[nodiscard]] Result<Attributes> attributes(std::string_view path);
// A path's times in nanoseconds since the Unix epoch (directories and volume
// roots included); created is 0 where the host does not record it.
struct Times {
    std::int64_t created_ns{}, modified_ns{}, accessed_ns{};
};
[[nodiscard]] Result<Times> times(std::string_view path);

// The mounted volume that holds `path` (statfs, CFURL volume keys, FSGetVolumeInfo).
enum class VolumeKind : std::uint8_t { fixed, removable, optical, network, ram, unknown };
struct Volume {
    std::string root;                          // the volume's root path, with its trailing separator
    std::string name;                          // the label; empty when it has none
    std::string filesystem;                    // as the host names it ("NTFS")
    std::uint32_t serial{}, max_name_length{};
    VolumeKind kind{VolumeKind::unknown};
    bool read_only{};                          // the volume itself is read-only
    bool system{};                             // the volume the host OS runs from
    std::uint64_t total{}, free{}, available{}; // bytes; available is what this user may use
    std::uint32_t block_size{};                // allocation unit in bytes (0: unknown)
};
[[nodiscard]] Result<Volume> volume(std::string_view path);
// The roots of the mounted volumes, in the host's order.
[[nodiscard]] Result<std::vector<std::string>> volume_roots();

// Replaces `to` with `from` in one step, flushed to disk before it returns
// (rename(2) and fsync): `to` is the old or the new file, never neither.
[[nodiscard]] Status replace(std::string_view from, std::string_view to);

// Change watching (kqueue EVFILT_VNODE). open_metadata opens a file or
// directory for its metadata only, without keeping it from being renamed or
// deleted; close it with host::close.
[[nodiscard]] Result<Handle> open_metadata(std::string_view path);
struct Info {
    std::int64_t modified_ns{}, changed_ns{};  // Unix epoch; changed: the metadata change time
    std::uint64_t size{}, links{};
    std::uint32_t mode{};                      // the host's attribute or mode bits, to compare for changes
    bool directory{}, delete_pending{};        // delete_pending: deleted, or deleted when the last handle closes
};
[[nodiscard]] Result<Info> info(Handle file);
// The path an open file has now (after renames), in the host's form.
[[nodiscard]] Result<std::string> final_path(Handle file);
// A watch for entries created, deleted or renamed directly in a directory.
// directory_changed reports whether that happened since the watch began or
// since the last call that reported it.
[[nodiscard]] Result<Handle> watch_directory(std::string_view path);
[[nodiscard]] Result<bool> directory_changed(Handle watch);
void close_watch(Handle watch) noexcept;

// A recursive watch of a directory tree (FSEvents): what is created, removed,
// renamed or modified anywhere below the directory, with each entry's path.
struct TreeChange {
    enum class Kind : std::uint8_t { created, removed, modified, renamed_from, renamed_to };
    Kind kind{};
    std::string path;   // relative to the watched directory, '/'-separated, UTF-8
};
class TreeWatch {
public:
    virtual ~TreeWatch() = default;
    // Waits up to `timeout_ms` for changes and returns the ones that arrived
    // (none at the timeout). `overflow` is set when the host lost changes, so
    // the watcher must rescan. After cancel() it fails with Error::interrupted.
    [[nodiscard]] virtual Result<std::vector<TreeChange>> read(std::uint32_t timeout_ms, bool& overflow) = 0;
    // Wakes a read() in progress (any thread).
    virtual void cancel() noexcept = 0;
};
[[nodiscard]] Result<std::unique_ptr<TreeWatch>> watch_tree(std::string_view path);

// Creates a new empty file with a unique name in the host's temporary
// directory and returns its path; `prefix` starts the name (the host may
// shorten it).
[[nodiscard]] Result<std::string> create_temporary(std::string_view prefix);

} // namespace alchemy::host::files
