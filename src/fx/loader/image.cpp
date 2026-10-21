#include "fx/loader/image.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace transmute::fx::loader {
namespace {

constexpr std::uint64_t kPage = 4096;

std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

class FileHandle {
public:
    explicit FileHandle(HANDLE h) : h_(h) {}
    ~FileHandle() {
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
    }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    [[nodiscard]] HANDLE get() const { return h_; }
    [[nodiscard]] bool valid() const { return h_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE h_;
};

std::string win32_error(const char* what, DWORD code = GetLastError()) {
    return std::string(what) + " (Win32 error " + std::to_string(code) + ")";
}

// Reads exactly `size` bytes at `offset` (synchronous handle, explicit offset).
bool read_at(HANDLE file, std::uint64_t offset, void* buffer, std::uint64_t size) {
    auto* out = static_cast<std::uint8_t*>(buffer);
    while (size != 0) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::uint64_t>(size, 1ull << 30));
        OVERLAPPED overlapped{};
        overlapped.Offset = static_cast<DWORD>(offset);
        overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD got = 0;
        if (!ReadFile(file, out, chunk, &got, &overlapped) || got == 0) return false;
        out += got;
        offset += got;
        size -= got;
    }
    return true;
}

struct ReadChunk {
    std::uint64_t offset;
    std::uint8_t* destination;
    std::uint64_t size;
};

constexpr std::uint64_t kChunk = 4ull << 20;
constexpr std::uint64_t kParallelThreshold = 24ull << 20;

// Persistent reader threads for large images (Blender's 100 MB blender.exe):
// the cost of reading an image is mostly demand-zero faults on the
// destination, which scale across cores. The threads are created once,
// outside the loader lock (a thread created while the host loader lock is
// held cannot start until it is released), and the caller never waits for a
// worker that has not joined the current job, so a pool whose threads cannot
// run degrades to a single-threaded read instead of a deadlock.
class ReaderPool {
public:
    explicit ReaderPool(unsigned workers) {
        for (unsigned i = 0; i < workers; ++i) {
            std::thread([this] { worker(); }).detach();
        }
    }

    bool run(HANDLE file, const std::wstring& path, const std::vector<ReadChunk>& chunks) {
        std::lock_guard serial(job_lock_);
        Job job{&chunks, &path};
        {
            std::lock_guard guard(lock_);
            job_ = &job;
            ++generation_;
        }
        wake_.notify_all();
        process(job, file);
        {
            std::unique_lock guard(lock_);
            job_ = nullptr;   // closed: late workers skip it
            done_.wait(guard, [&] { return job.participants == 0; });
        }
        return job.ok.load();
    }

private:
    struct Job {
        const std::vector<ReadChunk>* chunks;
        const std::wstring* path;
        std::atomic<std::size_t> next{0};
        std::atomic<bool> ok{true};
        unsigned participants = 0;   // guarded by lock_
    };

    static void process(Job& job, HANDLE handle) {
        const auto& chunks = *job.chunks;
        for (;;) {
            const std::size_t i = job.next.fetch_add(1, std::memory_order_relaxed);
            if (i >= chunks.size() || !job.ok.load(std::memory_order_relaxed)) return;
            if (!read_at(handle, chunks[i].offset, chunks[i].destination, chunks[i].size)) job.ok = false;
        }
    }

    void worker() {
        std::uint64_t seen = 0;
        for (;;) {
            Job* job = nullptr;
            {
                std::unique_lock guard(lock_);
                wake_.wait(guard, [&] { return generation_ != seen; });
                seen = generation_;
                job = job_;
                if (!job) continue;
                ++job->participants;
            }
            HANDLE h = CreateFileW(job->path->c_str(), GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                process(*job, h);
                CloseHandle(h);
            }
            {
                std::lock_guard guard(lock_);
                --job->participants;
            }
            done_.notify_all();
        }
    }

    std::mutex job_lock_;
    std::mutex lock_;
    std::condition_variable wake_;
    std::condition_variable done_;
    Job* job_ = nullptr;
    std::uint64_t generation_ = 0;
};

std::atomic<ReaderPool*> g_readers{nullptr};

