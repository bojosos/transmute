// Linux host backend, area "sections".
//
// A section is a descriptor: a memfd, a file opened for its pages, or a
// POSIX shared memory object for named ones ("/alchemy.<uid>.<name>" in
// /dev/shm). Sections reach fx as the same descriptors (fx/host/linux/vm.cpp
// uses the same descriptor + 1 encoding).
#include "alchemy/host/linux/linux.hpp"
#include "alchemy/host/sections.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <map>
#include <mutex>

namespace alchemy::host::sections {

namespace {

using linux_backend::fd_of;
using linux_backend::handle_of;
using linux_backend::last_error;

std::mutex g_lock;
std::map<std::uintptr_t, std::size_t>& views() {
    static auto* map = new std::map<std::uintptr_t, std::size_t>;
    return *map;
}

std::string shm_name(std::string_view name) {
    std::string result = "/alchemy." + std::to_string(getuid()) + ".";
    for (const char c : name) result += c == '/' ? '_' : c;
    return result;
}

} // namespace

Result<Handle> create_section(std::uint64_t size, bool inheritable) {
    const int fd = memfd_create("alchemy-section", inheritable ? 0 : MFD_CLOEXEC);
    if (fd < 0) return last_error();
    if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
        const auto error = last_error();
        ::close(fd);
        return error;
    }
    return handle_of(fd);
}

Result<Handle> create_file_section(Handle file, bool writable) {
    // The file's own pages: a descriptor of the file, opened as the section
    // needs it.
    const auto path = "/proc/self/fd/" + std::to_string(fd_of(file));
    const int fd = ::open(path.c_str(), (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    if (fd < 0) return last_error();
    return handle_of(fd);
}

Result<std::uintptr_t> map(Handle section, std::uint64_t offset, std::size_t size, bool writable) {
    void* view = mmap(nullptr, size, PROT_READ | (writable ? PROT_WRITE : 0), MAP_SHARED, fd_of(section),
                      static_cast<off_t>(offset));
    if (view == MAP_FAILED) return last_error();
    const auto address = reinterpret_cast<std::uintptr_t>(view);
    std::lock_guard guard(g_lock);
    views()[address] = size;
    return address;
}

Status unmap(std::uintptr_t address) {
    std::size_t size = 0;
    {
        std::lock_guard guard(g_lock);
        const auto it = views().find(address);
        if (it == views().end()) return Error::invalid_argument;
        size = it->second;
        views().erase(it);
    }
    if (munmap(reinterpret_cast<void*>(address), size) != 0) return last_error();
    return {};
}

bool same(Handle first, Handle second) noexcept {
    struct stat a {}, b {};
    return fstat(fd_of(first), &a) == 0 && fstat(fd_of(second), &b) == 0 && a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

// Into this process: the section reopened (read-only when not `writable`, so
// it cannot be mapped writable). Pushing a descriptor into another process
// needs that process's help (SCM_RIGHTS over a socket), which is not done yet.
Result<std::uint64_t> duplicate_into(Handle section, std::uint32_t target_pid, bool writable) {
    if (target_pid != 0 && target_pid != static_cast<std::uint32_t>(getpid())) return Error::unsupported;
    const auto path = "/proc/self/fd/" + std::to_string(fd_of(section));
    const int fd = ::open(path.c_str(), (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    if (fd < 0) return last_error();
    return handle_of(fd).value;
}

Status flush(std::uintptr_t address, std::size_t size) {
    const auto page = address & ~std::uintptr_t{0xfff};
    if (msync(reinterpret_cast<void*>(page), size + (address - page), MS_SYNC) != 0) return last_error();
    return {};
}

Result<Named> open_named(std::string_view name, std::uint64_t size) {
    const auto object = shm_name(name);
    bool created = true;
    int fd = shm_open(object.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0 && errno == EEXIST) {
        created = false;
        fd = shm_open(object.c_str(), O_RDWR | O_CLOEXEC, 0600);
    }
    if (fd < 0) return last_error();
    if (created && ftruncate(fd, static_cast<off_t>(size)) != 0) {
        const auto error = last_error();
        ::close(fd);
        shm_unlink(object.c_str());
        return error;
    }
    return Named{handle_of(fd), created};
}

Result<Handle> open_existing(std::string_view name, bool writable) {
    const int fd = shm_open(shm_name(name).c_str(), (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC, 0600);
    if (fd < 0) return last_error();
    return handle_of(fd);
}

} // namespace alchemy::host::sections
