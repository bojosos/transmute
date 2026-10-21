#include "alchemy/macho.hpp"

#include "alchemy/host/files.hpp"
#include "alchemy/host/sections.hpp"
#include "alchemy/path.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>


namespace alchemy {
namespace {

// Only numeric format contracts are used here. Parsing and linking are authored
// in this project; neither an Apple SDK nor a system library is needed.
// Committed (mapped) image bytes and file bytes held in memory. Reserved
// address space for an omitted string table is bounded by the file instead.
// Qt WebEngine Core in a renderer SDK is 561 MiB; 1 GiB bounds one image.
constexpr std::uint64_t maximum_image_size = 1024ULL * 1024 * 1024;
constexpr std::uint64_t maximum_loaded_bytes = maximum_image_size;
// LC_SYMTAB string tables at least this large are left on disk by load().
// Binding never reads them; Alchemy resolves through exports and fixups.
constexpr std::uint64_t omit_string_table_threshold = 16ULL * 1024 * 1024;
constexpr std::uint64_t omitted_page = 0x4000;
constexpr std::uint32_t segment_64 = 0x19;
constexpr std::uint32_t chained_fixups = 0x80000034;
constexpr std::uint32_t exports_trie = 0x80000033;
constexpr std::uint32_t main_command = 0x80000028;

[[noreturn]] void invalid(std::string_view message) {
    throw std::runtime_error("Mach-O: " + std::string(message));
}

bool fits(std::uint64_t offset, std::uint64_t size, std::uint64_t total) {
    return offset <= total && size <= total - offset;
}

std::uint64_t add(std::uint64_t a, std::uint64_t b) {
    if (b > UINT64_MAX - a) invalid("address arithmetic overflow (" + std::to_string(a) + " + " + std::to_string(b) + ")");
    return a + b;
}

std::uint64_t add_signed(std::uint64_t address, std::int64_t amount) {
    if (amount >= 0) return add(address, static_cast<std::uint64_t>(amount));
    const auto magnitude = static_cast<std::uint64_t>(-(amount + 1)) + 1;
    if (address < magnitude) invalid("negative binding addend underflows address");
    return address - magnitude;
}

std::int64_t sum_signed(std::int64_t a, std::int64_t b) {
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) {
        invalid("binding addend overflow");
    }
    return a + b;
}

// A logical byte range backed by `data`, except for an optional omitted
// range [hole_offset, hole_offset + hole_size) that was never read. Bounds
// checks use the logical size; reading an omitted byte fails explicitly.
class Bytes {
public:
    explicit Bytes(std::span<const std::uint8_t> data, std::uint64_t hole_offset = 0,
                   std::uint64_t hole_size = 0)
        : data_(data), hole_offset_(hole_size != 0 ? hole_offset : 0), hole_size_(hole_size) {
        if (hole_size_ != 0 && hole_offset_ > data_.size()) invalid("omitted range lies outside data");
    }
    [[nodiscard]] std::uint64_t size() const { return data_.size() + hole_size_; }
    [[nodiscard]] std::span<const std::uint8_t> physical() const { return data_; }
    [[nodiscard]] std::uint64_t hole_offset() const { return hole_offset_; }
    [[nodiscard]] std::uint64_t hole_size() const { return hole_size_; }
    [[nodiscard]] bool overlaps_hole(std::uint64_t offset, std::uint64_t length) const {
        return hole_size_ != 0 && length != 0 && offset < hole_offset_ + hole_size_ &&
               hole_offset_ < offset + length;
    }
    void require(std::uint64_t offset, std::uint64_t length) const {
        if (!fits(offset, length, size())) invalid("truncated or out-of-range data");
    }
    // The physical bytes of a logical range that contains no omitted byte.
    [[nodiscard]] std::span<const std::uint8_t> span(std::uint64_t at, std::uint64_t length) const {
        require(at, length);
        if (overlaps_hole(at, length)) invalid("read inside the omitted LC_SYMTAB string table");
        const auto start = hole_size_ != 0 && at >= hole_offset_ ? at - hole_size_ : at;
        return data_.subspan(static_cast<std::size_t>(start), static_cast<std::size_t>(length));
    }
    [[nodiscard]] std::uint8_t u8(std::uint64_t at) const {
        require(at, 1);
        if (hole_size_ != 0 && at >= hole_offset_) {
            if (at - hole_offset_ < hole_size_) invalid("read inside the omitted LC_SYMTAB string table");
            at -= hole_size_;
        }
        return data_[static_cast<std::size_t>(at)];
    }
    [[nodiscard]] std::uint16_t u16(std::uint64_t at) const {
        require(at, 2);
        return static_cast<std::uint16_t>(u8(at) | (u8(at + 1) << 8));
    }
    [[nodiscard]] std::uint32_t u32(std::uint64_t at) const {
        require(at, 4);
        return static_cast<std::uint32_t>(u16(at)) |
               (static_cast<std::uint32_t>(u16(at + 2)) << 16);
    }
    [[nodiscard]] std::uint64_t u64(std::uint64_t at) const {
        require(at, 8);
        return static_cast<std::uint64_t>(u32(at)) |
               (static_cast<std::uint64_t>(u32(at + 4)) << 32);
    }
    [[nodiscard]] std::string fixed_string(std::uint64_t at, std::uint64_t length) const {
        require(at, length);
        std::string result;
        for (std::uint64_t i = 0; i < length && u8(at + i) != 0; ++i) {
            result.push_back(static_cast<char>(u8(at + i)));
        }
        return result;
    }
    [[nodiscard]] std::string string(std::uint64_t at, std::uint64_t end) const {
        if (end > size() || at >= end) invalid("string offset is outside data");
        std::string result;
        while (at < end && result.size() <= 65536) {
            const auto c = u8(at++);
            if (c == 0) return result;
            result.push_back(static_cast<char>(c));
        }
        invalid("unterminated or excessive string");
    }
    [[nodiscard]] std::uint64_t uleb(std::uint64_t& at, std::uint64_t end) const {
        std::uint64_t value = 0;
        for (unsigned shift = 0; shift < 64; shift += 7) {
            if (at >= end) invalid("truncated export ULEB128");
            const auto c = u8(at++);
            const auto bits = static_cast<std::uint64_t>(c & 0x7f);
            if (shift == 63 && bits > 1) invalid("export ULEB128 overflow");
            value |= bits << shift;
            if ((c & 0x80) == 0) return value;
        }
        invalid("export ULEB128 overflow");
    }
    [[nodiscard]] std::int64_t sleb(std::uint64_t& at, std::uint64_t end) const {
        std::uint64_t value = 0;
        unsigned shift = 0;
        for (; shift < 64; shift += 7) {
            if (at >= end) invalid("truncated bind SLEB128");
            const auto c = u8(at++);
            const auto bits = static_cast<std::uint64_t>(c & 0x7fU);
            if (shift == 63 && bits != 0 && bits != 0x7fU) invalid("bind SLEB128 overflow");
            value |= bits << shift;
            if ((c & 0x80U) == 0) {
                if (shift < 57 && (c & 0x40U) != 0) value |= UINT64_MAX << (shift + 7);
                return std::bit_cast<std::int64_t>(value);
            }
        }
        invalid("bind SLEB128 overflow");
    }
    [[nodiscard]] Bytes slice(std::uint64_t at, std::uint64_t length) const {
        require(at, length);
        if (!overlaps_hole(at, length)) return Bytes(span(at, length));
        const auto hole_end = hole_offset_ + hole_size_;
        const auto start = std::min(at, hole_offset_);
        const auto inner_offset = hole_offset_ > at ? hole_offset_ - at : 0;
        const auto inner_size = std::min(hole_end, at + length) - at - inner_offset;
        return Bytes(data_.subspan(static_cast<std::size_t>(start),
                                   static_cast<std::size_t>(length - inner_size)),
                     inner_offset, inner_size);
    }

private:
    std::span<const std::uint8_t> data_;
    std::uint64_t hole_offset_{};
    std::uint64_t hole_size_{};
};

std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment) {
    return add(value, alignment - 1) & ~(alignment - 1);
}