ReaderPool* readers() {
    if (ReaderPool* pool = g_readers.load(std::memory_order_acquire)) return pool;
    // Creating threads while holding the host loader lock is safe only if we
    // do not wait for them; ReaderPool never does, but prefer creating it
    // outside (prepare_image_readers).
    prepare_image_readers();
    return g_readers.load(std::memory_order_acquire);
}

bool read_chunks(HANDLE file, const std::wstring& path, const std::vector<ReadChunk>& chunks,
                 std::uint64_t total) {
    ReaderPool* pool = total >= kParallelThreshold && chunks.size() >= 2 ? readers() : nullptr;
    if (pool) return pool->run(file, path, chunks);
    for (const auto& c : chunks) {
        if (!read_at(file, c.offset, c.destination, c.size)) return false;
    }
    return true;
}

// TRANSMUTE_FX_IMAGE_VIEWS=0 copies every image (the pre-view behaviour).
bool image_views_enabled() {
    static const bool enabled = [] {
        char value[8] = {};
        return !(GetEnvironmentVariableA("TRANSMUTE_FX_IMAGE_VIEWS", value, sizeof(value)) > 0 && value[0] == '0');
    }();
    return enabled;
}

std::uint64_t header_image_base(const std::uint8_t* image, const PeHeaders& headers) {
    std::uint64_t value;
    std::memcpy(&value, image + headers.nt_offset + 24 + 24, 8);   // OptionalHeader.ImageBase
    return value;
}

