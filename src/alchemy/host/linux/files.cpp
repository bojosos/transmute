// Linux host backend, area "files".
//
// Most of the area is the POSIX call it is named after. What Linux lacks:
// - Creation times cannot be set, and there is no hidden flag: both are kept
//   in one extended attribute, user.alchemy.attributes (8 bytes of creation
//   time in Unix-epoch nanoseconds, then a hidden byte), which metadata and
//   times read back and list_attributes leaves out. A file system without
//   user extended attributes (or a symbolic link, which cannot have them)
//   reports Error::unsupported.
// - path_of has no inode lookup (see remember_path).
#include "alchemy/host/files.hpp"
#include "alchemy/host/linux/linux.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <sys/xattr.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

namespace alchemy::host::files {

namespace {

using linux_backend::fd_of;
using linux_backend::handle_of;
using linux_backend::last_error;
using linux_backend::zstring;

constexpr const char* kAttributePrefix = "user.";
constexpr const char* kExtrasAttribute = "user.alchemy.attributes";

struct Extras {
    std::int64_t created_ns{};   // 0: none recorded
    bool hidden{};
};

Extras extras_of(int fd, const char* path, bool follow) {
    unsigned char bytes[9]{};
    const auto got = fd >= 0 ? fgetxattr(fd, kExtrasAttribute, bytes, sizeof(bytes))
                     : follow ? getxattr(path, kExtrasAttribute, bytes, sizeof(bytes))
                              : lgetxattr(path, kExtrasAttribute, bytes, sizeof(bytes));
    Extras extras;
    if (got == static_cast<ssize_t>(sizeof(bytes))) {
        std::memcpy(&extras.created_ns, bytes, 8);
        extras.hidden = bytes[8] != 0;
    }
    return extras;
}

Status store_extras(const char* path, bool follow, const Extras& extras) {
    unsigned char bytes[9]{};
    std::memcpy(bytes, &extras.created_ns, 8);
    bytes[8] = extras.hidden ? 1 : 0;
    const int result = follow ? setxattr(path, kExtrasAttribute, bytes, sizeof(bytes), 0)
                              : lsetxattr(path, kExtrasAttribute, bytes, sizeof(bytes), 0);
    if (result == 0) return {};
    return errno == ENOTSUP || errno == EPERM ? Error::unsupported : last_error();
}

void apply_extras(Metadata& m, const Extras& extras) {
    if (extras.created_ns) m.created_ns = extras.created_ns;
    m.hidden = extras.hidden;
}

std::int64_t nanoseconds(const timespec& t) {
    return static_cast<std::int64_t>(t.tv_sec) * 1000000000 + t.tv_nsec;
}
std::int64_t nanoseconds(const statx_timestamp& t) {
    return static_cast<std::int64_t>(t.tv_sec) * 1000000000 + t.tv_nsec;
}

Status status_of(int result) {
    if (result == 0) return {};
    return last_error();
}

Metadata metadata_of(const struct statx& info) {
    Metadata m;
    m.device = makedev(info.stx_dev_major, info.stx_dev_minor);
    m.inode = info.stx_ino;
    m.size = info.stx_size;
    m.allocated = info.stx_blocks * 512;
    m.links = info.stx_nlink;
    m.mode = info.stx_mode;
    m.directory = S_ISDIR(info.stx_mode);
    m.symlink = S_ISLNK(info.stx_mode);
    m.read_only = (info.stx_mode & S_IWUSR) == 0;
    m.owner_only = (info.stx_mode & 077) == 0;
    m.accessed_ns = nanoseconds(info.stx_atime);
    m.modified_ns = nanoseconds(info.stx_mtime);
    m.changed_ns = nanoseconds(info.stx_ctime);
    m.created_ns = (info.stx_mask & STATX_BTIME) ? nanoseconds(info.stx_btime) : 0;
    return m;
}

Result<struct statx> stat_path(std::string_view path, bool follow) {
    struct statx info {};
    const auto name = zstring(path);
    if (statx(AT_FDCWD, name.c_str(), follow ? 0 : AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS | STATX_BTIME, &info) != 0)
        return last_error();
    return info;
}

Result<struct statx> stat_handle(Handle file) {
    struct statx info {};
    if (statx(fd_of(file), "", AT_EMPTY_PATH, STATX_BASIC_STATS | STATX_BTIME, &info) != 0) return last_error();
    return info;
}

Status set_times_at(std::string_view path, std::optional<std::int64_t> accessed_ns,
                    std::optional<std::int64_t> modified_ns, int flags) {
    timespec times[2];
    for (int i = 0; i < 2; ++i) {
        const auto& value = i == 0 ? accessed_ns : modified_ns;
        if (!value) {
            times[i] = {0, UTIME_OMIT};
            continue;
        }
        times[i].tv_sec = static_cast<time_t>(*value / 1000000000);
        times[i].tv_nsec = static_cast<long>(*value % 1000000000);
        if (times[i].tv_nsec < 0) {
            times[i].tv_nsec += 1000000000;
            --times[i].tv_sec;
        }
    }
    const auto name = zstring(path);
    return status_of(utimensat(AT_FDCWD, name.c_str(), times, flags));
}

std::string attribute_name(std::string_view name) { return kAttributePrefix + std::string(name); }

Status remove_tree_at(int directory, const char* name) {
    struct stat info {};
    if (fstatat(directory, name, &info, AT_SYMLINK_NOFOLLOW) != 0) return errno == ENOENT ? Status{} : last_error();
    if (!S_ISDIR(info.st_mode)) return status_of(unlinkat(directory, name, 0) == 0 || errno == ENOENT ? 0 : -1);
    const int fd = openat(directory, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return last_error();
    DIR* dir = fdopendir(fd);
    if (!dir) {
        ::close(fd);
        return last_error();
    }
    Status result;
    while (const dirent* entry = readdir(dir)) {
        if (!std::strcmp(entry->d_name, ".") || !std::strcmp(entry->d_name, "..")) continue;
        if (auto status = remove_tree_at(dirfd(dir), entry->d_name); !status && result) result = status;
    }
    closedir(dir);
    if (!result) return result;
    if (unlinkat(directory, name, AT_REMOVEDIR) != 0 && errno != ENOENT) return last_error();
    return {};
}

// Mounts from /proc/self/mountinfo: mount point and file system type.
struct Mount {
    std::string point, type, source;
    bool read_only{};
};
std::vector<Mount> mounts() {
    std::vector<Mount> list;
    FILE* info = std::fopen("/proc/self/mountinfo", "re");
    if (!info) return list;
    char line[4096];
    while (std::fgets(line, sizeof(line), info)) {
        // id parent major:minor root point options [optional...] - type source super
        char point[2048], options[512], type[128], source[1024];
        int consumed = 0;
        if (std::sscanf(line, "%*s %*s %*s %*s %2047s %511s%n", point, options, &consumed) != 2) continue;
        const char* dash = std::strstr(line + consumed, " - ");
        if (!dash || std::sscanf(dash + 3, "%127s %1023s", type, source) != 2) continue;
        // Octal escapes (\040 for a space) in the mount point.
        std::string decoded;
        for (const char* p = point; *p; ++p) {
            if (p[0] == '\\' && p[1] >= '0' && p[1] <= '3' && p[2] && p[3]) {
                decoded += static_cast<char>((p[1] - '0') * 64 + (p[2] - '0') * 8 + (p[3] - '0'));
                p += 3;
            } else {
                decoded += *p;
            }
        }
        list.push_back({decoded, type, source, std::strncmp(options, "ro", 2) == 0 && (options[2] == ',' || !options[2])});
    }
    std::fclose(info);
    return list;
}

// path_of: Linux cannot look a file up by its inode, so the paths this
// backend has stat'ed or opened are remembered (the macOS layer stats what it
// later asks fsgetpath for), checked again before use, and a miss searches the
// volume breadth-first within a budget.
struct InodeKey {
    std::uint64_t device, inode;
    friend bool operator<(const InodeKey& a, const InodeKey& b) {
        return a.device != b.device ? a.device < b.device : a.inode < b.inode;
    }
};
std::mutex g_paths_lock;
std::map<InodeKey, std::string>& known_paths() {
    static auto* map = new std::map<InodeKey, std::string>;
    return *map;
}

void remember_path(std::uint64_t device, std::uint64_t inode, std::string_view path) {
    if (path.empty() || path.front() != '/') {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd))) return;
        std::lock_guard guard(g_paths_lock);
        if (known_paths().size() > 65536) known_paths().clear();
        known_paths()[{device, inode}] = std::string(cwd) + "/" + std::string(path);
        return;
    }
    std::lock_guard guard(g_paths_lock);
    if (known_paths().size() > 65536) known_paths().clear();
    known_paths()[{device, inode}] = std::string(path);
}