// Locates an LC_SYMTAB string table that load() may leave on disk: one at least
// omit_string_table_threshold bytes inside the selected ARM64 slice's
// __LINKEDIT. The omitted range starts at a 16 KiB boundary of __LINKEDIT, so
// the mapped head keeps every byte before it. Malformed inputs omit nothing;
// parse() then reports them with full validation.
std::pair<std::uint64_t, std::uint64_t> omittable_string_table(
    const std::function<bool(std::uint64_t, std::uint64_t, std::uint8_t*)>& read_at,
    std::uint64_t file_size) {
    const auto le32 = [](const std::uint8_t* p) {
        return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
               (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
    };
    const auto le64 = [&](const std::uint8_t* p) {
        return static_cast<std::uint64_t>(le32(p)) | (static_cast<std::uint64_t>(le32(p + 4)) << 32);
    };
    const auto be32 = [](const std::uint8_t* p) {
        return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
               (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
    };
    const std::pair<std::uint64_t, std::uint64_t> none{0, 0};
    std::array<std::uint8_t, 8> head{};
    if (file_size < 32 || !read_at(0, 8, head.data())) return none;
    std::uint64_t slice = 0, slice_size = file_size;
    const auto magic = be32(head.data());
    if (magic == 0xcafebabeU || magic == 0xcafebabfU) {
        const bool wide = magic == 0xcafebabfU;
        const auto count = be32(head.data() + 4);
        const auto stride = wide ? 32U : 20U;
        if (count == 0 || count > 128) return none;
        std::vector<std::uint8_t> table(static_cast<std::size_t>(count) * stride);
        if (!fits(8, table.size(), file_size) || !read_at(8, table.size(), table.data())) return none;
        bool found = false;
        for (std::uint32_t index = 0; index < count; ++index) {
            const auto* entry = table.data() + static_cast<std::size_t>(index) * stride;
            if (be32(entry) != 0x0100000cU || be32(entry + 4) != 0) continue;
            slice = wide ? (static_cast<std::uint64_t>(be32(entry + 8)) << 32) | be32(entry + 12) : be32(entry + 8);
            slice_size = wide ? (static_cast<std::uint64_t>(be32(entry + 16)) << 32) | be32(entry + 20) : be32(entry + 12);
            found = true;
            break;
        }
        if (!found || !fits(slice, slice_size, file_size)) return none;
    }
    std::array<std::uint8_t, 32> header{};
    if (slice_size < 32 || !read_at(slice, 32, header.data()) || le32(header.data()) != 0xfeedfacfU) return none;
    const auto command_count = le32(header.data() + 16);
    const auto command_size = le32(header.data() + 20);
    if (command_count > 4096 || !fits(32, command_size, slice_size)) return none;
    std::vector<std::uint8_t> commands(command_size);
    if (command_size != 0 && !read_at(slice + 32, command_size, commands.data())) return none;
    std::optional<std::pair<std::uint64_t, std::uint64_t>> linkedit, strings, symbols;
    std::uint64_t at = 0;
    for (std::uint32_t index = 0; index < command_count; ++index) {
        if (!fits(at, 8, command_size)) return none;
        const auto* command = commands.data() + at;
        const auto cmd = le32(command);
        const auto size = le32(command + 4);
        if (size < 8 || !fits(at, size, command_size)) return none;
        if (cmd == 0x19 && size >= 72 &&
            std::string_view(reinterpret_cast<const char*>(command + 8), 10) == "__LINKEDIT" && command[18] == 0) {
            linkedit = std::pair{le64(command + 40), le64(command + 48)};
        } else if (cmd == 0x2 && size == 24) {
            symbols = std::pair{static_cast<std::uint64_t>(le32(command + 8)),
                                static_cast<std::uint64_t>(le32(command + 12))};
            strings = std::pair{static_cast<std::uint64_t>(le32(command + 16)),
                                static_cast<std::uint64_t>(le32(command + 20))};
        }
        at += size;
    }
    if (!linkedit || !strings || strings->second < omit_string_table_threshold) return none;
    const auto [segment_offset, segment_size] = *linkedit;
    const auto [string_offset, string_size] = *strings;
    if (!fits(segment_offset, segment_size, slice_size) || string_offset < segment_offset ||
        !fits(string_offset - segment_offset, string_size, segment_size)) return none;
    // Keep every name the symbol table references (they precede appended
    // data), so that only bytes no nlist entry names are left on disk.
    std::uint64_t used = 0;
    if (symbols->second != 0) {
        const auto table_size = symbols->second * 16;
        if (table_size > 64ULL * 1024 * 1024 || !fits(symbols->first, table_size, slice_size)) return none;
        std::vector<std::uint8_t> table(static_cast<std::size_t>(table_size));
        if (!read_at(slice + symbols->first, table_size, table.data())) return none;
        std::uint64_t last = 0;
        for (std::uint64_t index = 0; index < symbols->second; ++index) {
            last = std::max<std::uint64_t>(last, le32(table.data() + index * 16));
        }
        if (last >= string_size) return none;
        std::array<std::uint8_t, 4096> name{};
        const auto window = std::min<std::uint64_t>(name.size(), string_size - last);
        if (!read_at(slice + string_offset + last, window, name.data())) return none;
        const auto terminator = std::find(name.begin(), name.begin() + window, 0);
        if (terminator == name.begin() + window) return none;
        used = last + static_cast<std::uint64_t>(terminator - name.begin()) + 1;
    }
    const auto start = segment_offset + align_up(string_offset + used - segment_offset, omitted_page);
    const auto end = string_offset + string_size;
    if (start >= end) return none;
    return {slice + start, end - start};
}

bool pagezero(const MachOSegment& segment) { return segment.name == "__PAGEZERO"; }

bool contains(const MachOSegment& segment, std::uint64_t address, std::uint64_t size) {
    return !pagezero(segment) && address >= segment.address &&
           fits(address - segment.address, size, segment.size);
}

std::uint64_t relocated(const MachOSegment& segment, std::uint64_t preferred,
                        std::uint64_t base) {
    if (segment.address < preferred) invalid("segment precedes image base");
    return add(base, segment.address - preferred);
}

void valid_load_base(std::uint64_t base, std::uint64_t size) {
    if (base < 0x10000 || (base & 0xfffU) != 0 || size == 0 ||
        base > UINT64_MAX - size || base + size > (1ULL << 56)) {
        invalid("load base must be page aligned and fit the guest address space");
    }
}

} // namespace

std::optional<std::string> macho_uuid(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    const auto read = [&](std::uint64_t offset, std::size_t size) {
        std::vector<std::uint8_t> bytes(size);
        input.clear();
        input.seekg(static_cast<std::streamoff>(offset));
        if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) bytes.clear();
        return bytes;
    };
    const auto u32 = [](const std::vector<std::uint8_t>& b, std::size_t at, bool big = false) {
        const std::uint32_t v = b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (static_cast<std::uint32_t>(b[at + 3]) << 24);
        return big ? ((v & 0xffU) << 24) | ((v & 0xff00U) << 8) | ((v >> 8) & 0xff00U) | (v >> 24) : v;
    };
    std::uint64_t base = 0;
    auto header = read(0, 32);
    if (header.size() < 32) return std::nullopt;
    if (u32(header, 0) == 0xbebafecaU) {   // universal, 32-bit entries (big endian)
        const auto count = u32(header, 4, true);
        const auto entries = read(8, 20ULL * std::min<std::uint32_t>(count, 128));
        for (std::uint32_t i = 0; i < std::min<std::uint32_t>(count, 128) && entries.size() >= 20ULL * (i + 1); ++i)
            if (u32(entries, i * 20, true) == 0x0100000c) { base = u32(entries, i * 20 + 8, true); break; }
        if (!base) return std::nullopt;
        header = read(base, 32);
        if (header.size() < 32) return std::nullopt;
    }
    if (u32(header, 0) != 0xfeedfacfU) return std::nullopt;
    const auto count = u32(header, 16), size = u32(header, 20);
    if (size > (16U << 20)) return std::nullopt;
    const auto commands = read(base + 32, size);
    for (std::size_t at = 0, index = 0; index < count && at + 8 <= commands.size(); ++index) {
        const auto kind = u32(commands, at), length = u32(commands, at + 4);
        if (length < 8) break;
        if (kind == 0x1b && length >= 24 && at + 24 <= commands.size()) {
            static constexpr char digits[] = "0123456789abcdef";
            std::string text;
            for (std::size_t i = 0; i < 16; ++i) {
                text += digits[commands[at + 8 + i] >> 4];
                text += digits[commands[at + 8 + i] & 15];
            }
            return text;
        }
        at += length;
    }
    return std::nullopt;
}

// A read-only view of an image file and its host section (map_from_file
// maps segments from the section).
struct MachOImage::MappedFile {
    host::OwnedHandle section;
    std::uintptr_t view{};
    MappedFile() = default;
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    ~MappedFile() {
        if (view) static_cast<void>(host::sections::unmap(view));
    }
};

