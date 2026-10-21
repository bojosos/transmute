#include "alchemy/macho.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#else
#include <unistd.h>
#define GetCurrentProcessId getpid
#endif

namespace {

constexpr std::uint64_t preferred = 0x100000000ULL;
constexpr std::size_t chain_offset = 0x2000;
constexpr std::size_t chain_size = 128;
constexpr std::size_t data_offset = 0x1000;
constexpr std::size_t data_command = 32 + 72 + 152;
constexpr std::size_t dependency_command = data_command + 152 + 72;
constexpr std::size_t main_command = dependency_command + 56;
constexpr std::size_t fixup_command = main_command + 24;
constexpr std::size_t export_command = fixup_command + 16;

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void expect_failure(const std::function<void()>& operation, std::string_view diagnostic = {}) {
    try { operation(); }
    catch (const std::runtime_error& error) {
        if (!diagnostic.empty()) expect(std::string(error.what()).find(diagnostic) != std::string::npos,
                                        "unexpected failure diagnostic");
        return;
    }
    throw std::runtime_error("expected a failure");
}

void put16(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint16_t value) {
    bytes.at(at) = static_cast<std::uint8_t>(value);
    bytes.at(at + 1) = static_cast<std::uint8_t>(value >> 8);
}

void put32(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint32_t value) {
    put16(bytes, at, static_cast<std::uint16_t>(value));
    put16(bytes, at + 2, static_cast<std::uint16_t>(value >> 16));
}

void put64(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint64_t value) {
    put32(bytes, at, static_cast<std::uint32_t>(value));
    put32(bytes, at + 4, static_cast<std::uint32_t>(value >> 32));
}

void put_string(std::vector<std::uint8_t>& bytes, std::size_t at, const std::string& text) {
    for (const auto c : text) bytes.at(at++) = static_cast<std::uint8_t>(c);
    bytes.at(at) = 0;
}

void segment(std::vector<std::uint8_t>& bytes, std::size_t at, const std::string& name,
             std::uint64_t address, std::uint64_t size, std::uint64_t offset,
             std::uint64_t file_size, std::uint32_t permissions, bool section) {
    put32(bytes, at, 0x19);
    put32(bytes, at + 4, section ? 152 : 72);
    put_string(bytes, at + 8, name);
    put64(bytes, at + 24, address);
    put64(bytes, at + 32, size);
    put64(bytes, at + 40, offset);
    put64(bytes, at + 48, file_size);
    put32(bytes, at + 56, permissions);
    put32(bytes, at + 60, permissions);
    put32(bytes, at + 64, section ? 1 : 0);
    if (section) {
        const bool text = name == "__TEXT";
        put_string(bytes, at + 72, text ? "__text" : "__mod_init_func");
        put_string(bytes, at + 88, name);
        put64(bytes, at + 104, address + (text ? 0x500 : 0));
        put64(bytes, at + 112, 8);
        put32(bytes, at + 120, static_cast<std::uint32_t>(offset) + (text ? 0x500U : 0U));
        put32(bytes, at + 124, 3);
        put32(bytes, at + 136, text ? 0x80000400U : 9U);
    }
}

std::vector<std::uint8_t> fixture() {
    std::vector<std::uint8_t> bytes(0x2200);
    put32(bytes, 0, 0xfeedfacf);
    put32(bytes, 4, 0x0100000c);
    put32(bytes, 12, 2);
    put32(bytes, 16, 8);
    put32(bytes, 20, static_cast<std::uint32_t>(export_command + 16 - 32));
    put32(bytes, 24, 0x200085);
    segment(bytes, 32, "__PAGEZERO", 0, preferred, 0, 0, 0, false);
    segment(bytes, 104, "__TEXT", preferred, 0x1000, 0, 0x1000, 5, true);
    segment(bytes, data_command, "__DATA", preferred + 0x1000, 0x1000, 0x1000, 0x100, 3, true);
    segment(bytes, data_command + 152, "__LINKEDIT", preferred + 0x2000, 0x1000, 0x2000, 0x200, 1, false);
    put32(bytes, dependency_command, 0xc);
    put32(bytes, dependency_command + 4, 56);
    put32(bytes, dependency_command + 8, 24);
    put_string(bytes, dependency_command + 24, "/usr/lib/libSystem.B.dylib");
    put32(bytes, main_command, 0x80000028);
    put32(bytes, main_command + 4, 24);
    put64(bytes, main_command + 8, 0x500);
    put32(bytes, fixup_command, 0x80000034);
    put32(bytes, fixup_command + 4, 16);
    put32(bytes, fixup_command + 8, chain_offset);
    put32(bytes, fixup_command + 12, chain_size);
    put32(bytes, export_command, 0x80000033);
    put32(bytes, export_command + 4, 16);
    put32(bytes, export_command + 8, 0x2100);
    put32(bytes, export_command + 12, 15);
    put32(bytes, 0x500, 0x52800540); // mov w0, #42
    put32(bytes, 0x504, 0xd65f03c0); // ret
    put64(bytes, data_offset, 0x500ULL | (2ULL << 51));
    put64(bytes, data_offset + 8, 1ULL << 63);
    put32(bytes, chain_offset + 4, 28);
    put32(bytes, chain_offset + 8, 100);
    put32(bytes, chain_offset + 12, 104);
    put32(bytes, chain_offset + 16, 1);
    put32(bytes, chain_offset + 20, 1);
    put32(bytes, chain_offset + 28, 4);
    put32(bytes, chain_offset + 40, 20); // Segment index 2 is __DATA.
    put32(bytes, chain_offset + 48, 24);
    put16(bytes, chain_offset + 52, 4096);
    put16(bytes, chain_offset + 54, 6);
    put64(bytes, chain_offset + 56, 0x1000);
    put16(bytes, chain_offset + 68, 1);
    put16(bytes, chain_offset + 70, 0);
    put32(bytes, chain_offset + 100, 1);
    put_string(bytes, chain_offset + 104, "_puts");
    // Root -> "_answer" -> regular symbol at image offset 0x500.
    bytes.at(0x2100) = 0;
    bytes.at(0x2101) = 1;
    put_string(bytes, 0x2102, "_answer");
    bytes.at(0x210a) = 11;
    bytes.at(0x210b) = 3;
    bytes.at(0x210c) = 0;
    bytes.at(0x210d) = 0x80;
    bytes.at(0x210e) = 0x0a;
    bytes.at(0x210f) = 0;
    put32(bytes, export_command + 12, 16);
    return bytes;
}

// Checked test memory also models reservations without allocating page-zero.
class Memory final : public transmute::GuestMemory {
public:
    struct Region { std::uint64_t size; std::vector<std::uint8_t> bytes; bool read; bool write; bool execute; };
    std::map<std::uint64_t, Region> regions;
    std::uint64_t fail_map_base{};
    std::uint8_t read8(std::uint64_t address) const override {
        const auto found = find(address);
        if (!found->second.read || found->second.bytes.empty()) throw std::runtime_error("unreadable memory");
        return found->second.bytes.at(static_cast<std::size_t>(address - found->first));
    }
    void write8(std::uint64_t address, std::uint8_t value) override {
        const auto found = find(address);
        if (!found->second.write || found->second.bytes.empty()) throw std::runtime_error("unwritable memory");
        found->second.bytes.at(static_cast<std::size_t>(address - found->first)) = value;
    }
    void reserve_memory(std::uint64_t base, std::uint64_t size) override {
        if (!is_range_available(base, size)) throw std::runtime_error("unavailable reservation");
        regions.emplace(base, Region{size, {}, false, false, false});
    }
    void map_memory(std::uint64_t base, std::size_t size) override {
        if (base == fail_map_base || !is_range_available(base, size)) throw std::runtime_error("unavailable mapping");
        regions.emplace(base, Region{size, std::vector<std::uint8_t>(size), true, true, false});
    }
    void map_external_memory(std::uint64_t, std::uint8_t*, std::size_t, bool) override {
        throw std::runtime_error("unsupported external mapping in test");
    }
    void unmap_memory(std::uint64_t base) override {
        if (regions.erase(base) != 1) throw std::runtime_error("missing mapping");
    }
    bool is_range_available(std::uint64_t base, std::uint64_t size) const override {
        if (size == 0 || base > UINT64_MAX - size) return false;
        return std::none_of(regions.begin(), regions.end(), [&](const auto& item) {
            return base < item.first + item.second.size && item.first < base + size;
        });
    }
    std::optional<RegionInfo> query_region(std::uint64_t address) const override {
        for (const auto& [base, region] : regions) {
            if (address >= base && address - base < region.size) {
                return RegionInfo{base, region.size, region.read, region.write, region.execute,
                                  false, !region.bytes.empty()};
            }
        }
        return std::nullopt;
    }
    void set_memory_access(std::uint64_t base, bool read, bool write, bool execute) override {
        auto& region = regions.at(base);
        region.read = read;
        region.write = write;
        region.execute = execute;
    }

private:
    auto find(std::uint64_t address) const -> std::map<std::uint64_t, Region>::const_iterator {
        auto found = regions.upper_bound(address);
        if (found == regions.begin()) throw std::runtime_error("unmapped memory");
        --found;
        if (address - found->first >= found->second.size) throw std::runtime_error("unmapped memory");
        return found;
    }
    auto find(std::uint64_t address) -> std::map<std::uint64_t, Region>::iterator {
        auto found = regions.upper_bound(address);
        if (found == regions.begin()) throw std::runtime_error("unmapped memory");
        --found;
        if (address - found->first >= found->second.size) throw std::runtime_error("unmapped memory");
        return found;
    }
};

void test_mapping_and_binding() {
    {
        const auto image = alchemy::MachOImage::parse(fixture());
        expect(image.preferred_base() == preferred && image.image_size() == 0x3000, "bad image metadata");
        expect(image.imports().size() == 1 && image.imports().front().symbol == "_puts", "bad import metadata");
        expect(image.pointer_formats() == std::vector<std::uint16_t>{6}, "bad pointer format metadata");
        // An image binds once: bind() frees its fixups, and binding again fails.
        expect(image.fixup_count() == 2, "two fixups parsed");
        Memory memory;
        image.map(memory, preferred);
        image.bind(memory, preferred, [](std::int32_t, std::string_view, bool) { return std::optional<std::uint64_t>{0x600000000000}; });
        expect(image.fixup_count() == 0, "bind frees the fixups");
        expect(memory.read64(preferred + 0x1000) == preferred + 0x500, "the fixups were applied before they were freed");
        expect_failure([&] {
            image.bind(memory, preferred, [](std::int32_t, std::string_view, bool) { return std::optional<std::uint64_t>{0}; });
        }, "already bound");
    }
    for (const auto base : {preferred, preferred + 0x100000}) {
        const auto image = alchemy::MachOImage::parse(fixture());
        Memory memory;
        image.map(memory, base);
        expect(memory.query_region(0)->size == preferred && !memory.query_region(0)->mapped,
               "page zero must have no allocated backing");
        expect(!memory.is_range_available(0x1000, 0x1000), "page zero must reject mapping");
        expect_failure([&] { (void)memory.read8(0); });
        expect(memory.read64(base + 0x1ff8) == 0, "segment tail was not zero filled");
        unsigned resolutions = 0;
        image.bind(memory, base, [&](std::int32_t ordinal, std::string_view symbol, bool weak) {
            expect(ordinal == 1 && symbol == "_puts" && !weak, "bad resolver input");
            ++resolutions;
            return std::optional<std::uint64_t>{0x600000000000};
        });
        expect(resolutions == 1, "imports should resolve once");
        expect(memory.read64(base + 0x1000) == base + 0x500, "bad offset rebase");
        expect(memory.read64(base + 0x1008) == 0x600000000000, "bad binding");
        image.protect(memory, base);
        expect(memory.query_region(base)->executable && !memory.query_region(base)->writable,
               "text permissions were not applied");
        expect(memory.query_region(base + 0x1000)->writable && !memory.query_region(base + 0x1000)->executable,
               "data permissions were not applied");
        expect_failure([&] { memory.write32(base + 0x500, 0); });
        expect(image.initializers(memory, base) == std::vector<std::uint64_t>{base + 0x500}, "bad initializers");
        expect(image.exported_symbol("_answer", base) == base + 0x500, "bad exported symbol");
        expect(!image.exported_symbol("_missing", base), "missing export should be absent");
        expect(image.entry(base) == base + 0x500, "bad entry");
    }
    const auto image = alchemy::MachOImage::parse(fixture());
    Memory collision;
    collision.map_memory(preferred + 0x1000, 4096);
    expect_failure([&] { image.map(collision, preferred); }, "unavailable");
    expect(collision.regions.size() == 1, "preflight mapping left side effects");
    Memory rollback;
    rollback.fail_map_base = preferred + 0x1000;
    expect_failure([&] { image.map(rollback, preferred); });
    expect(rollback.regions.empty(), "map failure must roll back mappings and reservation");
    Memory bad_base;
    expect_failure([&] { image.map(bad_base, 0x10000); }, "__PAGEZERO");
    expect_failure([&] { (void)image.entry(preferred + 1); }, "aligned");
    auto constant_bytes = fixture();
    put32(constant_bytes, data_command + 68, 0x10);
    const auto constant = alchemy::MachOImage::parse(constant_bytes);
    Memory constant_memory;
    constant.map(constant_memory, preferred);
    constant.bind(constant_memory, preferred, [](std::int32_t, std::string_view, bool) {
        return std::optional<std::uint64_t>{0x600000000000};
    });
    constant.protect(constant_memory, preferred);
    expect(!constant_memory.query_region(preferred + 0x1000)->writable,
           "SG_READ_ONLY must protect constants after binding");
    expect_failure([&] { constant_memory.write64(preferred + 0x1008, 0); });
}

void test_import_formats() {
    for (const auto format : {1U, 2U, 3U}) {
        auto bytes = fixture();
        put32(bytes, chain_offset + 20, format);
        const auto symbols = format == 1 ? 104U : format == 2 ? 108U : 116U;
        put32(bytes, chain_offset + 12, symbols);
        put_string(bytes, chain_offset + symbols, "_puts");
        if (format == 2) put32(bytes, chain_offset + 104, static_cast<std::uint32_t>(-7));
        if (format == 3) {
            put64(bytes, chain_offset + 100, 1);
            put64(bytes, chain_offset + 108, static_cast<std::uint64_t>(-7));
        }
        put64(bytes, data_offset + 8, (1ULL << 63) | (0xfeULL << 24));
        const auto image = alchemy::MachOImage::parse(bytes);
        Memory memory;
        image.map(memory, preferred);
        image.bind(memory, preferred, [](std::int32_t, std::string_view, bool) {
            return std::optional<std::uint64_t>{0x800000000ULL};
        });
        expect(memory.read64(preferred + 0x1008) == 0x800000000ULL + 254 - (format == 1 ? 0 : 7),
               "binding addends decoded incorrectly");
    }
    for (const auto ordinal : {-3, -2, -1, 0}) {
        auto bytes = fixture();
        put32(bytes, chain_offset + 100, static_cast<std::uint32_t>(ordinal) & 0xffU);
        const auto image = alchemy::MachOImage::parse(bytes);
        expect(image.imports().front().library_ordinal == ordinal, "special ordinal sign extension failed");
    }
    auto weak_bytes = fixture();
    put32(weak_bytes, chain_offset + 100, 0x101);
    const auto weak = alchemy::MachOImage::parse(weak_bytes);
    Memory memory;
    weak.map(memory, preferred);
    weak.bind(memory, preferred, {});
    expect(memory.read64(preferred + 0x1008) == 0, "unresolved weak binding must become null");
    const auto strong = alchemy::MachOImage::parse(fixture());
    expect_failure([&] { strong.bind(memory, preferred, {}); }, "unresolved strong");
    expect(memory.read64(preferred + 0x1008) == 0, "failed eager resolution modified binding");

    auto absolute = fixture();
    put16(absolute, chain_offset + 54, 2);
    put64(absolute, data_offset, (preferred + 0x500) | (2ULL << 51));
    const auto ptr64 = alchemy::MachOImage::parse(absolute);
    Memory moved;
    ptr64.map(moved, preferred + 0x200000);
    ptr64.bind(moved, preferred + 0x200000, [](std::int32_t, std::string_view, bool) {
        return std::optional<std::uint64_t>{0x800000000ULL};
    });
    expect(moved.read64(preferred + 0x201000) == preferred + 0x200500, "absolute ptr64 relocation failed");
}

void test_multistart_and_16k() {
    auto bytes = fixture();
    put32(bytes, chain_offset + 48, 28);
    put16(bytes, chain_offset + 70, 0x8001);
    put16(bytes, chain_offset + 72, 0);
    put16(bytes, chain_offset + 74, 0x8008);
    put64(bytes, data_offset, 0x500);
    const auto multiple = alchemy::MachOImage::parse(bytes);
    Memory memory;
    multiple.map(memory, preferred);
    multiple.bind(memory, preferred, [](std::int32_t, std::string_view, bool) {
        return std::optional<std::uint64_t>{0x800000000ULL};
    });
    expect(memory.read64(preferred + 0x1000) == preferred + 0x500 &&
           memory.read64(preferred + 0x1008) == 0x800000000ULL, "multiple page chains failed");
    bytes = fixture();
    put16(bytes, chain_offset + 52, 16384);
    (void)alchemy::MachOImage::parse(bytes);
}

void test_offset_initializers() {
    auto bytes = fixture();
    put_string(bytes, data_command + 72, "__init_offsets");
    put32(bytes, data_command + 136, 0x16);
    put64(bytes, data_command + 112, 4);
    put16(bytes, chain_offset + 70, 8); // Only the import pointer is a chained fixup.
    put64(bytes, data_offset, 0x500);
    for (const auto base : {preferred, preferred + 0x200000}) {
        const auto image = alchemy::MachOImage::parse(bytes);
        Memory memory;
        image.map(memory, base);
        image.bind(memory, base, [](std::int32_t, std::string_view, bool) {
            return std::optional<std::uint64_t>{0x800000000ULL};
        });
        expect(image.initializers(memory, base) == std::vector<std::uint64_t>{base + 0x500},
               "initializer offsets must relocate from the image header");
        memory.write32(base + 0x1000, 0x1008);
        expect_failure([&] { (void)image.initializers(memory, base); }, "executable code");
        memory.write32(base + 0x1000, 0x501);
        expect_failure([&] { (void)image.initializers(memory, base); }, "initializer");
    }
    auto malformed = bytes;
    put64(malformed, data_command + 112, 6);
    expect_failure([&] { (void)alchemy::MachOImage::parse(malformed); }, "initializer offset");
    malformed = bytes;
    put64(malformed, data_command + 104, preferred + 0x1002);
    put32(malformed, data_command + 120, 0x1002);
    expect_failure([&] { (void)alchemy::MachOImage::parse(malformed); }, "initializer offset");
    malformed = bytes;
    put32(malformed, data_command + 136, 0x17);
    expect_failure([&] { (void)alchemy::MachOImage::parse(malformed); }, "section type");
}

// A sparse file: only `head` and `tail` occupy disk; the gap reads as zeros.
// Returns false when the file system cannot store sparse files.
bool write_sparse(const std::filesystem::path& path, const std::vector<std::uint8_t>& head,
                  std::uint64_t tail_offset, const std::vector<std::uint8_t>& tail) {
#ifdef _WIN32
    const auto file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot create sparse test file");
    DWORD returned = 0;
    bool ok = DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr) != 0;
    const auto write_at = [&](std::uint64_t offset, const std::vector<std::uint8_t>& bytes) {
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);
        DWORD written = 0;
        return SetFilePointerEx(file, position, nullptr, FILE_BEGIN) != 0 &&
               WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) != 0 &&
               written == bytes.size();
    };
    ok = ok && write_at(0, head) && write_at(tail_offset, tail);
    CloseHandle(file);
    if (!ok) std::filesystem::remove(path);
    return ok;