class InotifyTreeWatch final : public TreeWatch {
public:
    InotifyTreeWatch(int fd, int cancel, std::string root) : fd_(fd), cancel_(cancel), root_(std::move(root)) {}
    ~InotifyTreeWatch() override {
        ::close(fd_);
        ::close(cancel_);
    }
    bool add(const std::string& relative) {
        const auto path = relative.empty() ? root_ : root_ + "/" + relative;
        const int wd = inotify_add_watch(fd_, path.c_str(),
                                         IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE |
                                             IN_ATTRIB | IN_ONLYDIR | IN_DONT_FOLLOW);
        if (wd < 0) return false;
        directories_[wd] = relative;
        if (DIR* dir = opendir(path.c_str())) {
            while (const dirent* entry = readdir(dir)) {
                if (entry->d_type != DT_DIR || !std::strcmp(entry->d_name, ".") || !std::strcmp(entry->d_name, ".."))
                    continue;
                add(relative.empty() ? entry->d_name : relative + "/" + entry->d_name);
            }
            closedir(dir);
        }
        return true;
    }
    Result<std::vector<TreeChange>> read(std::uint32_t timeout_ms, bool& overflow) override {
        overflow = false;
        pollfd fds[2] = {{fd_, POLLIN, 0}, {cancel_, POLLIN, 0}};
        const int ready = poll(fds, 2, static_cast<int>(std::min<std::uint32_t>(timeout_ms, INT_MAX)));
        if (ready < 0) return errno == EINTR ? Result<std::vector<TreeChange>>(std::vector<TreeChange>{}) : last_error();
        if (fds[1].revents) return Error::interrupted;
        std::vector<TreeChange> changes;
        if (!fds[0].revents) return changes;
        alignas(inotify_event) char buffer[16384];
        const auto got = ::read(fd_, buffer, sizeof(buffer));
        for (ssize_t at = 0; at < got;) {
            const auto* event = reinterpret_cast<const inotify_event*>(buffer + at);
            at += static_cast<ssize_t>(sizeof(inotify_event) + event->len);
            if (event->mask & IN_Q_OVERFLOW) {
                overflow = true;
                continue;
            }
            const auto directory = directories_.find(event->wd);
            if (directory == directories_.end() || !event->len) continue;
            auto path = directory->second.empty() ? std::string(event->name) : directory->second + "/" + event->name;
            TreeChange change;
            if (event->mask & IN_CREATE) change.kind = TreeChange::Kind::created;
            else if (event->mask & IN_DELETE) change.kind = TreeChange::Kind::removed;
            else if (event->mask & IN_MOVED_FROM) change.kind = TreeChange::Kind::renamed_from;
            else if (event->mask & IN_MOVED_TO) change.kind = TreeChange::Kind::renamed_to;
            else change.kind = TreeChange::Kind::modified;
            if ((event->mask & IN_ISDIR) && (event->mask & (IN_CREATE | IN_MOVED_TO))) add(path);
            change.path = std::move(path);
            changes.push_back(std::move(change));
        }
        return changes;
    }
    void cancel() noexcept override {
        const std::uint64_t one = 1;
        static_cast<void>(::write(cancel_, &one, sizeof(one)));
    }

private:
    int fd_, cancel_;
    std::string root_;
    std::map<int, std::string> directories_;
};

} // namespace