// Maps the file as an image section (SEC_IMAGE_NO_EXECUTE: no image-load
// callbacks, no code integrity, never executable). The kernel lays out the
// sections, applies the base relocations for the base it picks for the image
// (its per-boot ASLR base for relocatable images) and shares those pages
// between processes; nothing is read up front and pages fault in on first
// touch, as for a native image. The view is placed at that base when it is
// free, so no page needs user-mode relocation. `kernel_base` is the base the
// view's contents are relocated for.
std::uint8_t* map_image_view(HANDLE file, const PeHeaders& headers, const MapOptions& options,
                             std::uint64_t& kernel_base) {
    HANDLE section = CreateFileMappingW(file, nullptr, PAGE_READONLY | SEC_IMAGE_NO_EXECUTE, 0, 0, nullptr);
    if (!section) return nullptr;
    auto* view = static_cast<std::uint8_t*>(MapViewOfFile(section, FILE_MAP_READ, 0, 0, 0));
    if (view) {
        kernel_base = header_image_base(view, headers);
        const bool avoid = options.force_relocation && kernel_base == headers.image_base;
        if (!avoid && kernel_base != reinterpret_cast<std::uint64_t>(view)) {
            if (auto* at = static_cast<std::uint8_t*>(
                    MapViewOfFileEx(section, FILE_MAP_READ, 0, 0, 0, reinterpret_cast<void*>(kernel_base)))) {
                UnmapViewOfFile(view);
                view = at;
            }
        }
        if (options.force_relocation && reinterpret_cast<std::uint64_t>(view) == headers.image_base) {
            UnmapViewOfFile(view);   // extremely unlikely; the copy path picks another base
            view = nullptr;
        }
    }
    CloseHandle(section);
    if (view) {
        // The kernel validated the layout; make sure the view covers the image.
        MEMORY_BASIC_INFORMATION mbi{};
        std::uint64_t covered = 0;
        while (covered < headers.size_of_image &&
               VirtualQuery(view + covered, &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.AllocationBase == view &&
               mbi.Type == MEM_IMAGE) {
            covered = static_cast<std::uint64_t>(static_cast<std::uint8_t*>(mbi.BaseAddress) - view) + mbi.RegionSize;
        }
        if (covered < headers.size_of_image) {
            UnmapViewOfFile(view);
            view = nullptr;
        }
    }
    return view;
}

void finish_image(MappedImage& out, PeHeaders& headers, const std::vector<std::uint8_t>& head) {
    out.image_hash = compute_image_hash(out.file_size, out.last_write_time, headers, head.data(), head.size());
    std::uint64_t text_begin = UINT64_MAX, text_end = 0;
    for (const auto& s : headers.sections) {
        if ((s.characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) continue;
        const std::uint64_t extent = s.virtual_size ? s.virtual_size : s.raw_size;
        if (extent == 0) continue;
        text_begin = std::min<std::uint64_t>(text_begin, s.rva);
        text_end = std::max<std::uint64_t>(text_end, static_cast<std::uint64_t>(s.rva) + extent);
    }
    if (text_end != 0) {
        out.text_begin = reinterpret_cast<std::uint64_t>(out.base) + text_begin;
        out.text_end = reinterpret_cast<std::uint64_t>(out.base) + text_end;
    }
    out.headers = std::move(headers);
}

std::uint64_t mix64(std::uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x;
}

} // namespace

void prepare_image_readers() {
    static std::mutex create_lock;
    std::lock_guard guard(create_lock);
    if (g_readers.load(std::memory_order_acquire)) return;
    const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
    const unsigned workers = std::min(3u, hardware > 1 ? hardware - 1 : 0u);
    if (workers == 0) return;
    g_readers.store(new ReaderPool(workers), std::memory_order_release);   // lives for the process
}

std::uint64_t compute_image_hash(std::uint64_t file_size, std::uint64_t last_write_time,
                                 const PeHeaders& headers, const std::uint8_t* first_bytes,
                                 std::size_t first_size) noexcept {
    // FNV-1a over the header bytes, combined with the scalar identity fields.
    std::uint64_t h = 0xcbf29ce484222325ull;
    const std::size_t n = std::min<std::size_t>(first_size, 4096);
    for (std::size_t i = 0; i < n; ++i) {
        h ^= first_bytes[i];
        h *= 0x100000001b3ull;
    }
    std::uint64_t r = mix64(h ^ 0x6678'6c6f'6164'6572ull);
    r = mix64(r ^ file_size);
    r = mix64(r ^ last_write_time);
    r = mix64(r ^ headers.timestamp);
    r = mix64(r ^ headers.size_of_image);
    r = mix64(r ^ headers.checksum);
    return r;
}

std::uint16_t probe_pe_file(const std::wstring& path, bool* is_dll) {
    FileHandle file(CreateFileW(path.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid()) return 0;
    std::uint8_t buffer[1024];
    DWORD got = 0;
    if (!ReadFile(file.get(), buffer, sizeof(buffer), &got, nullptr)) return 0;
    std::uint16_t machine = pe_machine({buffer, got}, is_dll);
    if (machine != 0) return machine;
    // e_lfanew beyond the first KiB (rare): read the NT header directly.
    if (got >= 0x40) {
        std::uint32_t nt = 0;
        std::memcpy(&nt, buffer + 0x3c, 4);
        std::uint8_t nt_bytes[24];
        if (nt > 0 && nt < (1u << 20) && read_at(file.get(), nt, nt_bytes, sizeof(nt_bytes))) {
            std::uint32_t signature = 0;
            std::memcpy(&signature, nt_bytes, 4);
            if (buffer[0] == 'M' && buffer[1] == 'Z' && signature == 0x00004550) {
                std::uint16_t characteristics = 0;
                std::memcpy(&machine, nt_bytes + 4, 2);
                std::memcpy(&characteristics, nt_bytes + 22, 2);
                if (is_dll) *is_dll = (characteristics & 0x2000u) != 0;
                return machine;
            }
        }
    }
    return 0;
}

bool read_image_headers(const std::wstring& path, PeHeaders& out, std::string& error) {
    FileHandle file(CreateFileW(path.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid()) {
        error = win32_error("cannot open image");
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size)) {
        error = win32_error("cannot query image file");
        return false;
    }
    std::vector<std::uint8_t> head(static_cast<std::size_t>(std::min<std::int64_t>(size.QuadPart, 8192)));
    if (head.empty() || !read_at(file.get(), 0, head.data(), head.size())) {
        error = "cannot read image headers";
        return false;
    }
    return parse_pe_headers(head, out, error);
}

unsigned long section_protection(std::uint32_t c) noexcept {
    const bool exec = (c & IMAGE_SCN_MEM_EXECUTE) != 0;
    const bool write = (c & IMAGE_SCN_MEM_WRITE) != 0;
    const bool read = (c & IMAGE_SCN_MEM_READ) != 0;
    if (write) return PAGE_READWRITE;
    if (exec || read) return PAGE_READONLY;   // guest code is translated, never executed
    return PAGE_NOACCESS;
}

bool map_image(const std::wstring& path, const MapOptions& options, MappedImage& out,
               std::string& error) {
    out = MappedImage{};
    FileHandle file(CreateFileW(path.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file.valid()) {
        error = win32_error("cannot open image");
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file.get(), &info)) {
        error = win32_error("cannot query image file");
        return false;
    }
    const std::uint64_t file_size =
        (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    const std::uint64_t last_write =
        (static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) |
        info.ftLastWriteTime.dwLowDateTime;

    // Headers: one read of the first 4 KiB (more only if SizeOfHeaders is larger).
    std::vector<std::uint8_t> head(static_cast<std::size_t>(std::min<std::uint64_t>(file_size, 4096)));
    if (head.empty() || !read_at(file.get(), 0, head.data(), head.size())) {
        error = "cannot read image headers";
        return false;
    }
    PeHeaders headers;
    if (!parse_pe_headers(head, headers, error)) {
        // The section table may extend past 4 KiB: retry with the full header size.
        std::uint32_t nt = 0;
        if (head.size() >= 0x40) std::memcpy(&nt, head.data() + 0x3c, 4);
        const std::uint64_t want = std::min<std::uint64_t>(file_size, std::max<std::uint64_t>(nt + 0x2000ull, 4096));
        if (want <= head.size()) return false;
        head.resize(static_cast<std::size_t>(want));
        if (!read_at(file.get(), 0, head.data(), head.size()) ||
            !parse_pe_headers(head, headers, error)) {
            return false;
        }
    }
    if (options.required_machine != 0 && headers.machine != options.required_machine) {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "unexpected machine 0x%04x", headers.machine);
        error = buffer;
        return false;
    }
    if (!validate_image_layout(headers, file_size, error)) return false;
    if (headers.size_of_headers > head.size()) {
        head.resize(headers.size_of_headers);
        if (!read_at(file.get(), 0, head.data(), head.size())) {
            error = "cannot read image headers";
            return false;
        }
    }

    const std::uint64_t size = align_up(headers.size_of_image, kPage);
    const PeDirectory relocs = headers.dir(dir_basereloc);
    if (options.image_view && image_views_enabled() && headers.section_alignment >= kPage) {
        std::uint64_t kernel_base = 0;
        if (std::uint8_t* view = map_image_view(file.get(), headers, options, kernel_base)) {
            // Writable (copy-on-write) for the loader until protect_image.
            DWORD old = 0;
            bool ok = VirtualProtect(view, size, PAGE_READWRITE, &old) != FALSE;
            const auto actual = reinterpret_cast<std::uint64_t>(view);
            std::int64_t applied = 0;
            if (ok && actual != kernel_base) {
                // Not at the base the kernel relocated for: relocate the rest of the way.
                std::string reloc_error;
                applied = headers.relocs_stripped()
                              ? -1
                              : apply_base_relocations(view, headers.size_of_image, relocs,
                                                       static_cast<std::int64_t>(actual - kernel_base), reloc_error);
                if (applied >= 0) std::memcpy(view + headers.nt_offset + 24 + 24, &actual, 8);
                ok = applied >= 0;
            }
            if (ok) {
                out.base = view;
                out.size = size;
                out.view = true;
                out.delta = static_cast<std::int64_t>(actual - headers.image_base);
                out.relocations_applied = applied;
                out.file_size = file_size;
                out.last_write_time = last_write;
                finish_image(out, headers, head);
                return true;
            }
            UnmapViewOfFile(view);   // fall back to copying
        }
    }

    // Reserve and commit SizeOfImage, at the preferred base when free.
    std::uint8_t* base = nullptr;
    if (!options.force_relocation) {
        base = static_cast<std::uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(headers.image_base),
                                                       size, MEM_RESERVE | MEM_COMMIT,
                                                       PAGE_READWRITE));
    }
    if (!base) {
        // An image without a relocation directory is still relocatable unless
        // IMAGE_FILE_RELOCS_STRIPPED says otherwise (PC-relative code needs none).
        if (headers.relocs_stripped()) {
            error = options.force_relocation ? "image has its relocations stripped"
                                             : "preferred base is in use and the image has no relocations";
            return false;
        }
        base = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT,
                                                       PAGE_READWRITE));
        if (base && options.force_relocation &&
            reinterpret_cast<std::uint64_t>(base) == headers.image_base) {
            // Extremely unlikely: the system picked the preferred base. Pick again.
            std::uint8_t* other = static_cast<std::uint8_t*>(
                VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            VirtualFree(base, 0, MEM_RELEASE);
            base = other;
        }
        if (!base) {
            error = win32_error("cannot allocate image memory");
            return false;
        }
    }
    auto fail = [&](std::string message) {
        VirtualFree(base, 0, MEM_RELEASE);
        error = std::move(message);
        return false;
    };

    std::memcpy(base, head.data(), headers.size_of_headers);

    // Sections in file order so reads are sequential.
    std::vector<const PeSection*> order;
    order.reserve(headers.sections.size());
    for (const auto& s : headers.sections) {
        if (s.raw_size != 0 && s.raw_offset < file_size) order.push_back(&s);
    }
    std::sort(order.begin(), order.end(),
              [](const PeSection* a, const PeSection* b) { return a->raw_offset < b->raw_offset; });
    std::vector<ReadChunk> chunks;
    std::uint64_t total = 0;
    for (const PeSection* s : order) {
        std::uint64_t count = s->raw_size;
        if (s->virtual_size != 0) {
            count = std::min<std::uint64_t>(count, align_up(s->virtual_size, headers.section_alignment));
        }
        count = std::min<std::uint64_t>(count, file_size - s->raw_offset);
        count = std::min<std::uint64_t>(count, headers.size_of_image - s->rva);
        for (std::uint64_t done = 0; done < count; done += kChunk) {
            chunks.push_back({s->raw_offset + done, base + s->rva + done, std::min(kChunk, count - done)});
        }
        total += count;
    }
    if (!read_chunks(file.get(), path, chunks, total)) {
        return fail(win32_error("cannot read section data"));
    }

    out.base = base;
    out.size = size;
    out.delta = static_cast<std::int64_t>(reinterpret_cast<std::uint64_t>(base) - headers.image_base);
    out.file_size = file_size;
    out.last_write_time = last_write;
    if (out.delta != 0) {
        std::string reloc_error;
        const std::int64_t applied =
            apply_base_relocations(base, headers.size_of_image, relocs, out.delta, reloc_error);
        if (applied < 0) {
            out = MappedImage{};
            return fail("relocation failed: " + reloc_error);
        }
        out.relocations_applied = applied;
        // Keep the in-memory ImageBase consistent with the actual base, as the
        // Windows loader does (code reading its own headers relies on it).
        std::uint64_t actual = reinterpret_cast<std::uint64_t>(base);
        std::memcpy(base + headers.nt_offset + 24 + 24, &actual, 8);
    }
    finish_image(out, headers, head);
    return true;
}