#else
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(head.data()), static_cast<std::streamsize>(head.size()));
    }
    std::filesystem::resize_file(path, tail_offset);
    std::ofstream output(path, std::ios::binary | std::ios::app);
    output.write(reinterpret_cast<const char*>(tail.data()), static_cast<std::streamsize>(tail.size()));
    return static_cast<bool>(output);
#endif
}

// An installer-style image: LC_SYMTAB's string table is 3 GiB (larger than any
// in-memory limit, beyond 2^31) inside __LINKEDIT, and the export trie follows
// it. load() must leave the table on disk, read the trie past it, map the
// __LINKEDIT head and reserve (not map) the rest.
void test_sparse_string_table() {
    constexpr std::uint64_t string_offset = 0x2400;
    constexpr std::uint64_t string_size = 0xC0000000ULL;
    constexpr std::uint64_t tail_offset = string_offset + string_size;
    constexpr std::uint64_t file_size = tail_offset + 0x100;
    constexpr std::uint64_t linkedit_file_size = file_size - 0x2000;
    constexpr std::uint64_t linkedit_size = (linkedit_file_size + 0x3fff) & ~0x3fffULL;
    auto head = fixture();
    std::vector<std::uint8_t> tail(head.begin() + 0x2100, head.begin() + 0x2110);
    tail.resize(0x100);
    head.resize(string_offset + 0x20);
    put_string(head, string_offset + 1, "_kept_head");
    put32(head, 16, 9);
    put32(head, 20, static_cast<std::uint32_t>(export_command + 16 + 24 - 32));
    put64(head, data_command + 152 + 32, linkedit_size);
    put64(head, data_command + 152 + 48, linkedit_file_size);
    put32(head, export_command + 8, static_cast<std::uint32_t>(tail_offset));
    const auto symtab = export_command + 16;
    put32(head, symtab, 0x2);
    put32(head, symtab + 4, 24);
    put32(head, symtab + 8, 0x2200);
    put32(head, symtab + 12, 1); // One nlist naming "_kept_head" (strx 1).
    put32(head, 0x2200, 1);
    put32(head, symtab + 16, static_cast<std::uint32_t>(string_offset));
    put32(head, symtab + 20, static_cast<std::uint32_t>(string_size));

    const auto directory = std::filesystem::temp_directory_path() /
        ("alchemy_macho_sparse_" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(directory);
    const auto thin = directory / "thin";
    if (!write_sparse(thin, head, tail_offset, tail)) {
        std::cout << "sparse string-table test skipped: file system has no sparse files\n";
        std::filesystem::remove_all(directory);
        return;
    }
    try {
        expect(std::filesystem::file_size(thin) == file_size, "bad sparse fixture size");
        const auto check = [&](const alchemy::MachOImage& image) {
            expect(image.omitted_bytes() == string_offset + string_size - 0x6000,
                   "string table should be omitted from the 16 KiB boundary on");
            expect(image.image_size() == 0x2000 + linkedit_size, "reserved extent must cover __LINKEDIT");
            Memory memory;
            image.map(memory, preferred);
            const auto linkedit = preferred + 0x2000;
            expect(memory.query_region(linkedit)->mapped && memory.query_region(linkedit)->size == 0x4000,
                   "__LINKEDIT head must be mapped");
            expect(memory.read8(linkedit + 0x401) == '_' && memory.read8(linkedit + 0x402) == 'k',
                   "string bytes before the omitted range must be mapped");
            const auto reserved = memory.query_region(linkedit + 0x4000);
            expect(reserved && !reserved->mapped && reserved->size == linkedit_size - 0x4000,
                   "omitted range must be reserved, not mapped");
            expect_failure([&] { (void)memory.read8(linkedit + 0x4000); });
            image.bind(memory, preferred, [](std::int32_t, std::string_view, bool) {
                return std::optional<std::uint64_t>{0x600000000000};
            });
            image.protect(memory, preferred);
            expect(image.exported_symbol("_answer", preferred) == preferred + 0x500,
                   "export trie after the string table was not read");
        };
        check(alchemy::MachOImage::load(thin));

        // The same slice inside a universal file at a 16 KiB-aligned offset.
        std::vector<std::uint8_t> fat(0x4000 + head.size());
        const auto big32 = [&](std::size_t at, std::uint32_t value) {
            for (unsigned i = 0; i < 4; ++i) fat.at(at + i) = static_cast<std::uint8_t>(value >> (24 - 8 * i));
        };
        big32(0, 0xcafebabe);
        big32(4, 1);
        big32(8, 0x0100000c);
        big32(16, 0x4000);
        big32(20, static_cast<std::uint32_t>(file_size));
        big32(24, 14);
        std::copy(head.begin(), head.end(), fat.begin() + 0x4000);
        const auto universal = directory / "universal";
        expect(write_sparse(universal, fat, 0x4000 + tail_offset, tail), "cannot write universal fixture");
        check(alchemy::MachOImage::load(universal));

        // A huge string table outside __LINKEDIT is not omitted: the file is
        // then too large to hold, and the loader says so before reading it.
        put64(head, data_command + 152 + 32, 0x1000);
        put64(head, data_command + 152 + 48, 0x200);
        put32(head, export_command + 8, 0x2100);
        head.resize(0x2200);
        const auto outside = directory / "outside";
        expect(write_sparse(outside, head, tail_offset, tail), "cannot write outside fixture");
        expect_failure([&] { (void)alchemy::MachOImage::load(outside); }, "excessive file size");
    } catch (...) {
        std::filesystem::remove_all(directory);
        throw;
    }
    std::filesystem::remove_all(directory);
}

#ifdef _WIN32
// load() views the file (file_backed) and map() hands each segment to the
// engine's FileMapper with the file offset of its bytes (the slice's offset
// included); an engine without file mappings, or one that refuses, gets the
// copied segments. release_file() keeps everything parsed.
void test_file_mapping() {
    const auto directory = std::filesystem::temp_directory_path() /
        ("alchemy_macho_filemap_" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(directory);
    const auto write = [&](const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        expect(static_cast<bool>(output), "cannot write file-mapping fixture");
    };
    const auto thin = fixture();
    std::vector<std::uint8_t> fat(0x4000 + thin.size());
    const auto big32 = [&](std::size_t at, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) fat.at(at + i) = static_cast<std::uint8_t>(value >> (24 - 8 * i));
    };
    big32(0, 0xcafebabe);
    big32(4, 1);
    big32(8, 0x0100000c);
    big32(16, 0x4000);
    big32(20, static_cast<std::uint32_t>(thin.size()));
    big32(24, 14);
    std::copy(thin.begin(), thin.end(), fat.begin() + 0x4000);
    const auto same = [](const Memory& expected, const Memory& actual, const char* message) {
        expect(expected.regions.size() == actual.regions.size(), message);
        for (const auto& [base, region] : expected.regions) {
            const auto found = actual.regions.find(base);
            expect(found != actual.regions.end() && found->second.size == region.size &&
                   found->second.bytes == region.bytes, message);
        }
    };
    try {
        write(directory / "thin", thin);
        write(directory / "universal", fat);
        for (const auto& [name, slice] : {std::pair{"thin", std::uint64_t{0}}, std::pair{"universal", std::uint64_t{0x4000}}}) {
            auto image = alchemy::MachOImage::load(directory / name);
            expect(image.file_backed(), "load() should view the file");
            Memory copied;
            image.map(copied, preferred);
            std::vector<std::array<std::uint64_t, 4>> calls;
            Memory viewed;
            image.map(viewed, preferred, [&](std::uint64_t base, std::uint64_t size, void* section, std::uint64_t offset,
                                             std::uint64_t file_bytes) {
                calls.push_back({base, size, offset, file_bytes});
                const auto* view = static_cast<const std::uint8_t*>(MapViewOfFile(static_cast<HANDLE>(section), FILE_MAP_READ, 0, 0, 0));
                expect(view != nullptr, "the mapper's section cannot be viewed");
                viewed.map_memory(base, static_cast<std::size_t>(size));
                for (std::uint64_t i = 0; i < file_bytes; ++i) viewed.write8(base + i, view[offset + i]);
                UnmapViewOfFile(view);
                return true;
            });
            const std::vector<std::array<std::uint64_t, 4>> expected{
                {preferred, 0x1000, slice, 0x1000},
                {preferred + 0x1000, 0x1000, slice + 0x1000, 0x100},
                {preferred + 0x2000, 0x1000, slice + 0x2000, 0x200}};
            expect(calls == expected, "segments must map from their file offsets");
            same(copied, viewed, "file-mapped segments differ from copied ones");
            Memory unsupported;
            image.map(unsupported, preferred, [](std::uint64_t, std::uint64_t, void*, std::uint64_t, std::uint64_t) { return false; });
            same(copied, unsupported, "an engine without file mappings must get copies");
            Memory refused;
            image.map(refused, preferred, [](std::uint64_t, std::uint64_t, void*, std::uint64_t, std::uint64_t) -> bool {
                throw std::runtime_error("refused");
            });
            same(copied, refused, "a refused file mapping must fall back to copies");
            const bool flat = image.flat_namespace();
            image.release_file();
            expect(!image.file_backed() && image.flat_namespace() == flat, "release_file must keep parsed state");
            expect(image.exported_symbol("_answer", preferred) == preferred + 0x500, "exports lost by release_file");
            Memory late;
            expect_failure([&] { image.map(late, preferred); }, "released");
        }
        // A parsed image has no file: its segments are always copied.
        bool called = false;
        Memory parsed;
        alchemy::MachOImage::parse(thin).map(parsed, preferred,
            [&](std::uint64_t, std::uint64_t, void*, std::uint64_t, std::uint64_t) { return called = true; });
        expect(!called && parsed.read8(preferred + 0x1000) == thin.at(data_offset), "a parsed image must be copied");
    } catch (...) {
        std::filesystem::remove_all(directory);
        throw;
    }
    std::filesystem::remove_all(directory);
}
#endif

void test_rejections() {
    const auto reject = [](const std::function<void(std::vector<std::uint8_t>&)>& mutate,
                           std::string_view diagnostic) {
        auto bytes = fixture();
        mutate(bytes);
        expect_failure([&] { (void)alchemy::MachOImage::parse(bytes); }, diagnostic);
    };
    reject([](auto& b) { b.resize(16); }, "truncated");
    reject([](auto& b) { put32(b, 0, 0xcffaedfe); }, "little-endian");
    reject([](auto& b) { put32(b, 8, 2); }, "ARM64e");
    reject([](auto& b) { put32(b, 32 + 4, 71); }, "load-command size");
    reject([](auto& b) { put32(b, 16, 4097); }, "load-command count");
    reject([](auto& b) { put64(b, data_command + 24, preferred); }, "overlapping segments");
    reject([](auto& b) { put64(b, data_command + 32, UINT64_MAX); }, "segment layout");
    reject([](auto& b) { put64(b, data_command + 40, 0x3000); }, "out-of-range");
    reject([](auto& b) { put64(b, main_command + 8, 0x1000); }, "entry");
    reject([](auto& b) { put64(b, main_command + 8, 0x501); }, "entry");
    auto stack_metadata = fixture();
    put64(stack_metadata, main_command + 16, 4096);
    expect(alchemy::MachOImage::parse(stack_metadata).stack_size() == 4096,
           "LC_MAIN stack requirement metadata was lost");
    reject([](auto& b) { put32(b, chain_offset + 24, 1); }, "compressed");
    reject([](auto& b) { put32(b, chain_offset + 20, 4); }, "import format");
    reject([](auto& b) { put32(b, chain_offset + 100, 2); }, "library ordinal");
    reject([](auto& b) { put16(b, chain_offset + 54, 1); }, "pointer format");
    reject([](auto& b) { put16(b, chain_offset + 70, 0xffc); }, "chain leaves");
    reject([](auto& b) { put64(b, data_offset, 0x500 | (0xfffULL << 51)); }, "chain leaves");
    reject([](auto& b) { put64(b, data_offset + 8, (1ULL << 63) | 1); }, "import index");
    reject([](auto& b) { put64(b, data_offset + 8, (1ULL << 63) | (1ULL << 32)); }, "reserved");
    reject([](auto& b) { put64(b, data_offset, 0x3500 | (2ULL << 51)); }, "rebasing target");
    reject([](auto& b) { put64(b, chain_offset + 56, 0x2000); }, "segment/page");
    reject([](auto& b) { put16(b, chain_offset + 70, 0x8000); }, "overlaps page");
    reject([](auto& b) { put32(b, chain_offset + 48, 22); }, "out-of-range");
    reject([](auto& b) { b.at(0x210a) = 0; }, "cyclic export");
    {
        // Older ld64 tries: "_answer" (non-terminal) --""--> terminal "_answer".
        auto empty_edge = fixture();
        const std::uint8_t node[] = {0, 1, 0, 15, 3, 0, 0x80, 0x0a, 0};
        for (std::size_t index = 0; index < sizeof(node); ++index) empty_edge.at(0x210b + index) = node[index];
        put32(empty_edge, export_command + 12, 20);
        const auto parsed = alchemy::MachOImage::parse(empty_edge);
        expect(parsed.exports().size() == 1 && parsed.exports()[0].name == "_answer" &&
               parsed.exports()[0].offset == 0x500, "export trie with an empty edge was not accepted");
        empty_edge.at(0x210e) = 11; // the empty edge now points back to its own node
        reject([&](auto& b) { b = empty_edge; }, "cyclic export");
    }
    reject([](auto& b) { b.at(0x210c) = 8; }, "reexport ordinal");
    reject([](auto& b) { b.at(0x210c) = 16; }, "resolvers");
    reject([](auto& b) { b.at(0x210c) = 1; }, "thread-local");
    reject([](auto& b) { put32(b, export_command, 0x8000001c); }, "string");
    reject([](auto& b) { put32(b, data_command + 136, 0x13); }, "misaligned");
    reject([](auto& b) { put32(b, data_command + 132, 1); }, "classic section relocations");
    const auto image = alchemy::MachOImage::parse(fixture());
    Memory memory;
    image.map(memory, preferred);
    image.bind(memory, preferred, [](std::int32_t, std::string_view, bool) {
        return std::optional<std::uint64_t>{0x800000000ULL};
    });
    memory.write64(preferred + 0x1000, preferred + 0x1008);
    expect_failure([&] { (void)image.initializers(memory, preferred); }, "initializer");
}

} // namespace

int main() {
    try {
        test_mapping_and_binding();
        test_import_formats();
        test_multistart_and_16k();
        test_offset_initializers();
        test_sparse_string_table();
#ifdef _WIN32
        test_file_mapping();
#endif
        test_rejections();
        std::cout << "Alchemy Mach-O tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
