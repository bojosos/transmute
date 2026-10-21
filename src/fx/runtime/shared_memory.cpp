// Shared guest memory (owner: runtime): section-backed guest ranges that can
// be mapped at more than one guest address, the engine side of Darwin's
// vm_remap with writable sources (process.hpp: map_shared_guest_memory,
// alias_guest_memory, unmap_shared_guest_memory).
//
// Address space (host/vm.hpp, "placeholders and sections"): fx reserves the
// 64 KiB granules a shared range touches as placeholders (win32: VirtualAlloc2
// MEM_RESERVE_PLACEHOLDER), splits them at 4 KiB page boundaries and replaces
// the pieces with views of pagefile-backed sections (MapViewOfFile3
// MEM_REPLACE_PLACEHOLDER). Unmapping a view turns it back into a
// placeholder (UnmapViewOfFile2 MEM_PRESERVE_PLACEHOLDER), so
// no other allocation can take the address meanwhile; adjacent placeholders
// are coalesced and a reservation that is entirely placeholder again is
// released. Views are mapped PAGE_READWRITE and then protected down, so a
// later protection change may raise them again. A section lives as long as
// any view maps it: an alias keeps the pages after the original is unmapped.
//
// Copy-on-write file views (map_file_copy_guest_memory, Mach-O segments): a
// read-only file section viewed PAGE_WRITECOPY, so the file's pages are shared
// with every process viewing the file until a write makes a private copy.
// Such a section is never aliased or duplicated, and a view of it is never
// cut: the written pages exist only in that view.
//
// Translated code: a page with translations is write-protected at every
// address that maps it (regions.cpp asks shared_memory_aliases), and a write
// fault at any of them invalidates the translations of all. Creating or
// removing an alias invalidates the translations of both ranges.

#include "fx/runtime/runtime_internal.hpp"
#include "fx/runtime/shared_memory.hpp"
#include "fx/host/vm.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace transmute::fx::rt {

std::atomic<bool> g_shared_aliases{false};

