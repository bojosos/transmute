// linux backend of fx/host/vm.hpp.
//
// Linux has no VirtualQuery and no reservation objects, so this backend keeps
// its own table of the address space it manages: runs of pages with equal
// attributes, each tagged with the allocation (reservation, placeholder or
// view) it belongs to. query() answers from the table; addresses outside it
// (the host's own stacks, heap and images) are read from /proc/self/maps.
//
// - A reservation is a PROT_NONE, MAP_NORESERVE anonymous mapping. Committing
//   is mprotect (the kernel supplies zero pages on first touch); decommitting
//   maps fresh PROT_NONE pages over the range, which drops the contents.
// - Reservations without an address are 64 KiB aligned, as on Windows.
// - kGuard pages are PROT_NONE; the first fault on one restores its
//   protection (take_guard_page) and is reported as a guard-page fault.
// - A placeholder is a PROT_NONE reservation; a view replaces it with
//   mmap(MAP_FIXED | MAP_SHARED) of a memfd (or MAP_PRIVATE for write-copy),
//   and unmapping maps PROT_NONE back, so the address stays taken. Splitting
//   and coalescing only change the table.
// - Sections are memfd descriptors; a Section holds the descriptor + 1 (the
//   host API's Handle encoding, so Alchemy's sections are fx's).
#include "fx/host/vm.hpp"
#include "fx/host/linux/internal.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>
#include <new>
#include <vector>