bool protect_image(const MappedImage& image, std::string& error) {
    const auto& h = image.headers;
    DWORD old = 0;
    if (h.section_alignment < kPage) {
        // Sections share pages: the whole image must stay writable.
        if (!VirtualProtect(image.base, image.size, PAGE_READWRITE, &old)) {
            error = win32_error("VirtualProtect(image) failed");
            return false;
        }
        return true;
    }
    if (!VirtualProtect(image.base, image.size, PAGE_READONLY, &old)) {
        error = win32_error("VirtualProtect(headers) failed");
        return false;
    }
    for (const auto& s : h.sections) {
        const std::uint64_t extent = s.virtual_size ? s.virtual_size : s.raw_size;
        if (extent == 0) continue;
        const std::uint64_t begin = s.rva;
        const std::uint64_t end = std::min<std::uint64_t>(align_up(begin + extent, h.section_alignment),
                                                          image.size);
        if (end <= begin) continue;
        const DWORD protection = section_protection(s.characteristics);
        if (protection == PAGE_READONLY) continue;   // already
        if (!VirtualProtect(image.base + begin, end - begin, protection, &old)) {
            error = win32_error("VirtualProtect(section) failed");
            return false;
        }
    }
    return true;
}

void unmap_image(MappedImage& image) noexcept {
    if (image.base) {
        if (image.view) UnmapViewOfFile(image.base);
        else VirtualFree(image.base, 0, MEM_RELEASE);
    }
    image.base = nullptr;
    image.size = 0;
    image.view = false;
}

} // namespace transmute::fx::loader