MachOImage MachOImage::load(const std::filesystem::path& path) {
    // Positional reads straight into the final buffer (an ifstream reads a
    // 175 MB image in small chunks through its own buffer).
    const auto opened = host::files::open(path_utf8(path), host::files::Access::read, host::files::Create::open_existing,
                                      host::files::open_sequential);
    if (!opened) invalid("cannot open " + path.string());
    const host::OwnedHandle file(*opened);
    const auto size = host::files::size(file.get());
    if (!size) invalid("invalid file size");
    const auto file_size = *size;
    const auto read_at = [&](std::uint64_t offset, std::uint64_t length, std::uint8_t* out) {
        if (!fits(offset, length, file_size)) return false;
        while (length != 0) {
            const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(length, 1ULL << 30));
            const auto read = host::files::read_at(file.get(), host::Bytes(reinterpret_cast<std::byte*>(out), chunk), offset);
            if (!read || *read == 0) return false;
            out += *read;
            offset += *read;
            length -= *read;
        }
        return true;
    };
    const auto [hole_offset, hole_size] = omittable_string_table(read_at, file_size);
    const auto loaded = file_size - hole_size;
    if (loaded > maximum_loaded_bytes) {
        invalid("invalid or excessive file size (" + std::to_string(file_size) + " bytes, " +
                std::to_string(loaded) + " needed in memory; limit " +
                std::to_string(maximum_loaded_bytes) + ")");
    }
    if (hole_size == 0 && file_size != 0) {
        // A read-only view: the file's own pages, shared with every process
        // viewing the file, instead of a private copy.
        if (const auto section = host::sections::create_file_section(file.get(), false)) {
            auto mapped = std::make_shared<MappedFile>();
            mapped->section = host::OwnedHandle(*section);
            if (const auto view = host::sections::map(mapped->section.get(), 0, static_cast<std::size_t>(file_size), false)) {
                mapped->view = *view;
                const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(*view),
                                                          static_cast<std::size_t>(file_size));
                return parse_omitting(bytes, 0, 0, path, nullptr, mapped);
            }
        }
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(loaded));
    const auto head = hole_size != 0 ? hole_offset : file_size;
    if ((head != 0 && !read_at(0, head, bytes.data())) ||
        (hole_size != 0 && loaded != head &&
         !read_at(hole_offset + hole_size, loaded - head, bytes.data() + head))) {
        invalid("cannot read entire file");
    }
    const std::span<const std::uint8_t> view(bytes);
    return parse_omitting(view, hole_offset, hole_size, path, &bytes);
}

MachOImage MachOImage::parse(std::span<const std::uint8_t> bytes,
                            std::filesystem::path source) {
    return parse_omitting(bytes, 0, 0, std::move(source), nullptr);
}