Result<Handle> open(std::string_view path, Access access, Create create, std::uint32_t options) {
    int flags = O_CLOEXEC;
    switch (access) {
    case Access::read: flags |= O_RDONLY; break;
    case Access::write: flags |= O_WRONLY; break;
    case Access::read_write: flags |= O_RDWR; break;
    }
    switch (create) {
    case Create::open_existing: break;
    case Create::create_new: flags |= O_CREAT | O_EXCL; break;
    case Create::open_or_create: flags |= O_CREAT; break;
    case Create::truncate_existing: flags |= O_TRUNC; break;
    case Create::create_or_truncate: flags |= O_CREAT | O_TRUNC; break;
    }
    if (options & open_append) flags |= O_APPEND;
    mode_t mode = (options & open_owner_only) ? 0600 : 0666;
    if (options & open_create_read_only) mode &= ~0222;
    const auto name = zstring(path);
    int fd = ::open(name.c_str(), flags, mode);
    if (fd < 0 && errno == EISDIR && access == Access::read) fd = ::open(name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return last_error();
    if (options & open_sequential) posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
    return handle_of(fd);
}

Result<Handle> open_null(Access access) {
    const int fd = ::open("/dev/null", (access == Access::read ? O_RDONLY : access == Access::write ? O_WRONLY : O_RDWR) | O_CLOEXEC);
    if (fd < 0) return last_error();
    return handle_of(fd);
}

Result<std::size_t> read_at(Handle file, Bytes buffer, std::uint64_t offset) {
    for (;;) {
        const auto got = pread(fd_of(file), buffer.data(), buffer.size(), static_cast<off_t>(offset));
        if (got >= 0) return static_cast<std::size_t>(got);
        if (errno != EINTR) return last_error();
    }
}

Result<std::size_t> write_at(Handle file, ConstBytes data, std::uint64_t offset) {
    for (;;) {
        const auto put = pwrite(fd_of(file), data.data(), data.size(), static_cast<off_t>(offset));
        if (put >= 0) return static_cast<std::size_t>(put);
        if (errno != EINTR) return last_error();
    }
}

Result<std::uint64_t> size(Handle file) {
    struct stat info {};
    if (fstat(fd_of(file), &info) != 0) return last_error();
    return static_cast<std::uint64_t>(info.st_size);
}

Result<std::size_t> read(Handle file, Bytes buffer) {
    for (;;) {
        const auto got = ::read(fd_of(file), buffer.data(), buffer.size());
        if (got >= 0) return static_cast<std::size_t>(got);
        if (errno != EINTR) return last_error();
    }
}

Result<std::size_t> write(Handle file, ConstBytes data) {
    for (;;) {
        const auto put = ::write(fd_of(file), data.data(), data.size());
        if (put >= 0) return static_cast<std::size_t>(put);
        if (errno != EINTR) return last_error();
    }
}

Result<std::uint64_t> seek(Handle file, std::int64_t offset, Whence whence) {
    const int how = whence == Whence::set ? SEEK_SET : whence == Whence::current ? SEEK_CUR : SEEK_END;
    const auto at = lseek(fd_of(file), static_cast<off_t>(offset), how);
    if (at < 0) return last_error();
    return static_cast<std::uint64_t>(at);
}

Status set_size(Handle file, std::uint64_t bytes) { return status_of(ftruncate(fd_of(file), static_cast<off_t>(bytes))); }
Status flush(Handle file) { return status_of(fsync(fd_of(file))); }

Status sync_directory(std::string_view path) {
    const auto name = zstring(path);
    const int fd = ::open(name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return last_error();
    const int result = fsync(fd);
    ::close(fd);
    return status_of(result);
}

bool is_character_device(Handle file) noexcept {
    struct stat info {};
    return fstat(fd_of(file), &info) == 0 && S_ISCHR(info.st_mode);
}

Kind kind(Handle file) noexcept {
    struct stat info {};
    if (fstat(fd_of(file), &info) != 0) return Kind::unknown;
    if (S_ISREG(info.st_mode) || S_ISDIR(info.st_mode)) return Kind::file;
    if (S_ISFIFO(info.st_mode) || S_ISSOCK(info.st_mode)) return Kind::pipe;
    if (S_ISCHR(info.st_mode)) return Kind::character;
    return Kind::unknown;
}

Status remove_open(Handle file) {
    char link[64];
    std::snprintf(link, sizeof(link), "/proc/self/fd/%d", fd_of(file));
    char target[PATH_MAX];
    const auto length = readlink(link, target, sizeof(target) - 1);
    if (length <= 0) return last_error();
    target[length] = 0;
    return status_of(unlink(target));
}

Status create_directory(std::string_view path, bool owner_only) {
    const auto name = zstring(path);
    return status_of(mkdir(name.c_str(), owner_only ? 0700 : 0777));
}

Status set_read_only(std::string_view path, bool read_only) {
    const auto name = zstring(path);
    struct stat info {};
    if (stat(name.c_str(), &info) != 0) return last_error();
    const mode_t mode = read_only ? (info.st_mode & ~mode_t{0222}) : (info.st_mode | S_IWUSR);
    return status_of(chmod(name.c_str(), mode & 07777));
}

Result<Pipe> create_pipe() {
    int ends[2];
    if (pipe2(ends, O_CLOEXEC) != 0) return last_error();
    fcntl(ends[1], F_SETPIPE_SZ, 65536);
    return Pipe{handle_of(ends[0]), handle_of(ends[1])};
}

Result<std::size_t> pipe_available(Handle pipe) {
    int bytes = 0;
    if (ioctl(fd_of(pipe), FIONREAD, &bytes) != 0) return last_error();
    if (bytes == 0) {
        pollfd fd{fd_of(pipe), POLLIN, 0};
        if (poll(&fd, 1, 0) == 1 && (fd.revents & POLLHUP) && !(fd.revents & POLLIN)) return Error::broken_pipe;
    }
    return static_cast<std::size_t>(bytes);
}

Result<Metadata> metadata(std::string_view path, bool follow) {
    const auto info = stat_path(path, follow);
    if (!info) return info.error();
    auto result = metadata_of(*info);
    const auto name = zstring(path);
    apply_extras(result, extras_of(-1, name.c_str(), follow));
    remember_path(result.device, result.inode, path);
    return result;
}

Result<Metadata> metadata(Handle file) {
    const auto info = stat_handle(file);
    if (!info) return info.error();
    auto result = metadata_of(*info);
    apply_extras(result, extras_of(fd_of(file), nullptr, true));
    return result;
}

Result<Identity> identity(Handle file) {
    struct stat info {};
    if (fstat(fd_of(file), &info) != 0) return last_error();
    Identity id;
    id.volume = info.st_dev;
    std::memcpy(id.id.data(), &info.st_ino, sizeof(info.st_ino));
    return id;
}

Result<std::string> path_of(std::uint64_t device, std::uint64_t inode) {
    const auto matches = [&](const std::string& path) {
        struct stat info {};
        return lstat(path.c_str(), &info) == 0 && static_cast<std::uint64_t>(info.st_dev) == device &&
               static_cast<std::uint64_t>(info.st_ino) == inode;
    };
    {
        std::lock_guard guard(g_paths_lock);
        const auto it = known_paths().find({device, inode});
        if (it != known_paths().end() && matches(it->second)) return it->second;
    }
    // The volume: the mount whose device is `device`.
    std::string root;
    for (const auto& mount : mounts()) {
        struct stat info {};
        if (stat(mount.point.c_str(), &info) == 0 && static_cast<std::uint64_t>(info.st_dev) == device) {
            root = mount.point;
            break;
        }
    }
    if (root.empty()) return Error::not_found;
    // Breadth-first, on this volume only, up to a budget of entries.
    std::vector<std::string> queue{root};
    std::size_t budget = 200000;
    for (std::size_t next = 0; next < queue.size() && budget; ++next) {
        DIR* dir = opendir(queue[next].c_str());
        if (!dir) continue;
        while (const dirent* entry = readdir(dir)) {
            if (!budget--) break;
            if (!std::strcmp(entry->d_name, ".") || !std::strcmp(entry->d_name, "..")) continue;
            const auto path = (queue[next] == "/" ? "" : queue[next]) + "/" + entry->d_name;
            if (entry->d_ino == inode && matches(path)) {
                closedir(dir);
                remember_path(device, inode, path);
                return path;
            }
            if (entry->d_type == DT_DIR) {
                struct stat info {};
                if (lstat(path.c_str(), &info) == 0 && static_cast<std::uint64_t>(info.st_dev) == device) queue.push_back(path);
            }
        }
        closedir(dir);
    }
    return Error::not_found;
}

Status lock(Handle file, std::uint64_t offset, std::uint64_t length, bool exclusive) {
    struct flock region {};
    region.l_type = exclusive ? F_WRLCK : F_RDLCK;
    region.l_whence = SEEK_SET;
    region.l_start = static_cast<off_t>(offset);
    region.l_len = static_cast<off_t>(length);
    if (fcntl(fd_of(file), F_OFD_SETLK, &region) == 0) return {};
    return errno == EAGAIN || errno == EACCES ? Error::would_block : last_error();
}

Status unlock(Handle file, std::uint64_t offset, std::uint64_t length) {
    struct flock region {};
    region.l_type = F_UNLCK;
    region.l_whence = SEEK_SET;
    region.l_start = static_cast<off_t>(offset);
    region.l_len = static_cast<off_t>(length);
    return status_of(fcntl(fd_of(file), F_OFD_SETLK, &region));
}

Status set_times(std::string_view path, std::optional<std::int64_t> accessed_ns, std::optional<std::int64_t> modified_ns) {
    return set_times_at(path, accessed_ns, modified_ns, 0);
}
Status set_link_times(std::string_view path, std::optional<std::int64_t> accessed_ns,
                      std::optional<std::int64_t> modified_ns) {
    return set_times_at(path, accessed_ns, modified_ns, AT_SYMLINK_NOFOLLOW);
}
Status set_creation_time(std::string_view path, std::int64_t created_ns, bool follow) {
    const auto name = zstring(path);
    if (::access(name.c_str(), F_OK) != 0 && follow) return last_error();
    auto extras = extras_of(-1, name.c_str(), follow);
    extras.created_ns = created_ns;
    return store_extras(name.c_str(), follow, extras);
}

Status set_hidden(std::string_view path, bool hidden, bool follow) {
    const auto name = zstring(path);
    if (::access(name.c_str(), F_OK) != 0 && follow) return last_error();
    auto extras = extras_of(-1, name.c_str(), follow);
    extras.hidden = hidden;
    return store_extras(name.c_str(), follow, extras);
}

Status rename(std::string_view from, std::string_view to, bool replace) {
    const auto source = zstring(from), target = zstring(to);
    if (renameat2(AT_FDCWD, source.c_str(), AT_FDCWD, target.c_str(), replace ? 0 : RENAME_NOREPLACE) == 0) return {};
    if (!replace && errno == EINVAL) {   // a file system without RENAME_NOREPLACE: link + unlink keeps it atomic
        if (link(source.c_str(), target.c_str()) != 0) return last_error();
        return status_of(unlink(source.c_str()));
    }
    return last_error();
}

Status access(std::string_view path, bool write) {
    const auto name = zstring(path);
    return status_of(::access(name.c_str(), write ? W_OK : F_OK));
}

bool is_console(Handle file) noexcept { return isatty(fd_of(file)) == 1; }

Result<ConsoleInputMode> console_input_mode(Handle input) {
    termios mode{};
    if (tcgetattr(fd_of(input), &mode) != 0) return last_error();
    ConsoleInputMode result;
    result.line = (mode.c_lflag & ICANON) != 0;
    result.echo = (mode.c_lflag & ECHO) != 0;
    result.signals = (mode.c_lflag & ISIG) != 0;
    result.escape_sequences = true;   // a terminal always delivers special keys as sequences
    return result;
}

Status set_console_input_mode(Handle input, const ConsoleInputMode& wanted) {
    termios mode{};
    if (tcgetattr(fd_of(input), &mode) != 0) return last_error();
    const auto set = [&](tcflag_t bit, bool on) { mode.c_lflag = on ? (mode.c_lflag | bit) : (mode.c_lflag & ~bit); };
    set(ICANON, wanted.line);
    set(ECHO, wanted.echo);
    set(ISIG, wanted.signals);
    return status_of(tcsetattr(fd_of(input), TCSANOW, &mode));
}

Status enable_console_escape_sequences(Handle) { return {}; }
Status flush_console_input(Handle input) { return status_of(tcflush(fd_of(input), TCIFLUSH)); }

Result<std::vector<std::byte>> get_attribute(std::string_view path, std::string_view name) {
    const auto file = zstring(path), key = attribute_name(name);
    for (;;) {
        const auto length = getxattr(file.c_str(), key.c_str(), nullptr, 0);
        if (length < 0) return errno == ENODATA ? Error::not_found : last_error();
        std::vector<std::byte> value(static_cast<std::size_t>(length));
        const auto got = getxattr(file.c_str(), key.c_str(), value.data(), value.size());
        if (got >= 0) {
            value.resize(static_cast<std::size_t>(got));
            return value;
        }
        if (errno != ERANGE) return errno == ENODATA ? Error::not_found : last_error();
    }
}

Result<bool> has_attribute(std::string_view path, std::string_view name) {
    const auto file = zstring(path), key = attribute_name(name);
    if (getxattr(file.c_str(), key.c_str(), nullptr, 0) >= 0) return true;
    if (errno == ENODATA) return false;
    return last_error();
}

Status set_attribute(std::string_view path, std::string_view name, ConstBytes value) {
    const auto file = zstring(path), key = attribute_name(name);
    return status_of(setxattr(file.c_str(), key.c_str(), value.data(), value.size(), 0));
}

Status remove_attribute(std::string_view path, std::string_view name) {
    const auto file = zstring(path), key = attribute_name(name);
    if (removexattr(file.c_str(), key.c_str()) == 0) return {};
    return errno == ENODATA ? Error::not_found : last_error();
}

Result<std::vector<std::string>> list_attributes(std::string_view path) {
    const auto file = zstring(path);
    for (;;) {
        const auto length = listxattr(file.c_str(), nullptr, 0);
        if (length < 0) return last_error();
        std::string names(static_cast<std::size_t>(length), '\0');
        const auto got = listxattr(file.c_str(), names.data(), names.size());
        if (got < 0) {
            if (errno == ERANGE) continue;
            return last_error();
        }
        std::vector<std::string> result;
        for (std::size_t at = 0; at < static_cast<std::size_t>(got);) {
            const std::string name(names.c_str() + at);
            at += name.size() + 1;
            if (name.rfind(kAttributePrefix, 0) == 0 && name != kExtrasAttribute)
                result.push_back(name.substr(std::strlen(kAttributePrefix)));
        }
        return result;
    }
}

Status create_symbolic_link(std::string_view link, std::string_view target, bool) {
    const auto from = zstring(link), to = zstring(target);
    return status_of(symlink(to.c_str(), from.c_str()));
}

Status create_junction(std::string_view link, std::string_view absolute_target) {
    if (absolute_target.empty() || absolute_target.front() != '/') return Error::invalid_argument;
    return create_symbolic_link(link, absolute_target, true);
}

Status create_hard_link(std::string_view link, std::string_view target) {
    const auto from = zstring(link), to = zstring(target);
    return status_of(::link(to.c_str(), from.c_str()));
}

Status remove_tree(std::string_view path) {
    const auto name = zstring(path);
    return remove_tree_at(AT_FDCWD, name.c_str());
}

Error error(const std::error_code& code) noexcept {
    if (!code) return Error::none;
    if (code.category() == std::generic_category() || code.category() == std::system_category())
        return linux_backend::error(code.value());
    return Error::other;
}

Result<Attributes> attributes(std::string_view path) {
    const auto info = stat_path(path, false);
    if (!info) return info.error();
    Attributes a;
    a.directory = S_ISDIR(info->stx_mode);
    a.symlink = S_ISLNK(info->stx_mode);
    a.read_only = (info->stx_mode & S_IWUSR) == 0;
    const auto name = zstring(path);
    a.hidden = extras_of(-1, name.c_str(), false).hidden;
    return a;
}

Result<Times> times(std::string_view path) {
    const auto info = stat_path(path, true);
    if (!info) return info.error();
    Times t;
    t.created_ns = (info->stx_mask & STATX_BTIME) ? nanoseconds(info->stx_btime) : 0;
    const auto name = zstring(path);
    if (const auto extras = extras_of(-1, name.c_str(), true); extras.created_ns) t.created_ns = extras.created_ns;
    t.modified_ns = nanoseconds(info->stx_mtime);
    t.accessed_ns = nanoseconds(info->stx_atime);
    return t;
}

Result<Volume> volume(std::string_view path) {
    const auto name = zstring(path);
    struct statvfs space {};
    if (statvfs(name.c_str(), &space) != 0) return last_error();
    char resolved[PATH_MAX];
    std::string real = realpath(name.c_str(), resolved) ? resolved : name;
    Volume v;
    std::size_t best = 0;
    for (const auto& mount : mounts()) {
        const auto& point = mount.point;
        const bool inside = real == point || (real.rfind(point, 0) == 0 && (point == "/" || real[point.size()] == '/'));
        if (inside && point.size() >= best) {
            best = point.size();
            v.root = point == "/" ? "/" : point + "/";
            v.filesystem = mount.type;
            v.name = mount.source;
            v.read_only = mount.read_only;
            v.system = point == "/";
            v.kind = mount.type == "tmpfs" || mount.type == "ramfs" ? VolumeKind::ram
                     : mount.type == "nfs" || mount.type == "nfs4" || mount.type == "cifs" || mount.type == "9p" ||
                               mount.type == "fuse.sshfs"
                         ? VolumeKind::network
                     : mount.type == "iso9660" || mount.type == "udf" ? VolumeKind::optical
                                                                       : VolumeKind::fixed;
        }
    }
    v.serial = static_cast<std::uint32_t>(space.f_fsid);
    v.max_name_length = static_cast<std::uint32_t>(space.f_namemax);
    v.total = static_cast<std::uint64_t>(space.f_blocks) * space.f_frsize;
    v.free = static_cast<std::uint64_t>(space.f_bfree) * space.f_frsize;
    v.available = static_cast<std::uint64_t>(space.f_bavail) * space.f_frsize;
    v.block_size = static_cast<std::uint32_t>(space.f_bsize);
    if (space.f_flag & ST_RDONLY) v.read_only = true;
    return v;
}

Result<std::vector<std::string>> volume_roots() {
    std::vector<std::string> roots;
    // Every mount volume() can answer for, without the kernel's pseudo file systems.
    static constexpr std::string_view pseudo[] = {"proc", "sysfs", "devtmpfs", "devpts", "cgroup", "cgroup2", "securityfs",
                                                  "pstore", "bpf", "debugfs", "tracefs", "mqueue", "hugetlbfs", "configfs",
                                                  "fusectl", "binfmt_misc", "autofs", "efivarfs", "nsfs", "rpc_pipefs"};
    for (const auto& mount : mounts()) {
        if (std::find(std::begin(pseudo), std::end(pseudo), mount.type) != std::end(pseudo)) continue;
        roots.push_back(mount.point == "/" ? "/" : mount.point + "/");
    }
    if (roots.empty()) roots.push_back("/");
    return roots;
}

Status replace(std::string_view from, std::string_view to) {
    const auto source = zstring(from), target = zstring(to);
    const int fd = ::open(source.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        fsync(fd);
        ::close(fd);
    }
    if (::rename(source.c_str(), target.c_str()) != 0) return last_error();
    const auto slash = target.rfind('/');
    const auto directory = slash == std::string::npos ? std::string(".") : slash == 0 ? std::string("/") : target.substr(0, slash);
    static_cast<void>(sync_directory(directory));
    return {};
}

Result<Handle> open_metadata(std::string_view path) {
    const auto name = zstring(path);
    const int fd = ::open(name.c_str(), O_PATH | O_CLOEXEC);
    if (fd < 0) return last_error();
    return handle_of(fd);
}

Result<Info> info(Handle file) {
    struct stat stat_info {};
    if (fstat(fd_of(file), &stat_info) != 0) return last_error();
    Info result;
    result.modified_ns = nanoseconds(stat_info.st_mtim);
    result.changed_ns = nanoseconds(stat_info.st_ctim);
    result.size = static_cast<std::uint64_t>(stat_info.st_size);
    result.links = stat_info.st_nlink;
    result.mode = stat_info.st_mode;
    result.directory = S_ISDIR(stat_info.st_mode);
    result.delete_pending = stat_info.st_nlink == 0;
    return result;
}

Result<std::string> final_path(Handle file) {
    char link[64];
    std::snprintf(link, sizeof(link), "/proc/self/fd/%d", fd_of(file));
    char target[PATH_MAX];
    const auto length = readlink(link, target, sizeof(target) - 1);
    if (length <= 0) return last_error();
    return std::string(target, static_cast<std::size_t>(length));
}

Result<Handle> watch_directory(std::string_view path) {
    const int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) return last_error();
    const auto name = zstring(path);
    if (inotify_add_watch(fd, name.c_str(), IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_ONLYDIR) < 0) {
        const auto error = last_error();
        ::close(fd);
        return error;
    }
    return handle_of(fd);
}

Result<bool> directory_changed(Handle watch) {
    alignas(inotify_event) char buffer[4096];
    bool changed = false;
    for (;;) {
        const auto got = ::read(fd_of(watch), buffer, sizeof(buffer));
        if (got > 0) {
            changed = true;
            continue;
        }
        if (got < 0 && errno != EAGAIN) return last_error();
        return changed;
    }
}

void close_watch(Handle watch) noexcept {
    if (watch) ::close(fd_of(watch));
}

Result<std::unique_ptr<TreeWatch>> watch_tree(std::string_view path) {
    const int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) return last_error();
    const int cancel = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (cancel < 0) {
        ::close(fd);
        return last_error();
    }
    auto watch = std::make_unique<InotifyTreeWatch>(fd, cancel, std::string(path));
    if (!watch->add("")) return last_error();
    return std::unique_ptr<TreeWatch>(std::move(watch));
}

Result<std::string> create_temporary(std::string_view prefix) {
    const char* directory = std::getenv("TMPDIR");
    std::string pattern = std::string(directory && *directory ? directory : "/tmp") + "/";
    pattern += prefix.substr(0, 32);
    pattern += "XXXXXX";
    const int fd = mkstemp(pattern.data());
    if (fd < 0) return last_error();
    ::close(fd);
    return pattern;
}

} // namespace alchemy::host::files