namespace transmute::fx::host::vm {

namespace {

constexpr std::uint64_t kPage = 0x1000;
constexpr std::uint64_t kGranule = 0x10000;
constexpr std::uint64_t kUserTop = 0x00007ffffffff000ull;

std::uint64_t down(std::uint64_t value, std::uint64_t alignment) { return value & ~(alignment - 1); }
std::uint64_t up(std::uint64_t value, std::uint64_t alignment) { return (value + alignment - 1) & ~(alignment - 1); }
void* pointer(std::uint64_t address) { return reinterpret_cast<void*>(address); }
std::uint64_t address_of(const void* p) { return reinterpret_cast<std::uint64_t>(p); }

enum class Kind : std::uint8_t { reservation, placeholder, view };

struct Run {
    std::uint64_t end;
    std::uint64_t allocation_base;
    Protection allocation_protect;
    Protection protect;   // 0 while reserved
    State state;
    Kind kind;
};

std::mutex g_lock;
std::map<std::uint64_t, Run>& runs() {
    static auto* map = new std::map<std::uint64_t, Run>;   // never destroyed: used until _exit
    return *map;
}

int native_protection(Protection protection) {
    if (protection & kGuard) return PROT_NONE;
    switch (protection & 0xff) {
    case kReadOnly: return PROT_READ;
    case kReadWrite:
    case kWriteCopy: return PROT_READ | PROT_WRITE;
    case kExecute: return PROT_EXEC;
    case kExecuteRead: return PROT_READ | PROT_EXEC;
    case kExecuteReadWrite:
    case kExecuteWriteCopy: return PROT_READ | PROT_WRITE | PROT_EXEC;
    default: return PROT_NONE;
    }
}

int descriptor(Section section) { return static_cast<int>(reinterpret_cast<std::intptr_t>(section)) - 1; }
Section section_of(int fd) { return reinterpret_cast<Section>(static_cast<std::intptr_t>(fd) + 1); }

// ---- the table (g_lock held) -------------------------------------------------------

// The run containing `address`, or end().
std::map<std::uint64_t, Run>::iterator find(std::uint64_t address) {
    auto& table = runs();
    auto it = table.upper_bound(address);
    if (it == table.begin()) return table.end();
    --it;
    return address < it->second.end ? it : table.end();
}

// Makes `address` a run boundary (when it lies inside a run).
void split_at(std::uint64_t address) {
    const auto it = find(address);
    if (it == runs().end() || it->first == address) return;
    Run tail = it->second;
    it->second.end = address;
    runs().emplace(address, tail);
}

// True if [begin, end) is covered by runs of one allocation.
bool one_allocation(std::uint64_t begin, std::uint64_t end, std::uint64_t* base = nullptr) {
    auto it = find(begin);
    if (it == runs().end()) return false;
    const auto allocation = it->second.allocation_base;
    for (std::uint64_t at = begin; at < end;) {
        if (it == runs().end() || it->first > at || it->second.allocation_base != allocation) return false;
        at = it->second.end;
        ++it;
    }
    if (base) *base = allocation;
    return true;
}

// The extent of the allocation starting at `base`.
std::uint64_t allocation_end(std::uint64_t base) {
    auto it = runs().find(base);
    std::uint64_t end = base;
    for (; it != runs().end() && it->second.allocation_base == base && it->first == end; ++it) end = it->second.end;
    return end;
}

// Sets [begin, end) (boundaries already split) to one run state.
template <typename F>
void update(std::uint64_t begin, std::uint64_t end, F&& change) {
    split_at(begin);
    split_at(end);
    for (auto it = runs().find(begin); it != runs().end() && it->first < end; ++it) change(it->second);
}

void erase(std::uint64_t begin, std::uint64_t end) {
    split_at(begin);
    split_at(end);
    auto& table = runs();
    table.erase(table.lower_bound(begin), table.lower_bound(end));
}

void insert(std::uint64_t begin, std::uint64_t end, Run run) {
    run.end = end;
    runs()[begin] = run;
}

// Joins neighbouring runs with equal attributes (keeps the table small).
void merge_around(std::uint64_t begin, std::uint64_t end) {
    auto& table = runs();
    auto it = table.lower_bound(begin);
    if (it != table.begin()) --it;
    while (it != table.end() && it->first <= end) {
        auto next = std::next(it);
        if (next == table.end()) break;
        const Run& a = it->second;
        const Run& b = next->second;
        if (a.end == next->first && a.allocation_base == b.allocation_base && a.protect == b.protect &&
            a.state == b.state && a.kind == b.kind && a.allocation_protect == b.allocation_protect) {
            it->second.end = b.end;
            table.erase(next);
        } else {
            it = next;
        }
    }
}

// ---- address space ---------------------------------------------------------------------

void* map_reserved(std::uint64_t address, std::size_t size, bool fixed_replace) {
    int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
    if (address) flags |= fixed_replace ? MAP_FIXED : MAP_FIXED_NOREPLACE;
    void* got = mmap(pointer(address), size, PROT_NONE, flags, -1, 0);
    if (got == MAP_FAILED) return nullptr;
    if (address && address_of(got) != address) {   // a kernel without MAP_FIXED_NOREPLACE
        munmap(got, size);
        errno = EEXIST;
        return nullptr;
    }
    return got;
}

// `size` bytes of PROT_NONE address space at a 64 KiB boundary.
void* map_granular(std::size_t size) {
    const std::size_t padded = size + kGranule;
    auto* raw = static_cast<std::uint8_t*>(map_reserved(0, padded, false));
    if (!raw) return nullptr;
    const auto start = up(address_of(raw), kGranule);
    const auto head = start - address_of(raw);
    if (head) munmap(raw, head);
    const auto tail = padded - head - size;
    if (tail) munmap(pointer(start + size), tail);
    return pointer(start);
}

// The mapped ranges of the process, sorted (from /proc/self/maps; no
// allocation, so it also serves the fault handler).
template <typename F>
void each_mapping(F&& visit) {
    const int fd = ::open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    char buffer[8192];
    std::size_t have = 0;
    bool done = false;
    while (!done) {
        const auto got = ::read(fd, buffer + have, sizeof(buffer) - have);
        if (got <= 0) done = true;
        else have += static_cast<std::size_t>(got);
        std::size_t start = 0;
        for (std::size_t i = 0; i < have; ++i) {
            if (buffer[i] != '\n') continue;
            buffer[i] = 0;
            const char* line = buffer + start;
            char* rest = nullptr;
            const auto begin = std::strtoull(line, &rest, 16);
            const auto end = std::strtoull(rest + 1, &rest, 16);
            const char* perms = rest + 1;
            // "rwxp offset dev inode path": a path ("/..." or "[...]") after the inode.
            const char* path = std::strchr(perms, '/');
            if (!visit(begin, end, perms, path)) {
                ::close(fd);
                return;
            }
            start = i + 1;
        }
        if (start == 0 && have == sizeof(buffer)) break;   // a line longer than the buffer
        std::memmove(buffer, buffer + start, have - start);
        have -= start;
    }
    ::close(fd);
}

// A free 64 KiB aligned range of `size` bytes at or above `lowest`, or 0.
std::uint64_t find_free(std::uint64_t lowest, std::uint64_t size) {
    std::uint64_t candidate = up(std::max<std::uint64_t>(lowest, kGranule), kGranule);
    std::uint64_t found = 0;
    each_mapping([&](std::uint64_t begin, std::uint64_t end, const char*, const char*) {
        if (end <= candidate) return true;
        if (begin >= candidate + size) {
            found = candidate;
            return false;
        }
        candidate = up(end, kGranule);
        return true;
    });
    if (!found && candidate + size <= kUserTop) found = candidate;
    return found;
}

void* reserve_at_or_above(std::uint64_t lowest, std::size_t size) {
    for (int attempt = 0; attempt < 16; ++attempt) {
        const auto at = find_free(lowest, size);
        if (!at) return nullptr;
        if (void* got = map_reserved(at, size, false)) return got;
        if (errno != EEXIST) return nullptr;
        lowest = at + kGranule;   // taken meanwhile: look further up
    }
    return nullptr;
}

bool query_host(std::uint64_t address, RegionInfo& info) {
    const auto page = down(address, kPage);
    bool found = false;
    std::uint64_t next = kUserTop;
    each_mapping([&](std::uint64_t begin, std::uint64_t end, const char* perms, const char* path) {
        if (address >= end) return true;
        if (address < begin) {
            next = begin;
            return false;
        }
        Protection protect = kNoAccess;
        const bool r = perms[0] == 'r', w = perms[1] == 'w', x = perms[2] == 'x';
        if (x) protect = w ? kExecuteReadWrite : r ? kExecuteRead : kExecute;
        else if (r) protect = w ? kReadWrite : kReadOnly;
        info.base = page;
        info.allocation_base = begin;
        info.size = end - page;
        info.protect = protect;
        info.allocation_protect = protect;
        info.state = State::committed;
        info.type = path && x ? Type::image : path ? Type::mapped : Type::private_memory;
        found = true;
        return false;
    });
    if (!found) {
        info = {};
        info.base = page;
        info.size = next - page;
        info.state = State::free;
    }
    return true;
}

bool take_guard(std::uint64_t address) noexcept {
    std::lock_guard lock(g_lock);
    const auto it = find(address);
    if (it == runs().end() || !(it->second.protect & kGuard) || it->second.state != State::committed) return false;
    const auto page = down(address, kPage);
    const Protection base = it->second.protect & ~kGuard;
    if (mprotect(pointer(page), kPage, native_protection(base)) != 0) return false;
    update(page, page + kPage, [&](Run& run) { run.protect = base; });
    merge_around(page, page + kPage);
    return true;
}

} // namespace

bool query(const void* address, RegionInfo& info) noexcept {
    const auto at = address_of(address);
    if (at >= kUserTop) return false;
    {
        std::lock_guard lock(g_lock);
        const auto it = find(at);
        if (it != runs().end()) {
            const Run& run = it->second;
            info.base = down(at, kPage);
            info.size = run.end - info.base;
            info.allocation_base = run.allocation_base;
            info.allocation_protect = run.allocation_protect;
            info.protect = run.state == State::committed ? run.protect : 0;
            info.state = run.state;
            info.type = run.kind == Kind::view ? Type::mapped : Type::private_memory;
            // Equal neighbours in the same allocation extend the answer.
            for (auto next = std::next(it); next != runs().end() && next->first == run.end + 0 &&
                                            next->second.allocation_base == run.allocation_base &&
                                            next->second.state == run.state && next->second.protect == run.protect;
                 ++next)
                info.size = next->second.end - info.base;
            return true;
        }
    }
    return query_host(at, info);
}

void* allocate(void* address, std::size_t size, Protection protect) noexcept {
    void* base = reserve(address, size, protect);
    if (!base) return nullptr;
    const auto begin = address ? down(address_of(address), kPage) : address_of(base);
    const auto length = address ? size + (address_of(address) - begin) : size;
    if (!commit(pointer(begin), length, protect)) {
        release(base);
        return nullptr;
    }
    return address ? pointer(begin) : base;
}

void* reserve(void* address, std::size_t size, Protection protect, bool) noexcept {
    if (size == 0) {
        errno = EINVAL;
        return nullptr;
    }
    std::uint64_t base = 0, length = 0;
    void* got = nullptr;
    try {
        std::lock_guard lock(g_lock);
        if (address) {
            base = down(address_of(address), kGranule);
            length = up(address_of(address) + size, kPage) - base;
            got = map_reserved(base, length, false);
        } else {
            length = up(size, kPage);
            got = map_granular(length);
            base = address_of(got);
        }
        if (!got) return nullptr;
        insert(base, base + length, Run{0, base, protect, 0, State::reserved, Kind::reservation});
    } catch (...) {
        if (got) munmap(got, length);
        errno = ENOMEM;
        return nullptr;
    }
    return got;
}

void* reserve_above(std::uint64_t lowest, std::size_t size) noexcept {
    try {
        std::lock_guard lock(g_lock);
        const auto length = up(size, kPage);
        void* got = reserve_at_or_above(lowest, length);
        if (!got) return nullptr;
        insert(address_of(got), address_of(got) + length,
               Run{0, address_of(got), kNoAccess, 0, State::reserved, Kind::reservation});
        return got;
    } catch (...) {
        errno = ENOMEM;
        return nullptr;
    }
}

void* commit(void* address, std::size_t size, Protection protect) noexcept {
    if (!address) return allocate(nullptr, size, protect);
    const auto begin = down(address_of(address), kPage);
    const auto end = up(address_of(address) + std::max<std::size_t>(size, 1), kPage);
    try {
        std::lock_guard lock(g_lock);
        std::uint64_t base = 0;
        if (!one_allocation(begin, end, &base) || find(begin)->second.kind != Kind::reservation) {
            errno = EINVAL;
            return nullptr;
        }
        if (mprotect(pointer(begin), end - begin, native_protection(protect)) != 0) return nullptr;
        update(begin, end, [&](Run& run) {
            run.state = State::committed;
            run.protect = protect;
        });
        merge_around(begin, end);
        return pointer(begin);
    } catch (...) {
        errno = ENOMEM;
        return nullptr;
    }
}

bool decommit(void* address, std::size_t size) noexcept {
    const auto begin = down(address_of(address), kPage);
    try {
        std::lock_guard lock(g_lock);
        std::uint64_t end = size ? up(address_of(address) + size, kPage) : 0;
        if (!end) {   // size 0: from `address` to the end of its reservation
            const auto it = find(begin);
            if (it == runs().end()) return false;
            end = allocation_end(it->second.allocation_base);
        }
        if (!one_allocation(begin, end) || find(begin)->second.kind != Kind::reservation) {
            errno = EINVAL;
            return false;
        }
        if (!map_reserved(begin, end - begin, true)) return false;
        update(begin, end, [&](Run& run) {
            run.state = State::reserved;
            run.protect = 0;
        });
        merge_around(begin, end);
        return true;
    } catch (...) {
        errno = ENOMEM;
        return false;
    }
}

bool release(void* address) noexcept {
    const auto base = address_of(address);
    try {
        std::lock_guard lock(g_lock);
        const auto it = runs().find(base);
        // A reservation or a whole placeholder (VirtualFree MEM_RELEASE takes both).
        if (it == runs().end() || it->second.allocation_base != base || it->second.kind == Kind::view) {
            errno = EINVAL;
            return false;
        }
        const auto end = allocation_end(base);
        if (munmap(address, end - base) != 0) return false;
        erase(base, end);
        return true;
    } catch (...) {
        errno = ENOMEM;
        return false;
    }
}

bool protect(void* address, std::size_t size, Protection protection, Protection* old) noexcept {
    const auto begin = down(address_of(address), kPage);
    const auto end = up(address_of(address) + std::max<std::size_t>(size, 1), kPage);
    try {
        std::lock_guard lock(g_lock);
        if (!one_allocation(begin, end)) {
            errno = EINVAL;
            return false;
        }
        Protection previous = 0;
        bool committed = true;
        for (auto it = find(begin); it != runs().end() && it->first < end; ++it) {
            if (it == find(begin)) previous = it->second.protect;
            if (it->second.state != State::committed) committed = false;
        }
        if (!committed) {   // VirtualProtect refuses reserved pages
            errno = EACCES;
            return false;
        }
        if (mprotect(pointer(begin), end - begin, native_protection(protection)) != 0) return false;
        update(begin, end, [&](Run& run) { run.protect = protection; });
        merge_around(begin, end);
        if (old) *old = previous;
        return true;
    } catch (...) {
        errno = ENOMEM;
        return false;
    }
}

void discard(void* address, std::size_t size) noexcept {
    const auto begin = up(address_of(address), kPage);
    const auto end = down(address_of(address) + size, kPage);
    if (end > begin && madvise(pointer(begin), end - begin, MADV_FREE) != 0)
        madvise(pointer(begin), end - begin, MADV_DONTNEED);
}

void flush_instruction_cache(const void*, std::size_t) noexcept {
    // x86-64 keeps instruction fetch coherent with stores.
}

void prefetch(const void* address, std::size_t size) noexcept {
    const auto begin = down(address_of(address), kPage);
    madvise(pointer(begin), up(address_of(address) + size, kPage) - begin, MADV_WILLNEED);
}

// ---- placeholders and sections ---------------------------------------------------------

void* reserve_placeholder(void* address, std::size_t size) noexcept {
    try {
        std::lock_guard lock(g_lock);
        const auto length = up(size, kPage);
        void* got = address ? map_reserved(address_of(address), length, false) : map_granular(length);
        if (!got) return nullptr;
        insert(address_of(got), address_of(got) + length,
               Run{0, address_of(got), kNoAccess, 0, State::reserved, Kind::placeholder});
        return got;
    } catch (...) {
        errno = ENOMEM;
        return nullptr;
    }
}

bool split_placeholder(void* address, std::size_t size) noexcept {
    const auto base = address_of(address);
    try {
        std::lock_guard lock(g_lock);
        const auto it = runs().find(base);
        if (it == runs().end() || it->second.kind != Kind::placeholder || it->second.allocation_base != base) return false;
        const auto end = allocation_end(base);
        const auto cut = base + up(size, kPage);
        if (cut >= end) return false;
        update(cut, end, [&](Run& run) { run.allocation_base = cut; });
        return true;
    } catch (...) {
        return false;
    }
}

bool coalesce_placeholders(void* address, std::size_t size) noexcept {
    const auto begin = address_of(address);
    const auto end = begin + up(size, kPage);
    try {
        std::lock_guard lock(g_lock);
        for (auto it = find(begin); it != runs().end() && it->first < end; ++it)
            if (it->second.kind != Kind::placeholder) return false;
        if (find(begin) == runs().end() || find(end - 1) == runs().end()) return false;
        update(begin, end, [&](Run& run) { run.allocation_base = begin; });
        merge_around(begin, end);
        return true;
    } catch (...) {
        return false;
    }
}

void* reserve_placeholder_above(std::uint64_t lowest, std::size_t size) noexcept {
    try {
        std::lock_guard lock(g_lock);
        const auto length = up(size, kPage);
        void* got = reserve_at_or_above(lowest, length);
        if (!got) return nullptr;
        insert(address_of(got), address_of(got) + length,
               Run{0, address_of(got), kNoAccess, 0, State::reserved, Kind::placeholder});
        return got;
    } catch (...) {
        errno = ENOMEM;
        return nullptr;
    }
}

bool replace_placeholder_reserved(void* address, std::size_t size) noexcept {
    const auto begin = address_of(address);
    const auto end = begin + up(size, kPage);
    try {
        std::lock_guard lock(g_lock);
        const auto it = runs().find(begin);
        if (it == runs().end() || it->second.kind != Kind::placeholder || it->second.allocation_base != begin ||
            allocation_end(begin) != end)
            return false;
        update(begin, end, [&](Run& run) {
            run.kind = Kind::reservation;
            run.allocation_protect = kNoAccess;
        });
        return true;
    } catch (...) {
        return false;
    }
}

bool release_to_placeholder(void* address, std::size_t size) noexcept {
    const auto begin = address_of(address);
    const auto end = begin + up(size, kPage);
    try {
        std::lock_guard lock(g_lock);
        const auto it = runs().find(begin);
        if (it == runs().end() || it->second.kind != Kind::reservation || it->second.allocation_base != begin ||
            allocation_end(begin) != end)
            return false;
        if (!map_reserved(begin, end - begin, true)) return false;
        update(begin, end, [&](Run& run) {
            run.kind = Kind::placeholder;
            run.state = State::reserved;
            run.protect = 0;
        });
        merge_around(begin, end);
        return true;
    } catch (...) {
        return false;
    }
}

Section create_section(std::uint64_t size) noexcept {
    const int fd = memfd_create("fx-section", MFD_CLOEXEC);
    if (fd < 0) return nullptr;
    if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
        ::close(fd);
        return nullptr;
    }
    return section_of(fd);
}