MachOImage MachOImage::parse_omitting(std::span<const std::uint8_t> bytes,
                                      std::uint64_t hole_offset, std::uint64_t hole_size,
                                      std::filesystem::path source, std::vector<std::uint8_t>* owned,
                                      const std::shared_ptr<const MappedFile>& file_view) {
    const Bytes file(bytes, hole_offset, hole_size);
    file.require(0, 8);
    const auto magic = file.u32(0);
    if (magic == 0xbebafecaU || magic == 0xbfbafecaU ||
        magic == 0xcafebabeU || magic == 0xcafebabfU) {
        const bool big_endian = magic == 0xbebafecaU || magic == 0xbfbafecaU;
        const bool wide = magic == 0xbfbafecaU || magic == 0xcafebabfU;
        const auto number32 = [&](std::uint64_t offset) {
            const auto value = file.u32(offset);
            return big_endian ? ((value & 0xffU) << 24) | ((value & 0xff00U) << 8) |
                ((value >> 8) & 0xff00U) | (value >> 24) : value;
        };
        const auto number64 = [&](std::uint64_t offset) {
            return big_endian ? (static_cast<std::uint64_t>(number32(offset)) << 32) |
                number32(offset + 4) : file.u64(offset);
        };
        const auto count = number32(4);
        const auto stride = wide ? 32ULL : 20ULL;
        if (count == 0 || count > 128) invalid("invalid universal architecture count");
        file.require(8, stride * count);
        std::optional<std::pair<std::uint64_t, std::uint64_t>> selected;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> slices;
        for (std::uint32_t index = 0; index < count; ++index) {
            const auto pos = 8 + stride * index;
            const auto offset = wide ? number64(pos + 8) : number32(pos + 8);
            const auto size = wide ? number64(pos + 16) : number32(pos + 12);
            const auto alignment = number32(pos + (wide ? 24U : 16U));
            if (alignment > 31 || size < 32 || offset < 8 + stride * count ||
                (offset & ((1ULL << alignment) - 1)) != 0) invalid("invalid universal slice layout");
            file.require(offset, size);
            for (const auto& [prior, length] : slices) {
                if (offset < prior + length && prior < offset + size) invalid("overlapping universal slices");
            }
            slices.emplace_back(offset, size);
            if (wide && number32(pos + 28) != 0) invalid("reserved universal slice bits set");
            if (number32(pos) == 0x0100000c && number32(pos + 4) == 0) {
                if (selected) invalid("duplicate plain ARM64 universal slice");
                selected = {offset, size};
            }
        }
        if (!selected) invalid("universal image contains no plain ARM64 slice");
        if (file.u32(selected->first) != 0xfeedfacfU) invalid("universal ARM64 slice is not thin Mach-O");
        // Only the selected slice may contain an omitted range.
        if (file.overlaps_hole(0, selected->first) ||
            file.overlaps_hole(selected->first + selected->second,
                               file.size() - selected->first - selected->second)) {
            invalid("omitted range lies outside the ARM64 slice");
        }
        const auto slice = file.slice(selected->first, selected->second);
        return parse_omitting(slice.physical(), slice.hole_offset(), slice.hole_size(), std::move(source), owned,
                              file_view);
    }
    file.require(0, 32);
    if (file.u32(0) != 0xfeedfacf) invalid("expected a thin little-endian 64-bit image");
    if (file.u32(4) != 0x0100000c) invalid("only ARM64 images are supported");
    if (file.u32(8) != 0) invalid("only plain ARM64 is supported; ARM64e is unsupported");
    const auto type = file.u32(12);
    if (type != 2 && type != 6 && type != 8) invalid("expected executable, dylib, or bundle file type");
    const auto command_count = file.u32(16);
    const auto command_size = file.u32(20);
    if (command_count > 4096 || command_count > command_size / 8) {
        invalid("invalid load-command count");
    }
    if (file.overlaps_hole(0, 32ULL + command_size)) invalid("omitted range overlaps load commands");
    file.require(32, command_size);
    constexpr std::uint32_t unsupported_flags = 0x80000000U |
        0x00020000U | 0x00000040U | 0x00000020U | 0x00000010U;
    if ((file.u32(24) & unsupported_flags) != 0) invalid("unsupported image header flags");

    MachOImage image;
    image.source_ = std::move(source);
    if (file_view) {
        image.file_ = file_view;
        image.bytes_ = bytes;
        image.file_offset_ = static_cast<std::uint64_t>(bytes.data() - reinterpret_cast<const std::uint8_t*>(file_view->view));
    } else {
        // Moving `owned` keeps its buffer: `bytes` and `file` stay valid.
        auto kept = owned && owned->data() == bytes.data() && owned->size() == bytes.size()
            ? std::make_shared<const std::vector<std::uint8_t>>(std::move(*owned))
            : std::make_shared<const std::vector<std::uint8_t>>(bytes.begin(), bytes.end());
        image.bytes_ = *kept;
        image.owned_ = std::move(kept);
    }
    image.flat_namespace_ = (file.u32(24) & 0x100U) != 0 || (file.u32(24) & 0x80U) == 0;
    image.pie_ = (file.u32(24) & 0x200000U) != 0;
    image.omitted_offset_ = file.hole_offset();
    image.omitted_size_ = file.hole_size();
    image.dylib_ = type == 6;
    image.bundle_ = type == 8;
    std::optional<std::uint64_t> entry_offset;
    std::optional<std::pair<std::uint64_t, std::uint64_t>> export_data;
    bool has_fixups = false;
    bool has_version = false;
    bool has_identity = false;
    std::optional<std::array<std::pair<std::uint64_t, std::uint64_t>, 4>> classic_data;
    auto at = std::uint64_t{32};
    const auto commands_end = 32ULL + command_size;
    for (std::uint32_t i = 0; i < command_count; ++i) {
        if (!fits(at, 8, commands_end)) invalid("truncated load command");
        const auto cmd = file.u32(at);
        const auto size = file.u32(at + 4);
        if (size < 8 || (size & 7U) != 0 || !fits(at, size, commands_end)) {
            invalid("invalid load-command size");
        }
        const auto command = file.slice(at, size);
        const auto exact_size = [&](std::uint64_t expected) {
            if (size != expected) invalid("invalid known load-command size");
        };
        const auto command_string = [&](std::uint64_t minimum) {
            const auto offset = command.u32(8);
            if (offset < minimum) invalid("load-command string overlaps header");
            return command.string(offset, command.size());
        };
        const auto linkedit_range = [&] {
            exact_size(16);
            const auto offset = command.u32(8);
            const auto length = command.u32(12);
            file.require(offset, length);
            return std::pair<std::uint64_t, std::uint64_t>{offset, length};
        };
        switch (cmd) {
        case segment_64: {
            command.require(0, 72);
            const auto section_count = command.u32(64);
            if (section_count > 4096 || size != 72ULL + section_count * 80ULL) {
                invalid("invalid segment section count");
            }
            MachOSegment segment;
            segment.name = command.fixed_string(8, 16);
            segment.address = command.u64(24);
            segment.size = command.u64(32);
            segment.file_offset = command.u64(40);
            segment.file_size = command.u64(48);
            segment.maximum_permissions = command.u32(56);
            segment.initial_permissions = command.u32(60);
            segment.flags = command.u32(68);
            if (segment.name.empty() || segment.size < segment.file_size ||
                segment.address > UINT64_MAX - segment.size ||
                (segment.maximum_permissions & ~7U) != 0 ||
                (segment.initial_permissions & ~segment.maximum_permissions) != 0 ||
                (segment.flags & ~0x14U) != 0) {
                invalid("invalid or unsupported segment layout/permissions");
            }
            file.require(segment.file_offset, segment.file_size);
            // Only the end of a section-less __LINKEDIT may be omitted. Its
            // head is mapped; the rest is reserved address space, bounded by
            // the file (vmsize must not exceed the page-rounded file size).
            std::uint64_t committed_size = segment.size;
            if (file.overlaps_hole(segment.file_offset, segment.file_size)) {
                const auto mapped = file.hole_offset() - std::min(file.hole_offset(), segment.file_offset);
                if (segment.name != "__LINKEDIT" || section_count != 0 ||
                    file.hole_offset() < segment.file_offset || (mapped & 0xfffU) != 0 ||
                    image.reserved_tail_segment_ != SIZE_MAX ||
                    segment.size > align_up(segment.file_size, omitted_page)) {
                    invalid("omitted string table is not the page-aligned tail of a section-less __LINKEDIT");
                }
                image.reserved_tail_segment_ = image.segments_.size();
                image.reserved_tail_mapped_ = mapped;
                committed_size = mapped;
            }
            if (pagezero(segment)) {
                if (segment.address != 0 || segment.file_size != 0 ||
                    segment.initial_permissions != 0 || segment.maximum_permissions != 0 ||
                    section_count != 0 || segment.size == 0) invalid("invalid __PAGEZERO");
            } else if (segment.size != 0 &&
                       ((segment.address & 0xfffU) != 0 || committed_size > maximum_image_size)) {
                invalid("invalid segment alignment or size");
            }
            for (const auto& prior : image.segments_) {
                if (segment.name == prior.name) invalid("duplicate segment name");
                if (segment.size != 0 && prior.size != 0 &&
                    segment.address < prior.address + prior.size &&
                    prior.address < segment.address + segment.size) invalid("overlapping segments");
            }
            for (std::uint32_t s = 0; s < section_count; ++s) {
                const auto pos = 72ULL + s * 80ULL;
                MachOSection section;
                section.name = command.fixed_string(pos, 16);
                section.segment = command.fixed_string(pos + 16, 16);
                section.address = command.u64(pos + 32);
                section.size = command.u64(pos + 40);
                section.flags = command.u32(pos + 64);
                section.alignment = command.u32(pos + 52);
                const auto section_type = section.flags & 0xffU;
                if (section.segment != segment.name || section.name.empty() ||
                    !contains(segment, section.address, section.size) || command.u32(pos + 52) > 31) {
                    invalid("section extends outside its segment");
                }
                if (section_type == 0xd || section_type == 0x10 || section_type >= 0x17) {
                    invalid("unsupported execution-changing section type");
                }
                if (command.u32(pos + 60) != 0) invalid("classic section relocations unsupported");
                if (section_type != 1 && section_type != 0xc && section_type != 0x12) {
                    const auto offset = command.u32(pos + 48);
                    if (offset < segment.file_offset ||
                        !fits(offset - segment.file_offset, section.size, segment.file_size) ||
                        offset - segment.file_offset != section.address - segment.address) {
                        invalid("section file range disagrees with segment mapping");
                    }
                    file.require(offset, section.size);
                }
                if ((section_type == 9 || section.name == "__mod_init_func") &&
                    ((section.size & 7U) != 0 || (section.address & 7U) != 0)) {
                    invalid("misaligned initializer pointer section");
                }
                if (section_type == 0x16 &&
                    ((section.size & 3U) != 0 || (section.address & 3U) != 0)) {
                    invalid("misaligned initializer offset section");
                }
                if ((section_type == 0x13 && ((section.size % 24) != 0 || (section.address & 7U) != 0)) ||
                    ((section_type == 0x14 || section_type == 0x15 || section_type == 0xa) &&
                     ((section.size & 7U) != 0 || (section.address & 7U) != 0))) {
                    invalid("misaligned TLS or finalizer section");
                }
                for (const auto& prior : segment.sections) {
                    if (section.size != 0 && prior.size != 0 &&
                        section.address < prior.address + prior.size &&
                        prior.address < section.address + section.size) invalid("overlapping sections");
                }
                segment.sections.push_back(std::move(section));
            }
            image.segments_.push_back(std::move(segment));
            break;
        }
        case main_command:
            exact_size(24);
            if (entry_offset || image.dylib_ || image.bundle_) invalid("duplicate or non-executable LC_MAIN");
            image.stack_size_ = command.u64(16);
            entry_offset = command.u64(8);
            break;
        case 0xc: // LC_LOAD_DYLIB
        case 0x80000018: // LC_LOAD_WEAK_DYLIB
        case 0x8000001f: // LC_REEXPORT_DYLIB
        case 0xd: { // LC_ID_DYLIB
            command.require(0, 24);
            const auto name = command_string(24);
            if (name.empty()) invalid("empty dylib identity");
            if (cmd == 0xd) {
                if (!image.dylib_ || has_identity) invalid("duplicate or executable dylib identity");
                image.install_name_ = name;
                has_identity = true;
            } else {
                // LLD emits a load command and a reexport command for one
                // identity, but assigns that identity only one bind ordinal.
                const auto existing = std::find_if(image.dependencies_.begin(), image.dependencies_.end(),
                    [&](const auto& dependency) { return dependency.install_name == name; });
                if (existing == image.dependencies_.end()) {
                    image.dependencies_.push_back({name, cmd == 0x80000018, cmd == 0x8000001f});
                } else {
                    existing->weak = existing->weak && cmd == 0x80000018;
                    existing->reexport = existing->reexport || cmd == 0x8000001f;
                }
            }
            break;
        }
        case chained_fixups: {
            const auto [offset, length] = linkedit_range();
            if (has_fixups || length < 28) invalid("duplicate or truncated chained fixups");
            has_fixups = true;
            image.fixups_offset_ = offset;
            image.fixups_size_ = length;
            break;
        }
        case exports_trie: {
            const auto range = linkedit_range();
            if (export_data) invalid("duplicate export trie");
            export_data = range;
            break;
        }
        case 0x22:
        case 0x80000022: { // LC_DYLD_INFO[_ONLY]
            exact_size(48);
            if (classic_data) invalid("duplicate classic dyld info");
            classic_data.emplace();
            for (unsigned field = 8; field < 40; field += 8) {
                file.require(command.u32(field), command.u32(field + 4));
                (*classic_data)[(field - 8) / 8] = {command.u32(field), command.u32(field + 4)};
                image.classic_fixups_ |= command.u32(field + 4) != 0;
            }
            if (command.u32(44) != 0) {
                if (export_data) invalid("duplicate export trie");
                file.require(command.u32(40), command.u32(44));
                export_data = {command.u32(40), command.u32(44)};
            }
            break;
        }
        case 0x32: { // LC_BUILD_VERSION
            command.require(0, 24);
            const auto tools = command.u32(20);
            if (size != 24ULL + tools * 8ULL || command.u32(8) != 1 || has_version) {
                invalid("expected a single macOS build version");
            }
            has_version = true;
            image.minimum_os_ = command.u32(12);
            image.sdk_version_ = command.u32(16);
            break;
        }
        case 0x24: // LC_VERSION_MIN_MACOSX
            exact_size(16);
            if (has_version) invalid("duplicate macOS version metadata");
            has_version = true;
            image.minimum_os_ = command.u32(8);
            image.sdk_version_ = command.u32(12);
            break;
        case 0x2: // LC_SYMTAB, debugging metadata only
            exact_size(24);
            file.require(command.u32(8), static_cast<std::uint64_t>(command.u32(12)) * 16);
            file.require(command.u32(16), command.u32(20));
            break;
        case 0xb: // LC_DYSYMTAB
            exact_size(80);
            file.require(command.u32(56), static_cast<std::uint64_t>(command.u32(60)) * 4);
            if (command.u32(68) != 0 || command.u32(76) != 0) invalid("classic dynamic relocations unsupported");
            break;
        case 0xe: // LC_LOAD_DYLINKER: Alchemy performs dyld startup itself.
            command.require(0, 12);
            if (command_string(12) != "/usr/lib/dyld") invalid("custom dynamic linker unsupported");
            break;
        case 0x1b: exact_size(24); break; // LC_UUID
        case 0x2a: exact_size(16); break; // LC_SOURCE_VERSION
        case 0x1d: // LC_CODE_SIGNATURE, retained as metadata, never executed.
        case 0x26: // LC_FUNCTION_STARTS
        case 0x29: // LC_DATA_IN_CODE
        case 0x2b: // LC_DYLIB_CODE_SIGN_DRS
        case 0x2e: // LC_LINKER_OPTIMIZATION_HINT
            (void)linkedit_range();
            break;
        case 0x2c: // LC_ENCRYPTION_INFO_64
            exact_size(24);
            file.require(command.u32(8), command.u32(12));
            if (command.u32(16) != 0) invalid("encrypted Mach-O code unsupported");
            break;
        case 0x31: // LC_NOTE
            exact_size(40);
            file.require(command.u64(24), command.u64(32));
            break;
        case 0x2d: { // LC_LINKER_OPTION is link-time metadata.
            command.require(0, 12);
            auto pos = std::uint64_t{12};
            const auto count = command.u32(8);
            if (count > size - 12) invalid("invalid linker-option count");
            for (std::uint32_t option = 0; option < count; ++option) {
                pos += command.string(pos, size).size() + 1;
            }
            break;
        }
        case 0x8000001c: {
            command.require(0, 12);
            auto path = command_string(12);
            if (path.empty()) invalid("empty LC_RPATH");
            image.runpaths_.push_back(std::move(path));
            break;
        }
        case 0x80000023: invalid("LC_LOAD_UPWARD_DYLIB unsupported");
        case 0x20: invalid("LC_LAZY_LOAD_DYLIB unsupported");
        case 0x27: invalid("LC_DYLD_ENVIRONMENT unsupported");
        case 0x5: invalid("LC_UNIXTHREAD startup unsupported; use LC_MAIN");
        case 0x1a: invalid("LC_ROUTINES_64 initializer startup unsupported");
        case 0x1e: invalid("LC_SEGMENT_SPLIT_INFO unsupported");
        default:
            invalid("unsupported load command " + std::to_string(cmd));
        }
        at += size;
    }
    if (at != commands_end) invalid("load-command byte count mismatch");
    if (image.segments_.empty()) invalid("image has no segments");
    bool has_header_segment = false;
    auto base = UINT64_MAX;
    auto image_end = std::uint64_t{0};
    auto committed_end = std::uint64_t{0};
    for (std::size_t index = 0; index < image.segments_.size(); ++index) {
        const auto& segment = image.segments_[index];
        if (pagezero(segment) || segment.size == 0) continue;
        base = std::min(base, segment.address);
        image_end = std::max(image_end, segment.address + segment.size);
        committed_end = std::max(committed_end, segment.address +
            (index == image.reserved_tail_segment_ ? image.reserved_tail_mapped_ : segment.size));
        if (segment.file_offset == 0 && segment.file_size >= commands_end) {
            if (has_header_segment) invalid("multiple segments map the Mach-O header");
            has_header_segment = true;
            image.preferred_base_ = segment.address;
        }
    }
    if (!has_header_segment || base != image.preferred_base_ || image_end <= base ||
        committed_end < base || committed_end - base > maximum_image_size) {
        invalid("invalid image extent/header mapping");
    }
    if (file.hole_size() != 0 && image.reserved_tail_segment_ == SIZE_MAX) {
        invalid("omitted string table lies outside __LINKEDIT");
    }
    image.image_size_ = image_end - base;
    if (!image.dylib_ && !image.bundle_ && !entry_offset) invalid("executable has no LC_MAIN entry");
    if (image.dylib_ && !has_identity) invalid("dylib has no LC_ID_DYLIB identity");
    if (entry_offset) {
        for (const auto& segment : image.segments_) {
            if (!pagezero(segment) && (segment.initial_permissions & 4U) != 0 &&
                *entry_offset >= segment.file_offset &&
                fits(*entry_offset - segment.file_offset, 4, segment.file_size)) {
                image.entry_address_ = add(segment.address, *entry_offset - segment.file_offset);
                break;
            }
        }
        if (!image.entry_address_ || (*image.entry_address_ & 3U) != 0) {
            invalid("LC_MAIN entry is not aligned executable file-backed code");
        }
    }

    if (export_data && export_data->second != 0) {
        const auto trie = file.slice(export_data->first, export_data->second);
        struct Pending { std::uint64_t offset; std::string name; std::vector<std::uint64_t> ancestors; };
        std::vector<Pending> pending{{0, {}, {}}};
        std::set<std::string> names;
        std::size_t node_count = 0;
        std::uint64_t name_bytes = 0;
        while (!pending.empty()) {
            auto node = std::move(pending.back());
            pending.pop_back();
            name_bytes = add(name_bytes, node.name.size());
            if (++node_count > 2097152 || name_bytes > 128 * 1024 * 1024 ||
                node.ancestors.size() >= 1024 || node.name.size() > 65536) {
                invalid("excessive export trie");
            }
            if (std::find(node.ancestors.begin(), node.ancestors.end(), node.offset) != node.ancestors.end()) {
                invalid("cyclic export trie");
            }
            node.ancestors.push_back(node.offset);
            auto pos = node.offset;
            const auto terminal_size = trie.uleb(pos, trie.size());
            trie.require(pos, terminal_size);
            const auto terminal_end = pos + terminal_size;
            if (terminal_size != 0) {
                const auto flags = trie.uleb(pos, terminal_end);
                if ((flags & 0x10U) != 0) invalid("export resolvers unsupported");
                if ((flags & ~0xfULL) != 0 || (flags & 3U) == 3) invalid("unsupported export symbol kind/flags");
                MachOExport exported;
                exported.name = node.name;
                exported.weak_definition = (flags & 4U) != 0;
                exported.absolute = (flags & 3U) == 2;
                exported.thread_local_descriptor = (flags & 3U) == 1;
                if ((flags & 8U) != 0) {
                    if ((flags & 3U) != 0) invalid("reexport has incompatible symbol kind");
                    const auto ordinal = trie.uleb(pos, terminal_end);
                    if (ordinal == 0 || ordinal > image.dependencies_.size()) invalid("invalid reexport ordinal");
                    exported.reexport_ordinal = static_cast<std::uint32_t>(ordinal);
                    exported.imported_name = trie.string(pos, terminal_end);
                    pos += exported.imported_name.size() + 1;
                } else {
                    exported.offset = trie.uleb(pos, terminal_end);
                }
                if (pos != terminal_end || node.name.empty() || !names.insert(node.name).second) {
                    invalid("malformed or duplicate export terminal");
                }
                const auto address = exported.absolute ? exported.offset : add(image.preferred_base_, exported.offset);
                if (!exported.absolute && !exported.reexport_ordinal &&
                    !std::any_of(image.segments_.begin(), image.segments_.end(),
                    [&](const auto& segment) { return contains(segment, address, exported.thread_local_descriptor ? 24U : 1U); })) {
                    invalid("export address is outside mapped segments");
                }
                if (exported.thread_local_descriptor &&
                    !std::any_of(image.segments_.begin(), image.segments_.end(), [&](const auto& segment) {
                        return std::any_of(segment.sections.begin(), segment.sections.end(), [&](const auto& section) {
                            return (section.flags & 0xffU) == 0x13 && address >= section.address &&
                                fits(address - section.address, 24, section.size) &&
                                ((address - section.address) % 24) == 0;
                        });
                    })) invalid("thread-local export does not point to a TLS descriptor");
                image.exports_.push_back(std::move(exported));
            }
            pos = terminal_end;
            const auto children = trie.u8(pos++);
            for (std::uint32_t child = 0; child < children; ++child) {
                // Older ld64 emits an empty edge from a non-terminal node to the
                // terminal of the same name (Syphon.framework); dyld accepts it.
                // Offsets in `ancestors` still reject self-referencing edges.
                auto edge = trie.string(pos, trie.size());
                pos += edge.size() + 1;
                const auto child_offset = trie.uleb(pos, trie.size());
                trie.require(child_offset, 1);
                pending.push_back({child_offset, node.name + edge, node.ancestors});
            }
        }
    }

    if (has_fixups && image.classic_fixups_) invalid("mixed chained and classic fixups unsupported");
    if (classic_data && image.classic_fixups_) {
        std::map<std::uint64_t, std::size_t> written_locations;
        const auto destination = [&](std::uint32_t segment_index, std::uint64_t offset) {
            if (segment_index >= image.segments_.size()) invalid("invalid classic fixup segment index");
            const auto& segment = image.segments_[segment_index];
            if (pagezero(segment) || !fits(offset, 8, segment.size) ||
                !fits(offset, 8, segment.file_size)) invalid("classic fixup leaves file-backed segment");
            return add(segment.address - image.preferred_base_, offset);
        };
        const auto count_limit = [](std::uint64_t count) {
            if (count > 1048576) invalid("excessive classic fixup count");
        };
        const auto advance = [](std::uint64_t offset, std::uint64_t encoded, std::uint64_t extra = 0) {
            // ld can encode a backwards address step as a full-width unsigned
            // ULEB value. Apply the signed step without permitting underflow;
            // every emitted destination is still checked against its segment.
            if (encoded > INT64_MAX) return add_signed(offset,
                sum_signed(std::bit_cast<std::int64_t>(encoded), static_cast<std::int64_t>(extra)));
            return add(offset, add(encoded, extra));
        };
        const auto rebase_range = (*classic_data)[0];
        if (rebase_range.second != 0) {
            const auto stream = file.slice(rebase_range.first, rebase_range.second);
            std::uint64_t pos = 0, offset = 0;
            std::uint32_t segment_index = UINT32_MAX;
            unsigned type_value = 0;
            bool done = false;
            const auto emit = [&] {
                if (type_value != 1) invalid("unsupported classic rebase type; ARM64 pointers required");
                const auto location = destination(segment_index, offset);
                if (!written_locations.emplace(location, image.fixups_.size()).second) invalid("repeated classic rebase destination");
                const auto& segment = image.segments_[segment_index];
                const auto raw = file.u64(add(segment.file_offset, offset));
                const auto target = raw & 0x00ffffffffffffffULL;
                if (target < image.preferred_base_ ||
                    !std::any_of(image.segments_.begin(), image.segments_.end(),
                        [&](const auto& candidate) { return contains(candidate, target, 1); })) {
                    invalid("classic rebase target outside mapped image");
                }
                Fixup fixup;
                fixup.offset = location;
                fixup.target_offset = target - image.preferred_base_;
                fixup.high8 = static_cast<std::uint8_t>(raw >> 56);
                image.fixups_.push_back(fixup);
                if (image.fixups_.size() > 1048576) invalid("excessive classic fixups");
            };
            while (pos < stream.size() && !done) {
                const auto instruction = stream.u8(pos++);
                const auto immediate = instruction & 0xfU;
                switch (instruction & 0xf0U) {
                case 0x00: done = true; break;
                case 0x10: type_value = immediate; break;
                case 0x20: segment_index = immediate; offset = stream.uleb(pos, stream.size()); break;
                case 0x30: offset = advance(offset, stream.uleb(pos, stream.size())); break;
                case 0x40: offset = add(offset, immediate * 8ULL); break;
                case 0x50:
                    for (unsigned count = 0; count < immediate; ++count) { emit(); offset = add(offset, 8); }
                    break;
                case 0x60: {
                    const auto count = stream.uleb(pos, stream.size());
                    count_limit(count);
                    for (std::uint64_t index = 0; index < count; ++index) { emit(); offset = add(offset, 8); }
                    break;
                }
                case 0x70:
                    emit(); offset = advance(offset, stream.uleb(pos, stream.size()), 8); break;
                case 0x80: {
                    const auto count = stream.uleb(pos, stream.size());
                    const auto skip = stream.uleb(pos, stream.size());
                    count_limit(count);
                    for (std::uint64_t index = 0; index < count; ++index) { emit(); offset = advance(offset, skip, 8); }
                    break;
                }
                default: invalid("unsupported classic rebase opcode");
                }
            }
            // A complete program may end at the declared stream boundary.
            // Operand reads remain bounded even when DONE is omitted.
            while (pos < stream.size()) if (stream.u8(pos++) != 0) invalid("data after classic rebase terminator");
        }
        for (std::size_t stream_index = 1; stream_index < 4; ++stream_index) {
            const auto range = (*classic_data)[stream_index];
            if (range.second == 0) continue;
            const auto stream = file.slice(range.first, range.second);
            const bool weak_stream = stream_index == 2;
            const bool lazy_stream = stream_index == 3;
            std::uint64_t pos = 0, offset = 0;
            std::uint32_t segment_index = UINT32_MAX;
            std::int32_t ordinal = weak_stream ? -3 : 0;
            std::int64_t addend = 0;
            std::string symbol;
            unsigned type_value = 1, symbol_flags = 0;
            bool done = false;
            bool lazy_entry_open = false;
            const auto emit = [&] {
                if (type_value != 1) invalid("unsupported classic bind type; ARM64 pointers required");
                if (symbol.empty()) invalid("classic bind has no symbol");
                if (ordinal < -3 || (ordinal > 0 && static_cast<std::size_t>(ordinal) > image.dependencies_.size())) {
                    invalid("invalid classic bind library ordinal");
                }
                const auto location = destination(segment_index, offset);
                const auto [previous_location, inserted] = written_locations.emplace(location, image.fixups_.size());
                bool keep_previous = false;
                if (!inserted) {
                    const auto& previous_fixup = image.fixups_[previous_location->second];
                    // Lazy slots initially point to rebased stub helpers. Eager
                    // binding replaces those rebases; weak binding may coalesce
                    // an existing ordinary binding.
                    if (previous_fixup.import) {
                        const auto& previous = image.imports_[*previous_fixup.import];
                        const bool same_symbol = previous.symbol == symbol && previous.addend == addend;
                        const bool identical = same_symbol && previous.library_ordinal == ordinal;
                        keep_previous = lazy_stream && same_symbol && previous.library_ordinal == -3;
                        if (!weak_stream && !identical && !keep_previous) {
                            invalid("conflicting classic binding destination at " + std::to_string(location) +
                                    " (" + previous.symbol + " -> " + symbol + ")");
                        }
                    }
                }
                if (image.imports_.size() >= 1048576 || image.fixups_.size() >= 1048576) invalid("excessive classic bindings");
                const auto index = static_cast<std::uint32_t>(image.imports_.size());
                // The 64-bit SLEB addend acts on the pointer pattern, as with
                // chained ADDEND64: Apple arm64 RTTI sets bit 63 of non-unique
                // type-name pointers through an addend of INT64_MIN.
                image.imports_.push_back({ordinal, symbol, weak_stream || (symbol_flags & 1U) != 0, addend, true});
                Fixup fixup;
                fixup.offset = location;
                fixup.import = index;
                if (inserted) image.fixups_.push_back(fixup);
                else if (!keep_previous) image.fixups_[previous_location->second] = fixup;
            };
            while (pos < stream.size() && !done) {
                const auto instruction = stream.u8(pos++);
                const auto immediate = instruction & 0xfU;
                if ((instruction & 0xf0U) != 0) lazy_entry_open = true;
                switch (instruction & 0xf0U) {
                case 0x00:
                    if (!lazy_stream) done = true;
                    else { offset = 0; segment_index = UINT32_MAX; ordinal = 0; addend = 0; symbol.clear(); symbol_flags = 0; type_value = 1; lazy_entry_open = false; }
                    break;
                case 0x10: ordinal = static_cast<std::int32_t>(immediate); break;
                case 0x20: {
                    const auto value = stream.uleb(pos, stream.size());
                    if (value > INT32_MAX) invalid("classic bind ordinal overflow");
                    ordinal = static_cast<std::int32_t>(value);
                    break;
                }
                case 0x30: ordinal = immediate == 0 ? 0 : static_cast<std::int32_t>(immediate) - 16; break;
                case 0x40:
                    if ((immediate & ~9U) != 0) invalid("unsupported classic bind symbol flags");
                    symbol_flags = immediate;
                    symbol = stream.string(pos, stream.size()); pos += symbol.size() + 1; break;
                case 0x50: type_value = immediate; break;
                case 0x60: addend = stream.sleb(pos, stream.size()); break;
                case 0x70: segment_index = immediate; offset = stream.uleb(pos, stream.size()); break;
                case 0x80: offset = advance(offset, stream.uleb(pos, stream.size())); break;
                case 0x90: emit(); offset = add(offset, 8); break;
                case 0xa0: emit(); offset = advance(offset, stream.uleb(pos, stream.size()), 8); break;
                case 0xb0: emit(); offset = add(offset, 8ULL + immediate * 8ULL); break;
                case 0xc0: {
                    const auto count = stream.uleb(pos, stream.size());
                    const auto skip = stream.uleb(pos, stream.size());
                    count_limit(count);
                    for (std::uint64_t index = 0; index < count; ++index) { emit(); offset = advance(offset, skip, 8); }
                    break;
                }
                case 0xd0: invalid("threaded classic binding opcode unsupported");
                default: invalid("unsupported classic binding opcode");
                }
            }
            // The explicit dyld-info size is also a valid program boundary.
            if (lazy_stream && lazy_entry_open) invalid("unterminated lazy bind entry");
            while (pos < stream.size()) if (stream.u8(pos++) != 0) invalid("data after classic bind terminator");
        }
    }

    if (has_fixups) {
        const auto data = file.slice(image.fixups_offset_, image.fixups_size_);
        if (data.u32(0) != 0) invalid("unsupported chained-fixups version");
        const auto starts = data.u32(4);
        const auto imports = data.u32(8);
        const auto symbols = data.u32(12);
        const auto import_count = data.u32(16);
        const auto import_format = data.u32(20);
        if (data.u32(24) != 0) invalid("compressed chained-fixup symbols unsupported");
        if (import_count > 1048576 || starts < 28 || imports < 28 || symbols < 28) {
            invalid("invalid chained-fixups offsets/count");
        }
        const auto import_size = import_format == 1 ? 4ULL : import_format == 2 ? 8ULL :
                                 import_format == 3 ? 16ULL : 0ULL;
        if (import_size == 0) invalid("unsupported chained import format");
        data.require(imports, import_size * import_count);
        data.require(symbols, 0);
        if (imports > symbols || import_size * import_count > symbols - imports) {
            invalid("chained imports overlap symbol strings");
        }
        for (std::uint32_t i = 0; i < import_count; ++i) {
            const auto pos = imports + i * import_size;
            const auto raw = import_format == 3 ? data.u64(pos) : data.u32(pos);
            const auto ordinal_bits = import_format == 3 ? 16U : 8U;
            const auto mask = (1U << ordinal_bits) - 1;
            const auto ordinal_raw = static_cast<std::uint32_t>(raw) & mask;
            // Positive ordinals occupy all but the top fifteen encodings.
            const auto ordinal = ordinal_raw > mask - 15
                ? static_cast<std::int32_t>(ordinal_raw) - static_cast<std::int32_t>(mask + 1)
                : static_cast<std::int32_t>(ordinal_raw);
            if (ordinal < -3 || (ordinal > 0 && static_cast<std::size_t>(ordinal) > image.dependencies_.size())) {
                invalid("invalid chained import library ordinal");
            }
            const bool weak = ((raw >> ordinal_bits) & 1U) != 0;
            const auto name_offset = import_format == 3 ? raw >> 32 : raw >> 9;
            if (import_format == 3 && ((raw >> 17) & 0x7fffU) != 0) invalid("reserved chained import bits set");
            auto name = data.string(add(symbols, name_offset), data.size());
            if (name.empty()) invalid("empty imported symbol name");
            auto addend = std::int64_t{0};
            if (import_format == 2) addend = std::bit_cast<std::int32_t>(data.u32(pos + 4));
            if (import_format == 3) addend = std::bit_cast<std::int64_t>(data.u64(pos + 8));
            image.imports_.push_back({ordinal, std::move(name), weak, addend, import_format == 3});
        }
        data.require(starts, 4);
        const auto segment_count = data.u32(starts);
        if (segment_count != image.segments_.size()) invalid("chained segment count differs from load commands");
        data.require(starts + 4ULL, segment_count * 4ULL);
        std::set<std::uint64_t> fixed_locations;
        for (std::uint32_t index = 0; index < segment_count; ++index) {
            const auto info_offset = data.u32(starts + 4ULL + index * 4ULL);
            if (info_offset == 0) continue;
            if (info_offset < 4ULL + segment_count * 4ULL) invalid("chained segment starts overlap offset table");
            const auto info_pos = add(starts, info_offset);
            data.require(info_pos, 22);
            const auto info_size = data.u32(info_pos);
            if (info_size < 22 || (info_size & 1U) != 0) invalid("invalid chained segment starts size");
            const auto info = data.slice(info_pos, info_size);
            const auto page_size = info.u16(4);
            const auto format = info.u16(6);
            const auto segment_offset = info.u64(8);
            const auto page_count = info.u16(20);
            const auto& segment = image.segments_[index];
            if (pagezero(segment) || segment.address < image.preferred_base_ ||
                segment_offset != segment.address - image.preferred_base_ ||
                (page_size != 4096 && page_size != 16384) || info.u32(16) != 0 ||
                page_count > (segment.size + page_size - 1) / page_size) {
                invalid("chained starts do not match segment/page layout");
            }
            if (format != 2 && format != 6) invalid("unsupported chained pointer format " + std::to_string(format));
            if (std::find(image.pointer_formats_.begin(), image.pointer_formats_.end(), format) == image.pointer_formats_.end()) {
                image.pointer_formats_.push_back(format);
            }
            info.require(22, page_count * 2ULL);
            const auto walk = [&](std::uint32_t page, std::uint16_t start) {
                if ((start & 3U) != 0 || start >= page_size) invalid("invalid chain page start");
                const auto page_offset = static_cast<std::uint64_t>(page) * page_size;
                auto offset = add(page_offset, start);
                while (true) {
                    if (!fits(offset - page_offset, 8, page_size) ||
                        !fits(offset, 8, segment.file_size) || !fits(offset, 8, segment.size)) {
                        invalid("fixup chain leaves its page or file-backed segment");
                    }
                    const auto location = add(segment_offset, offset);
                    if (!fixed_locations.insert(location).second) invalid("overlapping or repeated fixup chain");
                    const auto raw = file.u64(add(segment.file_offset, offset));
                    Fixup fixup;
                    fixup.offset = location;
                    if ((raw >> 63) != 0) {
                        if (((raw >> 32) & 0x7ffffU) != 0) invalid("reserved chained binding bits set");
                        const auto import_index = static_cast<std::uint32_t>(raw & 0xffffffU);
                        if (import_index >= image.imports_.size()) invalid("invalid chained binding import index");
                        fixup.import = import_index;
                        fixup.addend = static_cast<std::int64_t>((raw >> 24) & 0xffU);
                    } else {
                        if (((raw >> 44) & 0x7fU) != 0) invalid("reserved chained rebasing bits set");
                        const auto target = raw & 0xfffffffffULL;
                        fixup.high8 = static_cast<std::uint8_t>((raw >> 36) & 0xffU);
                        if (format == 2 && target < image.preferred_base_) invalid("rebasing target precedes preferred base");
                        fixup.target_offset = format == 6 ? target : target - image.preferred_base_;
                        const auto address = add(image.preferred_base_, fixup.target_offset);
                        if (!std::any_of(image.segments_.begin(), image.segments_.end(),
                            [&](const auto& candidate) { return contains(candidate, address, 1); })) {
                            invalid("rebasing target outside image segments");
                        }
                    }
                    image.fixups_.push_back(fixup);
                    const auto next = (raw >> 51) & 0xfffU;
                    if (next == 0) break;
                    offset = add(offset, next * 4);
                }
            };
            for (std::uint32_t page = 0; page < page_count; ++page) {
                const auto start = info.u16(22ULL + page * 2ULL);
                if (start == 0xffff) continue;
                if ((start & 0x8000U) == 0) {
                    walk(page, start);
                    continue;
                }
                auto overflow_index = static_cast<std::uint64_t>(start & 0x7fffU);
                if (overflow_index < page_count) invalid("chain overflow list overlaps page starts");
                while (true) {
                    const auto overflow = info.u16(22 + overflow_index * 2);
                    walk(page, static_cast<std::uint16_t>(overflow & 0x7fffU));
                    ++overflow_index;
                    if ((overflow & 0x8000U) != 0) break;
                }
            }
        }
    }
    return image;
}

