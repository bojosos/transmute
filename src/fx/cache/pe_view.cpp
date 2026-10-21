#include "fx/cache/pe_view.hpp"

#include "fx/cache/format.hpp"
#include "fx/host/file.hpp"

#include <algorithm>

namespace transmute::fx::pe {
namespace {

template <typename T>
bool load(const std::uint8_t* data, std::size_t size, std::size_t offset, T& out) {
    if (offset > size || sizeof(T) > size - offset) return false;
    std::memcpy(&out, data + offset, sizeof(T));
    return true;
}

void set_error(std::wstring* error, const wchar_t* message) {
    if (error) *error = message;
}

struct HeaderLayout {
    std::uint32_t nt;          // e_lfanew
    std::uint32_t optional;    // offset of the optional header
    std::uint16_t optional_size;
    std::uint16_t sections;
    std::uint16_t magic;
    std::uint32_t section_table;
    std::uint32_t section_table_end;
};

bool header_layout(const std::uint8_t* data, std::size_t size, HeaderLayout& out) {
    std::uint16_t mz = 0;
    if (!load(data, size, 0, mz) || mz != 0x5A4D) return false;
    if (!load(data, size, 0x3C, out.nt) || out.nt > (1u << 20)) return false;
    std::uint32_t signature = 0;
    if (!load(data, size, out.nt, signature) || signature != 0x00004550) return false;
    if (!load(data, size, out.nt + 6, out.sections) || !load(data, size, out.nt + 20, out.optional_size)) return false;
    out.optional = out.nt + 24;
    if (!load(data, size, out.optional, out.magic)) return false;
    out.section_table = out.optional + out.optional_size;
    out.section_table_end = out.section_table + std::uint32_t{out.sections} * 40u;
    return out.section_table_end <= size;
}

} // namespace

bool Image::parse(const std::uint8_t* data, std::size_t size, std::wstring* error) {
    *this = Image{};
    HeaderLayout layout{};
    if (!header_layout(data, size, layout)) {
        set_error(error, L"not a PE image (bad DOS/NT headers or section table)");
        return false;
    }
    if (layout.magic != 0x20B || layout.optional_size < 112) {
        set_error(error, L"not a PE32+ image");
        return false;
    }
    data_ = data;
    size_ = size;
    load(data, size, layout.nt + 4, machine_);
    load(data, size, layout.nt + 22, characteristics_);
    const std::size_t opt = layout.optional;
    load(data, size, opt + 16, entry_rva_);
    load(data, size, opt + 24, image_base_);
    load(data, size, opt + 56, size_of_image_);
    load(data, size, opt + 60, size_of_headers_);
    std::uint32_t directory_count = 0;
    load(data, size, opt + 108, directory_count);
    directory_count = std::min<std::uint32_t>(directory_count, 16);
    directory_count = std::min<std::uint32_t>(directory_count, (layout.optional_size - 112u) / 8u);
    directories_.resize(16);
    for (std::uint32_t i = 0; i < directory_count; ++i) {
        load(data, size, opt + 112 + i * 8, directories_[i].rva);
        load(data, size, opt + 116 + i * 8, directories_[i].size);
    }
    sections_.reserve(layout.sections);
    for (std::uint32_t i = 0; i < layout.sections; ++i) {
        const std::size_t at = layout.section_table + std::size_t{i} * 40;
        Section s{};
        load(data, size, at + 8, s.virtual_size);
        load(data, size, at + 12, s.rva);
        load(data, size, at + 16, s.raw_size);
        load(data, size, at + 20, s.raw_offset);
        load(data, size, at + 36, s.characteristics);
        if (s.raw_offset >= size) s.raw_size = 0;
        else s.raw_size = static_cast<std::uint32_t>(std::min<std::uint64_t>(s.raw_size, size - s.raw_offset));
        // Raw data beyond the virtual size is not part of the image.
        if (s.virtual_size != 0 && s.raw_size > s.virtual_size) s.raw_size = s.virtual_size;
        s.executable = (s.characteristics & (0x20000000u | 0x20u)) != 0;  // MEM_EXECUTE | CNT_CODE
        sections_.push_back(s);
    }
    if (size_of_headers_ > size) size_of_headers_ = static_cast<std::uint32_t>(size);
    return true;
}

const Section* Image::section_for(std::uint32_t rva) const {
    for (const Section& s : sections_) {
        const std::uint32_t extent = std::max(s.virtual_size, s.raw_size);
        if (rva >= s.rva && rva - s.rva < extent) return &s;
    }
    return nullptr;
}

bool Image::executable(std::uint32_t rva) const {
    const Section* s = section_for(rva);
    return s && s->executable;
}

const std::uint8_t* Image::at_rva(std::uint32_t rva, std::uint32_t size) const {
    if (!data_) return nullptr;
    if (std::uint64_t{rva} + size <= size_of_headers_) return data_ + rva;
    for (const Section& s : sections_) {
        if (rva < s.rva) continue;
        const std::uint64_t delta = rva - s.rva;
        if (delta + size <= s.raw_size) return data_ + s.raw_offset + delta;
    }
    return nullptr;
}

std::vector<Function> pdata_functions(const Image& image) {
    std::vector<Function> out;
    const DataDirectory dir = image.directory(3);  // IMAGE_DIRECTORY_ENTRY_EXCEPTION
    if (dir.rva == 0 || dir.size < 8) return out;
    const std::uint32_t count = dir.size / 8;
    const std::uint8_t* table = image.at_rva(dir.rva, count * 8);
    if (!table) return out;
    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t begin = 0, unwind = 0;
        std::memcpy(&begin, table + i * 8, 4);
        std::memcpy(&unwind, table + i * 8 + 4, 4);
        std::uint32_t length = 0;
        if ((unwind & 3) != 0) {
            length = ((unwind >> 2) & 0x7FF) * 4;  // packed unwind data
        } else {
            std::uint32_t xdata = 0;
            if (image.read(unwind, xdata)) length = (xdata & 0x3FFFF) * 4;
        }
        const std::uint64_t end = std::uint64_t{begin} + length;
        out.push_back({begin, end > 0xFFFF'FFFFull ? begin : static_cast<std::uint32_t>(end)});
    }
    return out;
}

bool header_identity(const std::uint8_t* data, std::size_t size, std::uint64_t& out) noexcept {
    constexpr std::size_t kLimit = 64 * 1024;
    if (size > kLimit) size = kLimit;
    HeaderLayout layout{};
    if (!header_layout(data, size, layout) || layout.magic != 0x20B || layout.optional_size < 68) return false;
    std::uint8_t buffer[kLimit];
    std::memcpy(buffer, data, layout.section_table_end);
    std::memset(buffer + layout.optional + 24, 0, 8);  // ImageBase
    std::memset(buffer + layout.optional + 64, 0, 4);  // CheckSum
    const std::uint64_t h = cachefmt::hash64(buffer, layout.section_table_end, 0x1e);
    out = h ? h : 1;
    return true;
}

std::uint64_t image_identity(const std::uint8_t* data, std::size_t size) noexcept {
    std::uint64_t h = 0;
    if (header_identity(data, size, h)) return h;
    h = cachefmt::hash64(data, std::min<std::size_t>(size, 4096), 0x1d);
    return h ? h : 1;
}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this != &other) {
        close();
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

bool MappedFile::open(const std::wstring& path, bool share_write, std::wstring* error) {
    namespace file = host::file;
    close();
    const unsigned share = file::kRead | file::kDelete | (share_write ? file::kWrite : 0);
    file::File handle = file::open(path, file::kRead, share, file::Create::open_existing, file::Flags::normal);
    if (!handle) {
        if (error) *error = L"cannot open " + path;
        return false;
    }
    std::uint64_t size = 0;
    if (!file::size(handle, size) || size == 0 || size > (std::uint64_t{1} << 40)) {
        file::close(handle);
        if (error) *error = L"empty or unreadable file " + path;
        return false;
    }
    int stage = 0;
    const std::uint8_t* view = file::map_read_only(handle, &stage);
    file::close(handle);
    if (!view) {
        if (error) *error = (stage == 1 ? L"cannot map " : L"cannot map view of ") + path;
        return false;
    }
    data_ = view;
    size_ = static_cast<std::size_t>(size);
    return true;
}

void MappedFile::close() {
    if (data_) host::file::unmap(data_);
    data_ = nullptr;
    size_ = 0;
}

} // namespace transmute::fx::pe