Section duplicate_section(Section section) noexcept {
    const int fd = fcntl(descriptor(section), F_DUPFD_CLOEXEC, 0);
    return fd < 0 ? nullptr : section_of(fd);
}

void close_section(Section section) noexcept {
    if (section) ::close(descriptor(section));
}

void* map_view_into_placeholder(Section section, void* address, std::uint64_t offset, std::size_t size,
                                Protection protect) noexcept {
    const auto begin = address_of(address);
    const auto end = begin + up(size, kPage);
    try {
        std::lock_guard lock(g_lock);
        const auto it = runs().find(begin);
        if (it == runs().end() || it->second.kind != Kind::placeholder || it->second.allocation_base != begin ||
            allocation_end(begin) != end) {
            errno = EINVAL;
            return nullptr;
        }
        const bool copy = (protect & 0xff) == kWriteCopy || (protect & 0xff) == kExecuteWriteCopy;
        void* got = mmap(address, end - begin, native_protection(protect), MAP_FIXED | (copy ? MAP_PRIVATE : MAP_SHARED),
                         descriptor(section), static_cast<off_t>(offset));
        if (got == MAP_FAILED) return nullptr;
        update(begin, end, [&](Run& run) {
            run.kind = Kind::view;
            run.state = State::committed;
            run.protect = protect;
            run.allocation_protect = protect;
        });
        merge_around(begin, end);
        return got;
    } catch (...) {
        errno = ENOMEM;
        return nullptr;
    }
}