std::uint64_t MachOImage::entry(std::uint64_t load_base) const {
    valid_load_base(load_base, image_size_);
    if (!entry_address_) invalid("image has no executable entry point");
    return add(load_base, *entry_address_ - preferred_base_);
}

void MachOImage::map(transmute::GuestMemory& memory, std::uint64_t load_base, const FileMapper& map_file) const {
    valid_load_base(load_base, image_size_);
    if (bytes_.empty()) invalid("image file bytes were released");
    for (const auto& segment : segments_) {
        if (segment.size == 0) continue;
        const auto base = pagezero(segment) ? segment.address : relocated(segment, preferred_base_, load_base);
        if (!memory.is_range_available(base, segment.size)) invalid("requested segment address is unavailable");
        if (!pagezero(segment)) {
            for (const auto& reserved : segments_) {
                if (pagezero(reserved) && base < reserved.address + reserved.size &&
                    reserved.address < add(base, segment.size)) invalid("relocated image overlaps __PAGEZERO");
            }
        }
    }
    if (map_file && file_ && reserved_tail_segment_ == SIZE_MAX) {
        try {
            if (map_from_file(memory, load_base, map_file)) return;
        } catch (const std::exception&) {
            // Copied below instead (which reports a range that is really refused).
        }
    }
    map_copied(memory, load_base);
}