namespace {

namespace vm = host::vm;

constexpr std::uint64_t kPage = 0x1000;
constexpr std::uint64_t kGranule = 0x10000;

struct Section {
    vm::Section handle{};
    std::uint64_t size{};
    bool writable{true};                // false: a read-only section, viewed read-only
    bool copy_on_write{};               // a read-only file viewed copy-on-write (Mach-O segments)
    std::vector<std::uint64_t> views;   // bases of the views mapping it
    ~Section() { vm::close_section(handle); }
};

struct View {
    std::uint64_t size{};
    std::shared_ptr<Section> section;
    std::uint64_t offset{};
    std::uint64_t reservation{};
    std::uint64_t bytes{};   // mapped bytes (a file view may end inside its last page)
};

struct Piece {   // a placeholder
    std::uint64_t end{};
    std::uint64_t reservation{};
};

struct State {
    host::RwLock lock;
    std::map<std::uint64_t, View> views;               // by base
    std::map<std::uint64_t, Piece> placeholders;       // by begin
    std::map<std::uint64_t, std::uint64_t> reservations;   // base -> size
};

State& S() {
    static State* const s = new State();   // never destroyed
    return *s;
}

using Exclusive = host::ExclusiveLock;
using Shared = host::SharedLock;

[[noreturn]] void fail(const char* what, std::uint64_t address) {
    char text[256];
    std::snprintf(text, sizeof(text), "fx shared memory: %s at 0x%llx (error %lu)", what,
                  static_cast<unsigned long long>(address), static_cast<unsigned long>(host::last_error()));
    throw std::runtime_error(text);
}

void* host(std::uint64_t address) { return reinterpret_cast<void*>(address); }

std::uint64_t reservation_of(State& s, std::uint64_t address) {
    auto it = s.reservations.upper_bound(address);
    if (it == s.reservations.begin()) return 0;
    --it;
    return address - it->first < it->second ? it->first : 0;
}

// Every granule of [begin, end) is in an fx reservation afterwards.
void reserve(State& s, std::uint64_t begin, std::uint64_t end) {
    std::uint64_t g = begin & ~(kGranule - 1);
    const std::uint64_t last = (end + kGranule - 1) & ~(kGranule - 1);
    while (g < last) {
        if (reservation_of(s, g)) {
            g += kGranule;
            continue;
        }
        std::uint64_t run = g;
        while (run < last && !reservation_of(s, run)) run += kGranule;
        void* got = vm::reserve_placeholder(host(g), static_cast<std::size_t>(run - g));
        if (reinterpret_cast<std::uint64_t>(got) != g) {
            if (got) vm::release(got);
            fail("address range is not free", g);
        }
        s.reservations.emplace(g, run - g);
        s.placeholders.emplace(g, Piece{run, g});
        g = run;
    }
}

// Splits placeholders so that `at` starts one (if it lies inside one).
void split_at(State& s, std::uint64_t at) {
    auto it = s.placeholders.upper_bound(at);
    if (it == s.placeholders.begin()) return;
    --it;
    if (it->first == at || at >= it->second.end) return;
    const std::uint64_t begin = it->first;
    const Piece piece = it->second;
    if (!vm::split_placeholder(host(begin), static_cast<std::size_t>(at - begin)))
        fail("cannot split a placeholder", at);
    it->second.end = at;
    s.placeholders.emplace(at, Piece{piece.end, piece.reservation});
}

// [begin, end) must be placeholder only; splits it at `begin`, `end` and every
// `unit` boundary (0: none) and returns the pieces.
std::vector<std::pair<std::uint64_t, std::uint64_t>> carve(State& s, std::uint64_t begin, std::uint64_t end,
                                                           std::uint64_t unit) {
    for (auto at = begin; at < end;) {
        auto it = s.placeholders.upper_bound(at);
        if (it == s.placeholders.begin()) fail("range is not free fx shared-memory space", at);
        --it;
        if (at >= it->second.end) fail("range is already mapped", at);
        at = it->second.end;
    }
    split_at(s, begin);
    split_at(s, end);
    if (unit)
        for (auto at = begin + unit; at < end; at += unit) split_at(s, at);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> pieces;
    for (auto it = s.placeholders.lower_bound(begin); it != s.placeholders.end() && it->first < end; ++it)
        pieces.emplace_back(it->first, it->second.end);
    return pieces;
}

// Maps [begin, end) (one placeholder) as a view of `section` at `offset`.
// `bytes` (default: the whole piece) may end inside the last page when the
// section ends there (a file whose size is not a page multiple).
void map_piece(State& s, std::uint64_t begin, std::uint64_t end, const std::shared_ptr<Section>& section,
               std::uint64_t offset, vm::Protection protect, std::uint64_t bytes = 0) {
    auto it = s.placeholders.find(begin);
    if (it == s.placeholders.end() || it->second.end != end) fail("internal: placeholder mismatch", begin);
    const std::uint64_t reservation = it->second.reservation;
    const vm::Protection initial = section->copy_on_write ? vm::kWriteCopy
                                   : section->writable    ? vm::kReadWrite
                                                          : vm::kReadOnly;
    if (section->copy_on_write && protect == vm::kReadWrite) protect = vm::kWriteCopy;
    if (!section->writable && protect == vm::kReadWrite) fail("a read-only section cannot be mapped writable", begin);
    void* view = vm::map_view_into_placeholder(section->handle, host(begin), offset,
                                               static_cast<std::size_t>(bytes ? bytes : end - begin), initial);
    if (reinterpret_cast<std::uint64_t>(view) != begin) fail("cannot map a view", begin);
    s.placeholders.erase(it);
    if (protect != initial) vm::protect(view, static_cast<std::size_t>(end - begin), protect);
    section->views.push_back(begin);
    if (section->views.size() > 1) g_shared_aliases.store(true, std::memory_order_release);
    s.views.emplace(begin, View{end - begin, section, offset, reservation, bytes ? bytes : end - begin});
}

// Coalesces the placeholder at `begin` with its neighbours of the same
// reservation; releases the reservation once it is one placeholder again.
void coalesce(State& s, std::uint64_t begin) {
    auto it = s.placeholders.find(begin);
    if (it == s.placeholders.end()) return;
    const std::uint64_t reservation = it->second.reservation;
    std::uint64_t first = begin, last = it->second.end;
    if (it != s.placeholders.begin()) {
        auto prev = std::prev(it);
        if (prev->second.end == begin && prev->second.reservation == reservation) first = prev->first;
    }
    auto next = s.placeholders.find(last);
    if (next != s.placeholders.end() && next->second.reservation == reservation) last = next->second.end;
    if (first != begin || last != it->second.end) {
        if (!vm::coalesce_placeholders(host(first), static_cast<std::size_t>(last - first))) return;
        s.placeholders.erase(s.placeholders.lower_bound(first), s.placeholders.lower_bound(last));
        s.placeholders.emplace(first, Piece{last, reservation});
    }
    const auto r = s.reservations.find(reservation);
    if (r != s.reservations.end() && first == r->first && last == r->first + r->second) {
        if (vm::release(host(first))) {
            s.placeholders.erase(first);
            s.reservations.erase(r);
        }
    }
}

void unmap_view(State& s, std::map<std::uint64_t, View>::iterator it) {
    const std::uint64_t begin = it->first, end = begin + it->second.size;
    if (!vm::unmap_view_to_placeholder(host(begin))) fail("cannot unmap a view", begin);
    auto& views = it->second.section->views;
    views.erase(std::remove(views.begin(), views.end(), begin), views.end());
    s.placeholders.emplace(begin, Piece{end, it->second.reservation});
    s.views.erase(it);
    coalesce(s, begin);
}

// Views covering [begin, end): whole views, separated only by fx placeholders
// (the unmapped tail of a file mapping), or throws.
std::vector<std::uint64_t> whole_views(State& s, std::uint64_t begin, std::uint64_t end) {
    std::vector<std::uint64_t> out;
    std::uint64_t at = begin;
    while (at < end) {
        if (const auto it = s.views.find(at); it != s.views.end()) {
            if (at + it->second.size > end) fail("range is not made of whole shared-memory views", at);
            out.push_back(at);
            at += it->second.size;
            continue;
        }
        auto piece = s.placeholders.upper_bound(at);
        if (piece == s.placeholders.begin()) fail("range is not made of whole shared-memory views", at);
        --piece;
        if (at >= piece->second.end) fail("range is not made of whole shared-memory views", at);
        at = piece->second.end;
        // A placeholder may run past `end` (the rest of a reservation); a view may not start inside the range's end.
        if (at > end) at = end;
    }
    if (out.empty()) fail("range maps no shared-memory view", begin);
    return out;
}

void check_range(std::uint64_t base, std::uint64_t size) {
    if (size == 0 || (base & (kPage - 1)) || (size & (kPage - 1)) || base + size < base)
        throw std::invalid_argument("fx shared memory: range must be non-empty and 4 KiB aligned");
}

} // namespace