bool unmap_view_to_placeholder(void* address) noexcept {
    const auto begin = address_of(address);
    try {
        std::lock_guard lock(g_lock);
        const auto it = runs().find(begin);
        if (it == runs().end() || it->second.kind != Kind::view || it->second.allocation_base != begin) return false;
        const auto end = allocation_end(begin);
        if (!map_reserved(begin, end - begin, true)) return false;
        update(begin, end, [&](Run& run) {
            run.kind = Kind::placeholder;
            run.state = State::reserved;
            run.protect = 0;
            run.allocation_protect = kNoAccess;
        });
        merge_around(begin, end);
        return true;
    } catch (...) {
        return false;
    }
}

// ---- private heap ------------------------------------------------------------------------
// Segments of 64 MiB reserved through this file (so query() reports their
// allocation base, which the guest heap's bookkeeping reads) and committed
// read/write up front (the kernel backs pages on first touch). Blocks carry a
// 16-byte header; freed blocks go to per-size free lists: 16-byte steps up to
// 1 KiB, then powers of two.

namespace {

constexpr std::size_t kSegment = std::size_t{64} << 20;
constexpr std::uint64_t kMagic = 0x6678686561702121ull;   // "fxheap!!"
constexpr unsigned kSmallClasses = 64;                    // 16 .. 1024
constexpr unsigned kClasses = kSmallClasses + 20;

struct Header {
    std::uint64_t magic;
    std::uint64_t size;   // the block's capacity (its class size)
};
static_assert(sizeof(Header) == 16);

struct HeapState {
    std::mutex lock;
    std::vector<std::pair<std::uint8_t*, std::uint8_t*>> segments;   // [begin, end)
    std::uint8_t* cursor{};
    std::uint8_t* limit{};
    void* free_lists[kClasses]{};
};

unsigned class_of(std::size_t size, std::size_t& capacity) {
    if (size <= 1024) {
        capacity = up(std::max<std::size_t>(size, 1), 16);
        return static_cast<unsigned>(capacity / 16 - 1);
    }
    capacity = 2048;
    unsigned index = kSmallClasses;
    while (capacity < size) {
        capacity *= 2;
        ++index;
    }
    return index < kClasses ? index : kClasses;
}

} // namespace