bool MachOImage::map_from_file(transmute::GuestMemory& memory, std::uint64_t load_base,
                               const FileMapper& map_file) const {
    // Host pages (4 KiB): a segment maps from the file when its address,
    // size and file offset are page multiples; all segments do or none does.
    constexpr std::uint64_t page = 0x1000;
    for (const auto& segment : segments_) {
        if (segment.size == 0 || pagezero(segment)) continue;
        if (relocated(segment, preferred_base_, load_base) % page != 0 || segment.size % page != 0 ||
            segment.file_size == 0 || segment.file_size > segment.size ||
            (file_offset_ + segment.file_offset) % page != 0) return false;
    }
    void* const section = reinterpret_cast<void*>(file_->section.get().value);
    std::vector<std::uint64_t> mapped;
    const auto undo = [&] {
        for (auto it = mapped.rbegin(); it != mapped.rend(); ++it) {
            try { memory.unmap_memory(*it); } catch (...) { }
        }
    };
    try {
        for (const auto& segment : segments_) {
            if (segment.size == 0) continue;
            if (pagezero(segment)) {
                memory.reserve_memory(segment.address, segment.size);
                mapped.push_back(segment.address);
                continue;
            }
            const auto base = relocated(segment, preferred_base_, load_base);
            if (!map_file(base, segment.size, section, file_offset_ + segment.file_offset, segment.file_size)) {
                undo();
                return false;
            }
            mapped.push_back(base);
        }
    } catch (...) {
        undo();
        throw;
    }
    return true;
}

