#include "fx/cache/test_support.hpp"

#include "fx/cache/pe_view.hpp"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <stdexcept>

namespace transmute::fx::cache_test {

bool g_stub_split_at_calls = false;

TempDir::TempDir(const wchar_t* tag) {
    wchar_t temp[MAX_PATH + 1];
    GetTempPathW(MAX_PATH + 1, temp);
    static LONG counter = 0;
    path_ = std::wstring(temp) + L"fx-cache-tests";
    CreateDirectoryW(path_.c_str(), nullptr);
    wchar_t suffix[64];
    swprintf(suffix, 64, L"\\%s-%lu-%ld-%llu", tag, GetCurrentProcessId(), InterlockedIncrement(&counter),
             static_cast<unsigned long long>(GetTickCount64()));
    path_ += suffix;
    CreateDirectoryW(path_.c_str(), nullptr);
}

TempDir::~TempDir() {
    for (const std::wstring& f : files()) DeleteFileW((path_ + L"\\" + f).c_str());
    RemoveDirectoryW(path_.c_str());
}

std::vector<std::wstring> TempDir::files(const wchar_t* pattern) const {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((path_ + L"\\" + pattern).c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.emplace_back(fd.cFileName);
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    std::sort(out.begin(), out.end());
    return out;
}

FakeModule::FakeModule(std::uint64_t size, std::uint32_t seed) : size_(size) {
    memory_ = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, static_cast<SIZE_T>(size), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    std::mt19937 rng(seed);
    for (std::uint64_t i = 0; i + 4 <= size; i += 4) {
        const std::uint32_t word = rng();
        std::memcpy(memory_ + i, &word, 4);
    }
}

FakeModule::FakeModule(const FakeModule& from, int) : size_(from.size_) {
    memory_ = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, static_cast<SIZE_T>(size_), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    std::memcpy(memory_, from.memory_, static_cast<std::size_t>(size_));
}

FakeModule::~FakeModule() {
    if (memory_) VirtualFree(memory_, 0, MEM_RELEASE);
}

GuestModule FakeModule::info(std::uint64_t image_hash, const wchar_t* name) const {
    GuestModule m;
    m.base = base();
    m.size = size_;
    m.name = name;
    m.image_hash = image_hash;
    m.text_begin = base();
    m.text_end = base() + size_;
    return m;
}

TranslatedRegion make_region(std::uint64_t base, std::uint32_t rva, std::uint32_t n, std::uint32_t seed) {
    std::mt19937 rng(seed * 2654435761u ^ rva);
    n = std::max<std::uint32_t>(n, 4);
    TranslatedRegion r;
    const std::uint64_t start = base + rva;
    r.guest_start = start;
    r.guest_ranges.push_back({start, start + 4ull * n});
    if (n % 3 == 0) r.guest_ranges.push_back({start + 4ull * n + 16, start + 4ull * n + 32});  // literal pool
    const std::size_t code_size = (64 + 16 * std::size_t{n} + (rng() % 64)) & ~std::size_t{7};
    r.code.resize(code_size);
    for (auto& b : r.code) b = static_cast<std::uint8_t>(rng());
    r.entries.push_back({start, 0});
    for (std::uint32_t k = 1; k < 4 && k < n; ++k)
        r.entries.push_back({start + 4ull * (k * n / 4), 16 * k});
    r.exits.push_back({8, start + 4ull * n});
    r.exits.push_back({20, base + ((rva / 2) & ~3u)});
    const std::uint64_t values[] = {start + 4ull * (rng() % n), base + (rva & ~0xFFFu), start + 4ull * n};
    for (std::uint32_t k = 0; k < 3; ++k) {
        const std::uint32_t offset = 32 + 8 * k;
        r.fixups.push_back({offset, FixupKind::guest_address_imm64, values[k]});
        std::memcpy(r.code.data() + offset, &values[k], 8);
    }
    for (std::uint32_t k = 0; k < 4; ++k)
        r.faults.push_back({56 + 8 * k, 60 + 8 * k, start + 4ull * k,
                            static_cast<FlagsLocation>(k % 5), static_cast<std::uint8_t>(k)});
    r.pins = {{19, 3, false}, {20, 6, false}, {1, 7, true}};
    r.fault_maps = {{0, 2}, {2, 1}};
    r.fault_map_regs = {{9, 8, false}, {2, 1, true}, {20, kFaultMapContext, false}};
    r.faults[1].map = 1;
    r.faults[3].map = 2;
    r.stats = {n, n / 8, 1 + n / 16};
    return r;
}

bool regions_equal(const TranslatedRegion& a, const TranslatedRegion& b) {
    if (a.guest_start != b.guest_start || a.guest_ranges != b.guest_ranges || a.code != b.code) return false;
    if (a.tier != b.tier || a.tier_counter != b.tier_counter || a.tier_counter_slot != b.tier_counter_slot) return false;
    if (a.entries.size() != b.entries.size() || a.exits.size() != b.exits.size() ||
        a.fixups.size() != b.fixups.size() || a.faults.size() != b.faults.size() || a.pins.size() != b.pins.size())
        return false;
    for (std::size_t i = 0; i < a.entries.size(); ++i)
        if (a.entries[i].guest_pc != b.entries[i].guest_pc || a.entries[i].offset != b.entries[i].offset) return false;
    for (std::size_t i = 0; i < a.exits.size(); ++i)
        if (a.exits[i].rel32_offset != b.exits[i].rel32_offset || a.exits[i].target_pc != b.exits[i].target_pc)
            return false;
    for (std::size_t i = 0; i < a.fixups.size(); ++i)
        if (a.fixups[i].offset != b.fixups[i].offset || a.fixups[i].kind != b.fixups[i].kind ||
            a.fixups[i].value != b.fixups[i].value)
            return false;
    for (std::size_t i = 0; i < a.faults.size(); ++i)
        if (a.faults[i].host_begin != b.faults[i].host_begin || a.faults[i].host_end != b.faults[i].host_end ||
            a.faults[i].guest_pc != b.faults[i].guest_pc || a.faults[i].flags != b.faults[i].flags ||
            a.faults[i].flags_register != b.faults[i].flags_register || a.faults[i].map != b.faults[i].map)
            return false;
    if (a.fault_maps.size() != b.fault_maps.size() || a.fault_map_regs.size() != b.fault_map_regs.size()) return false;
    for (std::size_t i = 0; i < a.fault_maps.size(); ++i)
        if (a.fault_maps[i].begin != b.fault_maps[i].begin || a.fault_maps[i].count != b.fault_maps[i].count) return false;
    for (std::size_t i = 0; i < a.fault_map_regs.size(); ++i)
        if (a.fault_map_regs[i].guest != b.fault_map_regs[i].guest || a.fault_map_regs[i].host != b.fault_map_regs[i].host ||
            a.fault_map_regs[i].vector != b.fault_map_regs[i].vector)
            return false;
    for (std::size_t i = 0; i < a.pins.size(); ++i)
        if (a.pins[i].guest != b.pins[i].guest || a.pins[i].host != b.pins[i].host ||
            a.pins[i].vector != b.pins[i].vector)
            return false;
    return a.stats.guest_instructions == b.stats.guest_instructions &&
           a.stats.interpreted_instructions == b.stats.interpreted_instructions && a.stats.blocks == b.stats.blocks &&
           a.stats.hot_bytes == b.stats.hot_bytes;
}

std::wstring cache_dir_config(CacheConfig& config, const TempDir& dir) {
    config.directory = dir.path();
    return dir.path();
}

// ---------------------------------------------------------------------------
// stub translator

namespace {

struct StubEmitter {
    TranslatedRegion& r;
    void pad_for_rel32() {
        while ((r.code.size() + 1) % 4 != 0) r.code.push_back(0x90);
    }
    void imm64(std::uint64_t value) {  // mov rax, imm64 with a fixup
        r.code.push_back(0x48);
        r.code.push_back(0xB8);
        r.fixups.push_back({static_cast<std::uint32_t>(r.code.size()), FixupKind::guest_address_imm64, value});
        for (int i = 0; i < 8; ++i) r.code.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
    void exit(std::uint64_t target) {
        pad_for_rel32();
        r.code.push_back(0xE9);
        r.exits.push_back({static_cast<std::uint32_t>(r.code.size()), target});
        for (int i = 0; i < 4; ++i) r.code.push_back(0);
        imm64(target);  // unlinked tail
        r.code.insert(r.code.end(), {0x41, 0xFF, 0x67, 0x10});
    }
    void body(unsigned bytes) { r.code.insert(r.code.end(), bytes, 0x90); }
};

std::int64_t sext(std::uint64_t value, unsigned bits) {
    const std::uint64_t m = std::uint64_t{1} << (bits - 1);
    return static_cast<std::int64_t>((value ^ m) - m);
}

} // namespace

TranslatedRegion stub_translate(std::uint64_t pc, const CodeSource& code, const TranslateOptions& options) {
    std::uint32_t word = 0;
    if (!code.read(pc, word)) throw std::runtime_error("stub_translate: unreadable guest pc");
    std::uint64_t function_begin = 0, function_end = 0;
    const bool bounded = code.function_bounds(pc, function_begin, function_end) && function_end > pc;
    TranslatedRegion r;
    r.guest_start = pc;
    r.entries.push_back({pc, 0});
    StubEmitter e{r};
    e.body(8);  // entry prologue
    const unsigned limit = std::min(options.max_instructions ? options.max_instructions : 4096u, 4096u);
    std::uint64_t cur = pc;
    bool terminated = false;
    unsigned count = 0;
    while (count < limit) {
        if (bounded && cur >= function_end) break;
        if (!code.read(cur, word)) break;
        ++count;
        const std::uint64_t next = cur + 4;
        if ((word & 0x7C00'0000u) == 0x1400'0000u) {  // B / BL
            const std::uint64_t target = cur + static_cast<std::uint64_t>(sext(word & 0x03FF'FFFFu, 26) * 4);
            e.exit(target);
            if (!(word & 0x8000'0000u)) {
                terminated = true;
                cur = next;
                break;
            }
            if (g_stub_split_at_calls) {
                terminated = true;
                cur = next;
                break;
            }
            r.entries.push_back({next, static_cast<std::uint32_t>(r.code.size())});
        } else if ((word & 0xFF00'0010u) == 0x5400'0000u) {  // B.cond
            e.exit(cur + static_cast<std::uint64_t>(sext((word >> 5) & 0x7FFFF, 19) * 4));
        } else if ((word & 0x7E00'0000u) == 0x3400'0000u) {  // CBZ/CBNZ
            e.exit(cur + static_cast<std::uint64_t>(sext((word >> 5) & 0x7FFFF, 19) * 4));
        } else if ((word & 0x7E00'0000u) == 0x3600'0000u) {  // TBZ/TBNZ
            e.exit(cur + static_cast<std::uint64_t>(sext((word >> 5) & 0x3FFF, 14) * 4));
        } else if ((word & 0xFFFF'FC1Fu) == 0xD61F'0000u || (word & 0xFFFF'FC1Fu) == 0xD65F'0000u) {  // BR/RET
            e.body(12);
            terminated = true;
            cur = next;
            break;
        } else if ((word & 0xFFFF'FC1Fu) == 0xD63F'0000u) {  // BLR
            e.body(16);
            if (g_stub_split_at_calls) {
                terminated = true;
                cur = next;
                break;
            }
            r.entries.push_back({next, static_cast<std::uint32_t>(r.code.size())});
        } else if ((word & 0x9F00'0000u) == 0x9000'0000u) {  // ADRP
            const std::uint64_t imm = ((word >> 29) & 3) | (((word >> 5) & 0x7FFFF) << 2);
            e.imm64((cur & ~0xFFFull) + static_cast<std::uint64_t>(sext(imm, 21) << 12));
        } else if (((word >> 25) & 5) == 4) {  // loads and stores
            const auto at = static_cast<std::uint32_t>(r.code.size());
            e.body(5);
            r.faults.push_back({at, at + 5, cur, FlagsLocation::context, 0});
        } else if (word == 0) {  // UDF #0: interpret and stop
            e.body(16);
            ++r.stats.interpreted_instructions;
            terminated = true;
            cur = next;
            break;
        } else {
            e.body(4);
        }
        cur = next;
    }
    if (!terminated) e.exit(cur);
    r.guest_ranges.push_back({pc, std::max(cur, pc + 4)});
    r.pins = {{19, 3, false}, {20, 6, false}};
    r.stats.guest_instructions = count;
    r.stats.blocks = 1 + static_cast<std::uint32_t>(r.exits.size());
    r.stats.hot_bytes = static_cast<std::uint32_t>(r.code.size() / 2);   // round-trips through the record (format 5)
    return r;
}

// ---------------------------------------------------------------------------
// images

namespace {

bool is_arm64_image(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    std::uint8_t header[4096] = {};
    DWORD read = 0;
    ReadFile(f, header, sizeof(header), &read, nullptr);
    CloseHandle(f);
    pe::Image image;
    return image.parse(header, read, nullptr) && image.machine() == pe::kMachineArm64;
}

void find_images_into(const std::wstring& dir, std::vector<std::wstring>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        const std::wstring full = dir + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            find_images_into(full, out);
            continue;
        }
        std::wstring lower = name;
        for (wchar_t& c : lower) c = static_cast<wchar_t>(std::towlower(c));
        const bool candidate = lower.size() > 4 && (lower.ends_with(L".dll") || lower.ends_with(L".exe") ||
                                                    lower.ends_with(L".pyd"));
        if (candidate && is_arm64_image(full)) out.push_back(full);
    } while (FindNextFileW(find, &fd));
    FindClose(find);
}

} // namespace

std::vector<std::wstring> find_images(const std::wstring& root) {
    std::vector<std::wstring> out;
    find_images_into(root, out);
    std::sort(out.begin(), out.end());
    return out;
}

MappedImage::MappedImage(const std::wstring& path, bool at_preferred_base) : path_(path) {
    pe::MappedFile file;
    pe::Image image;
    if (!file.open(path, false) || !image.parse(file.data(), file.size(), nullptr)) return;
    size_ = image.size_of_image();
    preferred_base_ = image.image_base();
    void* want = at_preferred_base ? reinterpret_cast<void*>(preferred_base_) : nullptr;
    memory_ = static_cast<std::uint8_t*>(
        VirtualAlloc(want, static_cast<SIZE_T>(size_), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!memory_) return;
    std::memcpy(memory_, file.data(), std::min<std::size_t>(image.size_of_headers(), file.size()));
    for (const pe::Section& s : image.sections())
        if (s.raw_size && std::uint64_t{s.rva} + s.raw_size <= size_)
            std::memcpy(memory_ + s.rva, file.data() + s.raw_offset, s.raw_size);
    // Like the Windows loader, record the actual base in the mapped headers.
    std::uint32_t nt = 0;
    std::memcpy(&nt, memory_ + 0x3C, 4);
    const std::uint64_t actual = base();
    std::memcpy(memory_ + nt + 24 + 24, &actual, 8);
}

MappedImage::~MappedImage() {
    if (memory_) VirtualFree(memory_, 0, MEM_RELEASE);
}

GuestModule MappedImage::info() const {
    GuestModule m;
    m.base = base();
    m.size = size_;
    m.path = path_;
    const std::size_t slash = path_.find_last_of(L"\\/");
    m.name = slash == std::wstring::npos ? path_ : path_.substr(slash + 1);
    m.image_hash = 0;  // let the cache derive it from the mapped headers
    return m;
}

ImageMemorySource::ImageMemorySource(const MappedImage& image, const pe::Image& file)
    : base_(image.base()), size_(file.size_of_image()) {
    for (const pe::Function& f : pe::pdata_functions(file))
        if (f.end > f.begin) functions_.push_back(f);
    std::sort(functions_.begin(), functions_.end(),
              [](const pe::Function& a, const pe::Function& b) { return a.begin < b.begin; });
    functions_.erase(std::unique(functions_.begin(), functions_.end(),
                                 [](const pe::Function& a, const pe::Function& b) { return a.begin == b.begin; }),
                     functions_.end());
    for (const pe::Section& s : file.sections())
        if (s.executable) executable_.push_back(s);
}

bool ImageMemorySource::read(std::uint64_t address, std::uint32_t& word) const {
    if (address < base_ || address - base_ + 4 > size_ || (address & 3)) return false;
    const auto rva = static_cast<std::uint32_t>(address - base_);
    for (const pe::Section& s : executable_) {
        if (rva >= s.rva && std::uint64_t{rva - s.rva} + 4 <= s.raw_size) {
            std::memcpy(&word, reinterpret_cast<const void*>(address), 4);
            return true;
        }
    }
    return false;
}

bool ImageMemorySource::function_bounds(std::uint64_t address, std::uint64_t& begin, std::uint64_t& end) const {
    if (address < base_ || address - base_ > 0xFFFF'FFFFull) return false;
    const auto rva = static_cast<std::uint32_t>(address - base_);
    auto it = std::upper_bound(functions_.begin(), functions_.end(), rva,
                               [](std::uint32_t v, const pe::Function& f) { return v < f.begin; });
    if (it == functions_.begin()) return false;
    --it;
    if (rva >= it->end) return false;
    begin = base_ + it->begin;
    end = base_ + it->end;
    return true;
}

double now_seconds() {
    static const double frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / frequency;
}

} // namespace transmute::fx::cache_test