void shared_memory_aliases(std::uint64_t page, std::vector<std::uint64_t>& out) {
    State& s = S();
    Shared lock(s.lock);
    auto it = s.views.upper_bound(page);
    if (it == s.views.begin()) return;
    --it;
    if (page - it->first >= it->second.size) return;
    const std::uint64_t offset = it->second.offset + (page - it->first);
    for (const std::uint64_t other : it->second.section->views) {
        if (other == it->first) continue;
        const View& w = s.views.at(other);
        if (offset >= w.offset && offset - w.offset < w.size) out.push_back(other + (offset - w.offset));
    }
}

void shared_memory_aliases(std::uint64_t begin, std::uint64_t end, std::vector<std::uint64_t>& out) {
    if (end <= begin) return;
    begin &= ~(kPage - 1);
    State& s = S();
    Shared lock(s.lock);
    auto it = s.views.upper_bound(begin);
    if (it != s.views.begin()) --it;
    for (; it != s.views.end() && it->first < end; ++it) {
        const std::uint64_t base = it->first, limit = base + it->second.size;
        if (limit <= begin) continue;
        const std::uint64_t lo = it->second.offset + (std::max(begin, base) - base);
        const std::uint64_t hi = it->second.offset + (std::min(end, limit) - base);
        for (const std::uint64_t other : it->second.section->views) {
            if (other == base) continue;
            const View& w = s.views.at(other);
            const std::uint64_t from = std::max(lo, w.offset), to = std::min(hi, w.offset + w.size);
            for (std::uint64_t offset = from; offset < to; offset += kPage) out.push_back(other + (offset - w.offset));
        }
    }
}

} // namespace transmute::fx::rt