void MachOImage::map_copied(transmute::GuestMemory& memory, std::uint64_t load_base) const {
    std::vector<std::uint64_t> mapped;
    try {
        for (const auto& segment : segments_) {
            if (segment.size == 0) continue;
            const auto base = pagezero(segment) ? segment.address : relocated(segment, preferred_base_, load_base);
            if (pagezero(segment)) {
                memory.reserve_memory(base, segment.size);
                mapped.push_back(base);
                continue;
            }
            const bool reserved_tail = static_cast<std::size_t>(&segment - segments_.data()) ==
                reserved_tail_segment_;
            const auto committed = reserved_tail ? reserved_tail_mapped_ : segment.size;
            if (committed != 0) {
                memory.map_memory(base, static_cast<std::size_t>(committed));
                mapped.push_back(base);
            }
            if (reserved_tail) {
                // The omitted string table stays on disk: guest reads of it
                // fault instead of observing invented bytes.
                memory.reserve_memory(add(base, committed), segment.size - committed);
                mapped.push_back(add(base, committed));
            }
            // One bulk copy per segment (a byte-wise copy of a 175 MB image
            // costs seconds of per-call overhead).
            const auto copied = std::min(segment.file_size, committed);
            if (copied != 0) {
                const Bytes file(bytes_, omitted_offset_, omitted_size_);
                memory.write_bytes(base, file.span(segment.file_offset, copied));
            }
        }
    } catch (...) {
        for (auto it = mapped.rbegin(); it != mapped.rend(); ++it) {
            try { memory.unmap_memory(*it); } catch (...) { }
        }
        throw;
    }
}