Heap create_heap() noexcept { return new (std::nothrow) HeapState; }

void* heap_allocate(Heap heap, std::size_t size) noexcept {
    auto& state = *static_cast<HeapState*>(heap);
    std::size_t capacity = 0;
    const unsigned index = class_of(size, capacity);
    if (index >= kClasses || capacity + sizeof(Header) > kSegment) return nullptr;
    std::lock_guard lock(state.lock);
    std::uint8_t* block = nullptr;
    if (void* reused = state.free_lists[index]) {
        state.free_lists[index] = *static_cast<void**>(reused);
        block = static_cast<std::uint8_t*>(reused);
    } else {
        if (state.cursor + sizeof(Header) + capacity > state.limit) {
            void* segment = allocate(nullptr, kSegment, kReadWrite);
            if (!segment) return nullptr;
            try {
                state.segments.emplace_back(static_cast<std::uint8_t*>(segment),
                                            static_cast<std::uint8_t*>(segment) + kSegment);
            } catch (...) {
                release(segment);
                return nullptr;
            }
            state.cursor = static_cast<std::uint8_t*>(segment);
            state.limit = state.cursor + kSegment;
        }
        auto* header = reinterpret_cast<Header*>(state.cursor);
        header->magic = kMagic;
        header->size = capacity;
        block = state.cursor + sizeof(Header);
        state.cursor += sizeof(Header) + capacity;
    }
    std::memset(block, 0, capacity);
    return block;
}

bool heap_free(Heap heap, void* block) noexcept {
    auto& state = *static_cast<HeapState*>(heap);
    auto* bytes = static_cast<std::uint8_t*>(block);
    std::lock_guard lock(state.lock);
    const bool inside = std::any_of(state.segments.begin(), state.segments.end(), [&](const auto& segment) {
        return bytes >= segment.first + sizeof(Header) && bytes < segment.second;
    });
    if (!inside || (address_of(bytes) & 15) != 0) return false;
    auto* header = reinterpret_cast<Header*>(bytes - sizeof(Header));
    if (header->magic != kMagic) return false;
    std::size_t capacity = 0;
    const unsigned index = class_of(static_cast<std::size_t>(header->size), capacity);
    if (index >= kClasses || capacity != header->size) return false;
    *reinterpret_cast<void**>(bytes) = state.free_lists[index];
    state.free_lists[index] = bytes;
    return true;
}

} // namespace transmute::fx::host::vm

namespace transmute::fx::host::linux_internal {

bool take_guard_page(std::uint64_t address) noexcept { return vm::take_guard(address); }

} // namespace transmute::fx::host::linux_internal
