// linux backend of fx/host/file.hpp.
//
// Sharing modes become flock locks, enough for the cache's multi-process
// protocol (cache/merge.cpp): a handle that writes and denies other writers
// holds LOCK_EX; one that only reads but denies writers holds LOCK_SH; a
// handle that shares writing takes no lock. So an open that denies writers
// fails while a writer lives, and two exclusive writers exclude each other.
// A refused open reports a sharing violation (EWOULDBLOCK, Win32 error 32).
// Lock files are held exclusively and unlinked when their handle closes.
#include "fx/host/file.hpp"
#include "fx/host/linux/internal.hpp"
#include "fx/host/time.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <map>
#include <mutex>
#include <new>

namespace transmute::fx::host::file {

namespace {

struct Handle {
    int fd{-1};
    bool lock_file{};
    std::string path;
};

Handle* native(File file) { return static_cast<Handle*>(file); }

std::mutex g_views_lock;
std::map<const void*, std::size_t>& views() {
    static auto* map = new std::map<const void*, std::size_t>;
    return *map;
}

std::uint64_t filetime(const timespec& t) {
    return 116444736000000000ull + static_cast<std::uint64_t>(t.tv_sec) * 10000000u +
           static_cast<std::uint64_t>(t.tv_nsec) / 100u;
}

std::string path_of(const std::wstring& path) {
    auto narrow = linux_internal::narrow(path);
    std::replace(narrow.begin(), narrow.end(), '\\', '/');
    return narrow;
}

} // namespace

File open(const std::wstring& path, unsigned access, unsigned share, Create create, Flags flags) noexcept {
    try {
        const auto name = path_of(path);
        int mode = (access & kWrite) ? ((access & kRead) ? O_RDWR : O_WRONLY) : O_RDONLY;
        if (flags == Flags::lock_file && !(access & kWrite)) mode = O_RDWR;
        switch (create) {
        case Create::open_existing: break;
        case Create::open_always: mode |= O_CREAT; break;
        case Create::create_new: mode |= O_CREAT | O_EXCL; break;
        case Create::create_always: mode |= O_CREAT; break;   // truncated once the sharing check passed
        }
        const int fd = ::open(name.c_str(), mode | O_CLOEXEC, 0644);
        if (fd < 0) return nullptr;
        int lock = 0;
        if (!(share & kWrite)) lock = (access & kWrite) ? LOCK_EX : LOCK_SH;
        if (flags == Flags::lock_file) lock = LOCK_EX;
        if (lock && flock(fd, lock | LOCK_NB) != 0) {
            const int error = errno;
            ::close(fd);
            errno = error == EWOULDBLOCK ? EWOULDBLOCK : error;
            return nullptr;
        }
        if (create == Create::create_always && ftruncate(fd, 0) != 0) {
            const int error = errno;
            ::close(fd);
            errno = error;
            return nullptr;
        }
        if (flags == Flags::sequential) posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
        auto* handle = new (std::nothrow) Handle{fd, flags == Flags::lock_file, name};
        if (!handle) ::close(fd);
        return handle;
    } catch (...) {
        errno = ENOMEM;
        return nullptr;
    }
}

void close(File file) noexcept {
    auto* handle = native(file);
    if (!handle) return;
    // A lock file is held exclusively (open refuses a second holder), so its
    // holder's close is the last one.
    if (handle->lock_file) unlink(handle->path.c_str());
    ::close(handle->fd);
    delete handle;
}

bool write_all(File file, const void* data, std::size_t size) noexcept {
    const auto* p = static_cast<const std::uint8_t*>(data);
    while (size > 0) {
        const auto written = ::write(native(file)->fd, p, std::min<std::size_t>(size, 64u << 20));
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        p += written;
        size -= static_cast<std::size_t>(written);
    }
    return true;
}

bool flush_to_disk(File file) noexcept { return fsync(native(file)->fd) == 0; }

bool read_all(File file, void* data, std::size_t size) noexcept {
    auto* p = static_cast<std::uint8_t*>(data);
    for (std::size_t done = 0; done < size;) {
        const auto got = ::read(native(file)->fd, p + done, std::min<std::size_t>(size - done, std::size_t{8} << 20));
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return false;
        done += static_cast<std::size_t>(got);
    }
    return true;
}

bool seek(File file, std::uint64_t offset) noexcept {
    return lseek(native(file)->fd, static_cast<off_t>(offset), SEEK_SET) >= 0;
}

bool seek_end(File file) noexcept { return lseek(native(file)->fd, 0, SEEK_END) >= 0; }

bool size(File file, std::uint64_t& bytes) noexcept {
    struct stat info {};
    if (fstat(native(file)->fd, &info) != 0) return false;
    bytes = static_cast<std::uint64_t>(info.st_size);
    return true;
}

const std::uint8_t* map_read_only(File file, int* stage) noexcept {
    std::uint64_t bytes = 0;
    if (!size(file, bytes) || bytes == 0) {   // an empty file has no mapping, as on Windows
        if (stage) *stage = 1;
        return nullptr;
    }
    void* view = mmap(nullptr, static_cast<std::size_t>(bytes), PROT_READ, MAP_PRIVATE, native(file)->fd, 0);
    if (view == MAP_FAILED) {
        if (stage) *stage = 2;
        return nullptr;
    }
    try {
        std::lock_guard lock(g_views_lock);
        views()[view] = static_cast<std::size_t>(bytes);
    } catch (...) {
        munmap(view, static_cast<std::size_t>(bytes));
        if (stage) *stage = 2;
        return nullptr;
    }
    return static_cast<const std::uint8_t*>(view);
}

void unmap(const void* view) noexcept {
    if (!view) return;
    std::size_t bytes = 0;
    {
        std::lock_guard lock(g_views_lock);
        const auto it = views().find(view);
        if (it == views().end()) return;
        bytes = it->second;
        views().erase(it);
    }
    munmap(const_cast<void*>(view), bytes);
}

void mark_deleted(File file) noexcept { unlink(native(file)->path.c_str()); }

bool remove(const std::wstring& path) noexcept {
    try {
        return unlink(path_of(path).c_str()) == 0;
    } catch (...) {
        return false;
    }
}

bool rename_replace(const std::wstring& from, const std::wstring& to) noexcept {
    try {
        return rename(path_of(from).c_str(), path_of(to).c_str()) == 0;
    } catch (...) {
        return false;
    }
}

bool replace_atomically(const std::wstring& temp, const std::wstring& target) noexcept {
    return rename_replace(temp, target);
}

Directory create_directory(const std::wstring& path) noexcept {
    try {
        if (mkdir(path_of(path).c_str(), 0755) == 0) return Directory::created;
    } catch (...) {
        return Directory::failed;
    }
    switch (errno) {
    case EEXIST: return Directory::exists;
    case ENOENT: return Directory::parent_missing;
    default: return Directory::failed;
    }
}

std::vector<Entry> list(const std::wstring& pattern, bool) {
    std::vector<Entry> entries;
    const auto full = path_of(pattern);
    const auto slash = full.rfind('/');
    const std::string directory = slash == std::string::npos ? "." : (slash == 0 ? "/" : full.substr(0, slash));
    const std::string match = slash == std::string::npos ? full : full.substr(slash + 1);
    DIR* dir = opendir(directory.c_str());
    if (!dir) return entries;
    while (const dirent* entry = readdir(dir)) {
        if (fnmatch(match.c_str(), entry->d_name, FNM_PERIOD) != 0) continue;
        struct stat info {};
        if (fstatat(dirfd(dir), entry->d_name, &info, 0) != 0) continue;
        entries.push_back({linux_internal::widen(entry->d_name), S_ISDIR(info.st_mode),
                           static_cast<std::uint64_t>(info.st_size), filetime(info.st_mtim)});
    }
    closedir(dir);
    return entries;
}

bool attributes(const std::wstring& path, std::uint64_t* bytes, std::uint64_t* write_time) noexcept {
    try {
        struct stat info {};
        if (stat(path_of(path).c_str(), &info) != 0) return false;
        if (bytes) *bytes = static_cast<std::uint64_t>(info.st_size);
        if (write_time) *write_time = filetime(info.st_mtim);
        return true;
    } catch (...) {
        return false;
    }
}

std::wstring full_path(const std::wstring& path) {
    auto name = path_of(path);
    if (name.empty()) return {};
    if (name[0] != '/') {
        char cwd[4096];
        if (!getcwd(cwd, sizeof(cwd))) return {};
        name = std::string(cwd) + "/" + name;
    }
    // Collapse "." and ".." without resolving links (GetFullPathNameW).
    std::vector<std::string> parts;
    for (std::size_t at = 0; at <= name.size();) {
        auto end = name.find('/', at);
        if (end == std::string::npos) end = name.size();
        const auto part = name.substr(at, end - at);
        if (part == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        at = end + 1;
    }
    std::string result;
    for (const auto& part : parts) result += "/" + part;
    if (result.empty()) result = "/";
    if (name.back() == '/' && result != "/") result += '/';
    return linux_internal::widen(result);
}

std::FILE* open_stdio(const std::wstring& path, const char* mode) noexcept {
    try {
        return std::fopen(path_of(path).c_str(), mode);
    } catch (...) {
        return nullptr;
    }
}

std::wstring temp_directory() {
    const char* temp = std::getenv("TMPDIR");
    std::string directory = temp && *temp ? temp : "/tmp";
    if (directory.back() != '/') directory += '/';
    return linux_internal::widen(directory);
}

} // namespace transmute::fx::host::file