void MachOImage::release_file() noexcept {
    bytes_ = {};
    owned_.reset();
    file_.reset();
}

void MachOImage::bind(transmute::GuestMemory& memory, std::uint64_t load_base,
                      const SymbolResolver& resolver) const {
    valid_load_base(load_base, image_size_);
    if (bound_) invalid("the image is already bound (its fixups were applied and freed)");
    // Resolve everything before writing fixups, including imports not reached by
    // any chain. A missing strong import is an error before guest code starts.
    std::vector<std::optional<std::uint64_t>> resolved;
    resolved.reserve(imports_.size());
    for (const auto& import : imports_) {
        const auto value = resolver ? resolver(import.library_ordinal, import.symbol, import.weak) : std::nullopt;
        if (!value && !import.weak) invalid("unresolved strong import " + import.symbol +
                                           " from library ordinal " + std::to_string(import.library_ordinal));
        resolved.push_back(value);
    }
    std::vector<std::pair<std::uint64_t, std::uint64_t>> writes;
    writes.reserve(fixups_.size());
    for (const auto& fixup : fixups_) {
        std::uint64_t value;
        if (fixup.import) {
            const auto index = *fixup.import;
            if (!resolved[index]) value = 0;
            else if (imports_[index].full_width_addend) {
                // ADDEND64 arithmetic acts on a pointer value, including tag
                // bits. Unsigned addition preserves its defined 64-bit pattern.
                value = *resolved[index] + static_cast<std::uint64_t>(imports_[index].addend) +
                    static_cast<std::uint64_t>(fixup.addend);
            }
            else {
                try { value = add_signed(*resolved[index], sum_signed(imports_[index].addend, fixup.addend)); }
                catch (const std::runtime_error& error) {
                    invalid("binding " + imports_[index].symbol + " in " + source_.string() +
                        " address " + std::to_string(*resolved[index]) + " import addend " +
                        std::to_string(imports_[index].addend) + " pointer addend " +
                        std::to_string(fixup.addend) + ": " + error.what());
                }
            }
        } else {
            value = add(load_base, fixup.target_offset) | (static_cast<std::uint64_t>(fixup.high8) << 56);
        }
        const auto location = add(load_base, fixup.offset);
        const auto region = memory.query_region(location);
        if (!region || !region->writable || !fits(location - region->base, 8, region->size)) {
            invalid("fixup destination is not writable mapped memory");
        }
        writes.emplace_back(location, value);
    }
    for (const auto& [location, value] : writes) memory.write64(location, value);
    bound_ = true;
    std::vector<Fixup>().swap(fixups_);
}

void MachOImage::protect(transmute::GuestMemory& memory, std::uint64_t load_base) const {
    valid_load_base(load_base, image_size_);
    for (std::size_t index = 0; index < segments_.size(); ++index) {
        const auto& segment = segments_[index];
        if (pagezero(segment) || segment.size == 0) continue;
        if (index == reserved_tail_segment_ && reserved_tail_mapped_ == 0) continue;
        const auto permissions = (segment.flags & 0x10U) != 0
            ? segment.initial_permissions & ~2U : segment.initial_permissions;
        memory.set_memory_access(relocated(segment, preferred_base_, load_base),
            (permissions & 1U) != 0, (permissions & 2U) != 0, (permissions & 4U) != 0);
    }
}

std::optional<std::uint64_t> MachOImage::exported_symbol(
    std::string_view name, std::uint64_t load_base) const {
    valid_load_base(load_base, image_size_);
    const auto found = std::find_if(exports_.begin(), exports_.end(),
        [&](const auto& symbol) { return symbol.name == name; });
    if (found == exports_.end()) return std::nullopt;
    if (found->reexport_ordinal) return std::nullopt;
    return found->absolute ? found->offset : add(load_base, found->offset);
}

std::vector<std::uint64_t> MachOImage::initializers(
    const transmute::GuestMemory& memory, std::uint64_t load_base) const {
    valid_load_base(load_base, image_size_);
    std::vector<std::uint64_t> result;
    for (const auto& segment : segments_) {
        for (const auto& section : segment.sections) {
            const bool offsets = (section.flags & 0xffU) == 0x16;
            if (!offsets && (section.flags & 0xffU) != 9 && section.name != "__mod_init_func") continue;
            const auto start = add(load_base, section.address - preferred_base_);
            const auto stride = offsets ? 4U : 8U;
            for (std::uint64_t offset = 0; offset < section.size; offset += stride) {
                const auto address = offsets ? add(load_base, memory.read32(start + offset))
                                             : memory.read64(start + offset);
                const auto executable = std::any_of(segments_.begin(), segments_.end(),
                    [&](const auto& candidate) {
                        if (pagezero(candidate) || (candidate.initial_permissions & 4U) == 0) return false;
                        const auto base = relocated(candidate, preferred_base_, load_base);
                        return address >= base && fits(address - base, 4, candidate.size);
                    });
                if (!executable || (address & 3U) != 0) invalid("initializer does not point to image executable code");
                result.push_back(address);
            }
        }
    }
    return result;
}

} // namespace alchemy
