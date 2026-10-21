#include "fx/loader/pe.hpp"

#include <algorithm>
#include <cstring>

namespace transmute::fx::loader {
namespace {

bool in_range(std::uint64_t offset, std::uint64_t length, std::uint64_t total) {
    return offset <= total && length <= total - offset;
}

template <typename T>
T load(const std::uint8_t* p) {
    T value;
    std::memcpy(&value, p, sizeof(T));
    return value;
}

template <typename T>
bool read(std::span<const std::uint8_t> data, std::uint64_t offset, T& value) {
    if (!in_range(offset, sizeof(T), data.size())) return false;
    std::memcpy(&value, data.data() + offset, sizeof(T));
    return true;
}

} // namespace

std::uint16_t pe_machine(std::span<const std::uint8_t> data, bool* is_dll) {
    std::uint16_t mz = 0;
    std::uint32_t nt = 0, signature = 0;
    if (!read(data, 0, mz) || mz != 0x5a4d || !read(data, 0x3c, nt)) return 0;
    if (!read(data, nt, signature) || signature != 0x00004550) return 0;
    std::uint16_t machine = 0, characteristics = 0;
    if (!read(data, nt + 4ull, machine) || !read(data, nt + 22ull, characteristics)) return 0;
    if (is_dll) *is_dll = (characteristics & 0x2000u) != 0;
    return machine;
}

bool parse_pe_headers(std::span<const std::uint8_t> data, PeHeaders& out, std::string& error) {
    out = PeHeaders{};
    std::uint16_t mz = 0;
    if (!read(data, 0, mz) || mz != 0x5a4d) {
        error = "not a PE file (missing MZ signature)";
        return false;
    }
    std::uint32_t nt = 0, signature = 0;
    if (!read(data, 0x3c, nt) || !read(data, nt, signature) || signature != 0x00004550) {
        error = "invalid PE signature";
        return false;
    }
    out.nt_offset = nt;
    std::uint16_t section_count = 0, optional_size = 0;
    if (!read(data, nt + 4ull, out.machine) || !read(data, nt + 6ull, section_count) ||
        !read(data, nt + 8ull, out.timestamp) || !read(data, nt + 20ull, optional_size) ||
        !read(data, nt + 22ull, out.file_characteristics)) {
        error = "truncated COFF header";
        return false;
    }
    const std::uint64_t optional = nt + 24ull;
    std::uint16_t magic = 0;
    if (!read(data, optional, magic) || magic != 0x20b || optional_size < 112 ||
        !in_range(optional, optional_size, data.size())) {
        error = "not a PE32+ image";
        return false;
    }
    read(data, optional + 16, out.entry_rva);
    read(data, optional + 24, out.image_base);
    read(data, optional + 32, out.section_alignment);
    read(data, optional + 36, out.file_alignment);
    read(data, optional + 56, out.size_of_image);
    read(data, optional + 60, out.size_of_headers);
    read(data, optional + 64, out.checksum);
    read(data, optional + 68, out.subsystem);
    read(data, optional + 70, out.dll_characteristics);
    read(data, optional + 72, out.stack_reserve);
    read(data, optional + 80, out.stack_commit);
    read(data, optional + 108, out.directory_count);
    if (out.directory_count > 16) out.directory_count = 16;
    if (112ull + out.directory_count * 8ull > optional_size) {
        out.directory_count = static_cast<std::uint32_t>((optional_size - 112u) / 8u);
    }
    for (std::uint32_t i = 0; i < out.directory_count; ++i) {
        read(data, optional + 112 + i * 8ull, out.dirs[i].rva);
        read(data, optional + 116 + i * 8ull, out.dirs[i].size);
    }
    const std::uint64_t table = optional + optional_size;
    if (section_count > 96 || !in_range(table, section_count * 40ull, data.size())) {
        error = "truncated or oversized section table";
        return false;
    }
    out.sections.resize(section_count);
    for (std::uint32_t i = 0; i < section_count; ++i) {
        const auto* at = data.data() + table + i * 40ull;
        auto& section = out.sections[i];
        std::memcpy(section.name, at, 8);
        section.name[8] = 0;
        section.virtual_size = load<std::uint32_t>(at + 8);
        section.rva = load<std::uint32_t>(at + 12);
        section.raw_size = load<std::uint32_t>(at + 16);
        section.raw_offset = load<std::uint32_t>(at + 20);
        section.characteristics = load<std::uint32_t>(at + 36);
    }
    return true;
}

bool validate_image_layout(const PeHeaders& h, std::uint64_t file_size, std::string& error) {
    constexpr std::uint64_t kMaxImage = 3ull << 30;
    if (h.size_of_image == 0 || h.size_of_image > kMaxImage) {
        error = "invalid SizeOfImage";
        return false;
    }
    if (h.size_of_headers == 0 || h.size_of_headers > h.size_of_image ||
        h.size_of_headers > file_size) {
        error = "invalid SizeOfHeaders";
        return false;
    }
    if ((h.image_base & 0xffffu) != 0 || h.image_base == 0 ||
        h.image_base > 0x00007fffffffffffull - h.size_of_image) {
        error = "invalid ImageBase";
        return false;
    }
    if (h.section_alignment == 0 || h.file_alignment == 0 ||
        (h.section_alignment & (h.section_alignment - 1)) != 0) {
        error = "invalid section alignment";
        return false;
    }
    for (const auto& s : h.sections) {
        const std::uint32_t span_size =
            s.virtual_size ? s.virtual_size : s.raw_size;
        if (!in_range(s.rva, span_size, h.size_of_image)) {
            error = std::string("section ") + s.name + " extends beyond SizeOfImage";
            return false;
        }
        if (s.raw_size != 0 && s.raw_offset > file_size) {
            error = std::string("section ") + s.name + " raw data is outside the file";
            return false;
        }
    }
    if (h.entry_rva >= h.size_of_image) {
        error = "entry point outside the image";
        return false;
    }
    for (unsigned i = 0; i < h.directory_count; ++i) {
        if (i == dir_security || h.dirs[i].size == 0) continue;
        if (!in_range(h.dirs[i].rva, h.dirs[i].size, h.size_of_image)) {
            error = "data directory " + std::to_string(i) + " extends beyond the image";
            return false;
        }
    }
    return true;
}

// ---- exports ----------------------------------------------------------------

namespace {
struct ExportDirectoryRaw {
    std::uint32_t characteristics;
    std::uint32_t timestamp;
    std::uint16_t major, minor;
    std::uint32_t name;
    std::uint32_t base;
    std::uint32_t number_of_functions;
    std::uint32_t number_of_names;
    std::uint32_t address_of_functions;
    std::uint32_t address_of_names;
    std::uint32_t address_of_name_ordinals;
};
} // namespace

bool ExportView::open(const std::uint8_t* base) {
    *this = ExportView{};
    if (!base) return false;
    std::uint32_t nt = load<std::uint32_t>(base + 0x3c);
    const std::uint8_t* optional = base + nt + 24;
    if (load<std::uint16_t>(optional) != 0x20b) return false;
    const std::uint32_t image_size = load<std::uint32_t>(optional + 56);
    const std::uint32_t count = load<std::uint32_t>(optional + 108);
    if (count == 0) return false;
    const std::uint32_t rva = load<std::uint32_t>(optional + 112);
    const std::uint32_t size = load<std::uint32_t>(optional + 116);
    if (rva == 0 || size < sizeof(ExportDirectoryRaw) || !in_range(rva, size, image_size)) return false;
    ExportDirectoryRaw dir;
    std::memcpy(&dir, base + rva, sizeof(dir));
    if (!in_range(dir.address_of_functions, dir.number_of_functions * 4ull, image_size) ||
        !in_range(dir.address_of_names, dir.number_of_names * 4ull, image_size) ||
        !in_range(dir.address_of_name_ordinals, dir.number_of_names * 2ull, image_size)) {
        return false;
    }
    base_ = base;
    image_size_ = image_size;
    directory_ = base + rva;
    directory_rva_ = rva;
    directory_size_ = size;
    ordinal_base_ = dir.base;
    function_count_ = dir.number_of_functions;
    name_count_ = dir.number_of_names;
    functions_ = reinterpret_cast<const std::uint32_t*>(base + dir.address_of_functions);
    names_ = reinterpret_cast<const std::uint32_t*>(base + dir.address_of_names);
    name_ordinals_ = reinterpret_cast<const std::uint16_t*>(base + dir.address_of_name_ordinals);
    return true;
}

std::string_view ExportView::string_at(std::uint32_t rva) const {
    if (rva == 0 || rva >= image_size_) return {};
    const char* s = reinterpret_cast<const char*>(base_ + rva);
    const std::size_t max = image_size_ - rva;
    const void* end = std::memchr(s, 0, max);
    if (!end) return {};
    return std::string_view(s, static_cast<const char*>(end) - s);
}

std::string_view ExportView::name_at(std::uint32_t index) const {
    if (!valid() || index >= name_count_) return {};
    return string_at(names_[index]);
}

ExportLookup ExportView::by_index(std::uint32_t index) const {
    ExportLookup result;
    if (!valid() || index >= function_count_) return result;
    const std::uint32_t rva = functions_[index];
    if (rva == 0) return result;
    result.ordinal = index + ordinal_base_;
    if (rva >= directory_rva_ && rva < directory_rva_ + directory_size_) {
        result.kind = ExportLookup::Kind::forwarder;
        result.forwarder = string_at(rva);
        if (result.forwarder.empty()) result.kind = ExportLookup::Kind::not_found;
        return result;
    }
    if (rva >= image_size_) return result;
    result.kind = ExportLookup::Kind::address;
    result.address = reinterpret_cast<std::uint64_t>(base_) + rva;
    return result;
}

ExportLookup ExportView::by_name(std::string_view name, std::uint32_t hint) const {
    if (!valid() || name.empty()) return {};
    if (hint < name_count_ && name_at(hint) == name) return by_index(name_ordinals_[hint]);
    std::uint32_t lo = 0, hi = name_count_;
    while (lo < hi) {
        const std::uint32_t mid = lo + (hi - lo) / 2;
        const auto candidate = name_at(mid);
        const int c = candidate.compare(name);
        if (c == 0) return by_index(name_ordinals_[mid]);
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    // Some linkers emit unsorted name tables; fall back to a scan.
    for (std::uint32_t i = 0; i < name_count_; ++i) {
        if (name_at(i) == name) return by_index(name_ordinals_[i]);
    }
    return {};
}

ExportLookup ExportView::by_ordinal(std::uint32_t ordinal) const {
    if (!valid() || ordinal < ordinal_base_) return {};
    return by_index(ordinal - ordinal_base_);
}

std::string_view ExportView::name_of_ordinal(std::uint32_t ordinal) const {
    if (!valid() || ordinal < ordinal_base_) return {};
    const std::uint32_t index = ordinal - ordinal_base_;
    for (std::uint32_t i = 0; i < name_count_; ++i) {
        if (name_ordinals_[i] == index) return name_at(i);
    }
    return {};
}

// ---- relocations ------------------------------------------------------------

std::int64_t apply_base_relocations(std::uint8_t* base, std::uint32_t image_size,
                                    PeDirectory relocations, std::int64_t delta,
                                    std::string& error) {
    if (relocations.rva == 0 || relocations.size == 0) return 0;
    if (!in_range(relocations.rva, relocations.size, image_size)) {
        error = "relocation directory outside the image";
        return -1;
    }
    std::int64_t applied = 0;
    const std::uint8_t* block = base + relocations.rva;
    const std::uint8_t* end = block + relocations.size;
    while (end - block >= 8) {
        const std::uint32_t page = load<std::uint32_t>(block);
        const std::uint32_t size = load<std::uint32_t>(block + 4);
        if (size < 8 || size > static_cast<std::uint64_t>(end - block)) {
            if (size == 0 && page == 0) break;   // zero padding
            error = "malformed relocation block";
            return -1;
        }
        const std::uint32_t count = (size - 8) / 2;
        const std::uint8_t* entries = block + 8;
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint16_t entry = load<std::uint16_t>(entries + i * 2ull);
            const unsigned type = entry >> 12;
            const std::uint64_t target = static_cast<std::uint64_t>(page) + (entry & 0xfffu);
            switch (type) {
            case 0:   // IMAGE_REL_BASED_ABSOLUTE
                break;
            case 10: {   // IMAGE_REL_BASED_DIR64
                if (!in_range(target, 8, image_size)) goto out_of_range;
                std::uint64_t value = load<std::uint64_t>(base + target);
                value += static_cast<std::uint64_t>(delta);
                std::memcpy(base + target, &value, 8);
                ++applied;
                break;
            }
            case 3: {   // IMAGE_REL_BASED_HIGHLOW
                if (!in_range(target, 4, image_size)) goto out_of_range;
                std::uint32_t value = load<std::uint32_t>(base + target);
                value += static_cast<std::uint32_t>(delta);
                std::memcpy(base + target, &value, 4);
                ++applied;
                break;
            }
            case 2: {   // IMAGE_REL_BASED_LOW
                if (!in_range(target, 2, image_size)) goto out_of_range;
                std::uint16_t value = load<std::uint16_t>(base + target);
                value = static_cast<std::uint16_t>(value + static_cast<std::uint16_t>(delta));
                std::memcpy(base + target, &value, 2);
                ++applied;
                break;
            }
            case 1: {   // IMAGE_REL_BASED_HIGH
                if (!in_range(target, 2, image_size)) goto out_of_range;
                std::uint16_t value = load<std::uint16_t>(base + target);
                value = static_cast<std::uint16_t>(value + static_cast<std::uint16_t>(delta >> 16));
                std::memcpy(base + target, &value, 2);
                ++applied;
                break;
            }
            default:
                error = "unsupported relocation type " + std::to_string(type);
                return -1;
            }
        }
        block += size;
    }
    return applied;
out_of_range:
    error = "relocation target outside the image";
    return -1;
}

// ---- resources --------------------------------------------------------------

namespace {
struct ResourceDirectoryRaw {
    std::uint32_t characteristics;
    std::uint32_t timestamp;
    std::uint16_t major, minor;
    std::uint16_t named_entries;
    std::uint16_t id_entries;
};

// Returns the offset (relative to the resource root) of the entry whose
// integer id matches (or the first entry when id == 0xffff), 0 on failure.
std::uint32_t find_resource_entry(const std::uint8_t* root, std::uint32_t size,
                                  std::uint32_t directory, std::uint16_t id, bool& is_directory) {
    if (!in_range(directory, sizeof(ResourceDirectoryRaw), size)) return 0;
    ResourceDirectoryRaw dir;
    std::memcpy(&dir, root + directory, sizeof(dir));
    const std::uint32_t total = static_cast<std::uint32_t>(dir.named_entries) + dir.id_entries;
    const std::uint32_t entries = directory + sizeof(ResourceDirectoryRaw);
    if (!in_range(entries, total * 8ull, size)) return 0;
    for (std::uint32_t i = 0; i < total; ++i) {
        const std::uint32_t name = load<std::uint32_t>(root + entries + i * 8ull);
        const std::uint32_t offset = load<std::uint32_t>(root + entries + i * 8ull + 4);
        const bool named = (name & 0x80000000u) != 0;
        if (id != 0xffff && (named || (name & 0xffffu) != id)) continue;
        is_directory = (offset & 0x80000000u) != 0;
        return offset & 0x7fffffffu;
    }
    return 0;
}
} // namespace

std::span<const std::uint8_t> find_resource(const std::uint8_t* base, std::uint32_t image_size,
                                            PeDirectory resources, std::uint16_t type,
                                            std::uint16_t id) {
    if (resources.rva == 0 || !in_range(resources.rva, resources.size, image_size)) return {};
    const std::uint8_t* root = base + resources.rva;
    const std::uint32_t size = resources.size;
    bool is_dir = false;
    std::uint32_t at = find_resource_entry(root, size, 0, type, is_dir);
    if (at == 0 || !is_dir) return {};
    at = find_resource_entry(root, size, at, id, is_dir);
    if (at == 0 || !is_dir) return {};
    at = find_resource_entry(root, size, at, 0xffff, is_dir);
    if (at == 0 || is_dir || !in_range(at, 16, size)) return {};
    const std::uint32_t data_rva = load<std::uint32_t>(root + at);
    const std::uint32_t data_size = load<std::uint32_t>(root + at + 4);
    if (!in_range(data_rva, data_size, image_size)) return {};
    return {base + data_rva, data_size};
}

} // namespace transmute::fx::loader