namespace transmute::fx {

void map_shared_guest_memory(std::uint64_t base, std::uint64_t size, std::uint64_t granule, std::uint32_t protect) {
    rt::check_range(base, size);
    if (granule && ((granule & (rt::kPage - 1)) || size % granule))
        throw std::invalid_argument("fx shared memory: granule must be a page multiple dividing the size");
    rt::ensure_initialized();
    auto section = std::make_shared<rt::Section>();
    section->size = size;
    section->handle = rt::vm::create_section(size);
    if (!section->handle) rt::fail("cannot create a section", base);
    {
        rt::State& s = rt::S();
        rt::Exclusive lock(s.lock);
        rt::reserve(s, base, base + size);
        for (const auto& [b, e] : rt::carve(s, base, base + size, granule))
            rt::map_piece(s, b, e, section, b - base, protect);
    }
    invalidate_guest_code(base, size);   // translations of whatever was mapped here before
}

void alias_guest_memory(std::uint64_t destination, std::uint64_t source, std::uint64_t size, std::uint32_t protect) {
    rt::check_range(destination, size);
    rt::check_range(source, size);
    if (destination < source + size && source < destination + size)
        throw std::invalid_argument("fx shared memory: alias overlaps its source");
    rt::ensure_initialized();
    {
        rt::State& s = rt::S();
        rt::Exclusive lock(s.lock);
        // What the source maps, piece by piece (views may start before it).
        struct Part {
            std::uint64_t begin, end;
            std::shared_ptr<rt::Section> section;
            std::uint64_t offset;
        };
        std::vector<Part> parts;
        for (std::uint64_t at = source; at < source + size;) {
            auto it = s.views.upper_bound(at);
            if (it == s.views.begin()) rt::fail("alias source is not shared memory", at);
            --it;
            if (at - it->first >= it->second.size) rt::fail("alias source is not shared memory", at);
            // Its written pages are private copies a second view would not show.
            if (it->second.section->copy_on_write) rt::fail("alias source is a copy-on-write file view", at);
            const std::uint64_t end = std::min(source + size, it->first + it->second.size);
            parts.push_back({at, end, it->second.section, it->second.offset + (at - it->first)});
            at = end;
        }
        // The destination replaces whole views it covers (Mach VM_FLAGS_OVERWRITE).
        std::vector<std::uint64_t> replaced;
        auto it = s.views.upper_bound(destination);
        if (it != s.views.begin()) --it;
        for (; it != s.views.end() && it->first < destination + size; ++it)
            if (it->first + it->second.size > destination) {
                if (it->second.section->copy_on_write) rt::fail("alias destination is a copy-on-write file view", it->first);
                replaced.push_back(it->first);
            }
        if (!replaced.empty()) {
            const std::uint64_t first = std::min(replaced.front(), destination);
            if (first < destination || s.views.at(replaced.back()).size + replaced.back() > destination + size)
                rt::fail("alias destination splits a shared-memory view", first);
            for (const std::uint64_t b : replaced) rt::unmap_view(s, s.views.find(b));
        }
        rt::reserve(s, destination, destination + size);
        for (const Part& part : parts) {
            const std::uint64_t d = destination + (part.begin - source);
            for (const auto& [b, e] : rt::carve(s, d, d + (part.end - part.begin), 0))
                rt::map_piece(s, b, e, part.section, part.offset + (b - d), protect);
        }
    }
    invalidate_guest_code(destination, size);
    invalidate_guest_code(source, size);   // its pages are now writable through the alias
}

namespace {
std::uint64_t base_round(std::uint64_t bytes) { return (bytes + rt::kPage - 1) & ~(rt::kPage - 1); }
} // namespace

std::uint64_t map_section_guest_memory(std::uint64_t base, std::uint64_t size, void* section_handle, std::uint64_t offset,
                                       std::uint64_t view_size, bool writable_section, std::uint32_t protect) {
    if (size == 0 || (size & (rt::kPage - 1)) || (base & (rt::kPage - 1)) || base + size < base)
        throw std::invalid_argument("fx shared memory: range must be non-empty and 4 KiB aligned");
    if (view_size == 0 || view_size > size || (offset & (rt::kPage - 1)))
        throw std::invalid_argument("fx shared memory: the section view must be non-empty and inside the range, at a 4 KiB aligned offset");
    const std::uint64_t view_end = base_round(view_size);
    if (!writable_section && protect == rt::vm::kReadWrite)
        throw std::invalid_argument("fx shared memory: a read-only section cannot be mapped writable");
    rt::ensure_initialized();
    auto section = std::make_shared<rt::Section>();
    section->size = view_size;
    section->writable = writable_section;
    section->handle = rt::vm::duplicate_section(section_handle);
    if (!section->handle) rt::fail("cannot duplicate a section handle", base);
    {
        rt::State& s = rt::S();
        rt::Exclusive lock(s.lock);
        if (base == 0) {
            // Free address space of the host's choosing, owned by fx from the start.
            const std::uint64_t extent = (size + rt::kGranule - 1) & ~(rt::kGranule - 1);
            void* got = rt::vm::reserve_placeholder(nullptr, static_cast<std::size_t>(extent));
            if (!got) rt::fail("no free address space for a section view", 0);
            base = reinterpret_cast<std::uint64_t>(got);
            s.reservations.emplace(base, extent);
            s.placeholders.emplace(base, rt::Piece{base + extent, base});
        } else {
            rt::reserve(s, base, base + size);
        }
        try {
            for (const auto& [b, e] : rt::carve(s, base, base + view_end, 0))
                rt::map_piece(s, b, e, section, offset + (b - base), protect, std::min(e, base + view_size) - b);
        } catch (...) {
            // Nothing of this call may stay mapped or reserved.
            auto it = s.views.lower_bound(base);
            while (it != s.views.end() && it->first < base + view_end) {
                const auto next = std::next(it);
                rt::unmap_view(s, it);
                it = next;
            }
            if (auto piece = s.placeholders.find(base); piece != s.placeholders.end()) rt::coalesce(s, base);
            throw;
        }
    }
    invalidate_guest_code(base, size);
    return base;
}

void map_section_guest_pages(std::uint64_t base, std::uint64_t size, void* section_handle, std::uint64_t offset,
                             std::uint64_t granule, bool writable_section, std::uint32_t protect) {
    rt::check_range(base, size);
    if (granule && ((granule & (rt::kPage - 1)) || size % granule))
        throw std::invalid_argument("fx shared memory: granule must be a page multiple dividing the size");
    if (offset & (rt::kPage - 1)) throw std::invalid_argument("fx shared memory: the section offset must be 4 KiB aligned");
    if (!writable_section && protect == rt::vm::kReadWrite)
        throw std::invalid_argument("fx shared memory: a read-only section cannot be mapped writable");
    rt::ensure_initialized();
    auto section = std::make_shared<rt::Section>();
    section->size = offset + size;   // bytes known to exist from the section's start
    section->writable = writable_section;
    section->handle = rt::vm::duplicate_section(section_handle);
    if (!section->handle) rt::fail("cannot duplicate a section handle", base);
    {
        rt::State& s = rt::S();
        rt::Exclusive lock(s.lock);
        rt::reserve(s, base, base + size);
        try {
            for (const auto& [b, e] : rt::carve(s, base, base + size, granule))
                rt::map_piece(s, b, e, section, offset + (b - base), protect);
        } catch (...) {
            auto it = s.views.lower_bound(base);
            while (it != s.views.end() && it->first < base + size) {
                const auto next = std::next(it);
                rt::unmap_view(s, it);
                it = next;
            }
            throw;
        }
    }
    invalidate_guest_code(base, size);
}

void map_file_copy_guest_memory(std::uint64_t base, std::uint64_t size, void* section_handle, std::uint64_t offset,
                                std::uint64_t file_bytes) {
    rt::check_range(base, size);
    if (offset & (rt::kPage - 1)) throw std::invalid_argument("fx shared memory: the file offset must be 4 KiB aligned");
    if (file_bytes == 0 || file_bytes > size)
        throw std::invalid_argument("fx shared memory: the file bytes must be non-empty and inside the range");
    rt::ensure_initialized();
    const std::uint64_t file_end = base + base_round(file_bytes);
    auto file = std::make_shared<rt::Section>();
    file->size = offset + file_bytes;
    file->writable = false;
    file->copy_on_write = true;
    file->handle = rt::vm::duplicate_section(section_handle);
    if (!file->handle) rt::fail("cannot duplicate a section handle", base);
    std::shared_ptr<rt::Section> zero;
    if (file_end < base + size) {
        zero = std::make_shared<rt::Section>();
        zero->size = base + size - file_end;
        zero->handle = rt::vm::create_section(zero->size);
        if (!zero->handle) rt::fail("cannot create a section", file_end);
    }
    {
        rt::State& s = rt::S();
        rt::Exclusive lock(s.lock);
        rt::reserve(s, base, base + size);
        try {
            for (const auto& [b, e] : rt::carve(s, base, file_end, 0))
                rt::map_piece(s, b, e, file, offset + (b - base), rt::vm::kWriteCopy, std::min(e, base + file_bytes) - b);
            if (zero)
                for (const auto& [b, e] : rt::carve(s, file_end, base + size, 0))
                    rt::map_piece(s, b, e, zero, b - file_end, rt::vm::kReadWrite);
        } catch (...) {
            auto it = s.views.lower_bound(base);
            while (it != s.views.end() && it->first < base + size) {
                const auto next = std::next(it);
                rt::unmap_view(s, it);
                it = next;
            }
            if (auto piece = s.placeholders.find(base); piece != s.placeholders.end()) rt::coalesce(s, base);
            throw;
        }
    }
    // The rest of the last file page reads as zeros (the file's next bytes
    // otherwise). Only a page that holds some becomes a private copy.
    auto* tail = reinterpret_cast<std::uint8_t*>(base + file_bytes);
    const auto* tail_end = reinterpret_cast<const std::uint8_t*>(file_end);
    if (std::any_of(static_cast<const std::uint8_t*>(tail), tail_end, [](std::uint8_t byte) { return byte != 0; }))
        std::fill(tail, const_cast<std::uint8_t*>(tail_end), std::uint8_t{0});
    invalidate_guest_code(base, size);
}

void* duplicate_shared_guest_section(std::uint64_t address, std::uint64_t* offset, std::uint64_t* size,
                                     bool* writable) noexcept {
    rt::State& s = rt::S();
    rt::Shared lock(s.lock);
    auto it = s.views.upper_bound(address);
    if (it == s.views.begin()) return nullptr;
    --it;
    if (address - it->first >= it->second.size || it->second.section->copy_on_write) return nullptr;
    void* copy = rt::vm::duplicate_section(it->second.section->handle);
    if (!copy) return nullptr;
    if (offset) *offset = it->second.offset + (address - it->first);
    if (size) *size = it->second.section->size;
    if (writable) *writable = it->second.section->writable;
    return copy;
}

void unmap_shared_guest_range(std::uint64_t base, std::uint64_t size) {
    rt::check_range(base, size);
    rt::ensure_initialized();
    const std::uint64_t end = base + size;
    {
        rt::State& s = rt::S();
        rt::Exclusive lock(s.lock);
        std::vector<std::uint64_t> overlapping;
        auto it = s.views.upper_bound(base);
        if (it != s.views.begin()) --it;
        for (; it != s.views.end() && it->first < end; ++it)
            if (it->first + it->second.size > base) overlapping.push_back(it->first);
        for (const std::uint64_t begin : overlapping) {
            const auto found = s.views.find(begin);
            const rt::View view = found->second;
            const std::uint64_t view_end = begin + view.size;
            if (begin >= base && view_end <= end) {
                rt::unmap_view(s, found);
                continue;
            }
            // Mapping the parts again would drop the pages written there.
            if (view.section->copy_on_write) rt::fail("partial unmap of a copy-on-write file view", begin);
            // A view cannot be cut in place: record the protection of the
            // parts that stay, unmap the view to its placeholder, split that
            // and map the parts again from the same section (the pages live
            // in the section, so their contents stay).
            std::vector<std::pair<std::uint64_t, std::pair<std::uint64_t, rt::vm::Protection>>> runs;   // begin -> (end, protect)
            for (std::uint64_t at = begin; at < view_end;) {
                rt::vm::RegionInfo info;
                if (!rt::vm::query(rt::host(at), info)) rt::fail("cannot query a view", at);
                const std::uint64_t run_end = std::min(view_end, info.base + info.size);
                runs.push_back({at, {run_end, info.protect}});
                at = run_end;
            }
            if (!rt::vm::unmap_view_to_placeholder(rt::host(begin))) rt::fail("cannot unmap a view", begin);
            auto& views = view.section->views;
            views.erase(std::remove(views.begin(), views.end(), begin), views.end());
            s.views.erase(found);
            s.placeholders.emplace(begin, rt::Piece{view_end, view.reservation});
            const std::uint64_t cut_begin = std::max(begin, base), cut_end = std::min(view_end, end);
            const auto restore = [&](std::uint64_t from, std::uint64_t to) {
                for (const auto& [run_begin, rest] : runs) {
                    const std::uint64_t a = std::max(from, run_begin), b = std::min(to, rest.first);
                    if (a >= b || rest.second == 0) continue;
                    rt::vm::protect(rt::host(a), static_cast<std::size_t>(b - a), rest.second);
                }
            };
            const rt::vm::Protection initial = view.section->writable ? rt::vm::kReadWrite : rt::vm::kReadOnly;
            if (begin < cut_begin) {
                rt::split_at(s, cut_begin);
                rt::map_piece(s, begin, cut_begin, view.section, view.offset, initial, std::min(cut_begin - begin, view.bytes));
                restore(begin, cut_begin);
            }
            if (cut_end < view_end) {
                rt::split_at(s, cut_end);
                rt::map_piece(s, cut_end, view_end, view.section, view.offset + (cut_end - begin), initial,
                              view.bytes - (cut_end - begin));
                restore(cut_end, view_end);
            }
            rt::coalesce(s, cut_begin);
        }
    }
    invalidate_guest_code(base, size);
}

void unmap_shared_guest_memory(std::uint64_t base, std::uint64_t size) {
    rt::check_range(base, size);
    rt::ensure_initialized();
    {
        rt::State& s = rt::S();
        rt::Exclusive lock(s.lock);
        for (const std::uint64_t b : rt::whole_views(s, base, base + size)) rt::unmap_view(s, s.views.find(b));
    }
    invalidate_guest_code(base, size);
}

bool is_shared_guest_memory(std::uint64_t address) noexcept {
    rt::State& s = rt::S();
    rt::Shared lock(s.lock);
    auto it = s.views.upper_bound(address);
    if (it == s.views.begin()) return false;
    --it;
    return address - it->first < it->second.size;
}

} // namespace transmute::fx
