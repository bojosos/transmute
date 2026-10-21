#pragma once

// Read-only PE parsing over raw file bytes (owner: cache). Used by AOT (the
// image is mapped as data, never executed) and by compute_image_hash. Every
// accessor is bounds-checked; malformed input yields false/nullptr.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace transmute::fx::pe {

inline constexpr std::uint16_t kMachineArm64 = 0xAA64;

struct Section {
    std::uint32_t rva;
    std::uint32_t virtual_size;
    std::uint32_t raw_offset;
    std::uint32_t raw_size;
    std::uint32_t characteristics;
    bool executable;
};

struct DataDirectory {
    std::uint32_t rva;
    std::uint32_t size;
};

class Image {
public:
    // Parses headers of a PE file held in [data, data + size). The bytes must
    // outlive the Image.
    bool parse(const std::uint8_t* data, std::size_t size, std::wstring* error);

    [[nodiscard]] const std::uint8_t* data() const { return data_; }
    [[nodiscard]] std::size_t size() const { return size_; }
    [[nodiscard]] std::uint16_t machine() const { return machine_; }
    [[nodiscard]] std::uint64_t image_base() const { return image_base_; }
    [[nodiscard]] std::uint32_t size_of_image() const { return size_of_image_; }
    [[nodiscard]] std::uint32_t entry_rva() const { return entry_rva_; }
    [[nodiscard]] std::uint32_t size_of_headers() const { return size_of_headers_; }
    [[nodiscard]] bool is_dll() const { return (characteristics_ & 0x2000) != 0; }
    [[nodiscard]] const std::vector<Section>& sections() const { return sections_; }
    [[nodiscard]] DataDirectory directory(unsigned index) const {
        return index < directories_.size() ? directories_[index] : DataDirectory{};
    }

    // Section containing `rva` (virtual extent), or nullptr.
    [[nodiscard]] const Section* section_for(std::uint32_t rva) const;
    // True if `rva` lies in an executable section.
    [[nodiscard]] bool executable(std::uint32_t rva) const;
    // File bytes for [rva, rva + size) when wholly backed by raw data of one
    // section (or the headers); nullptr otherwise.
    [[nodiscard]] const std::uint8_t* at_rva(std::uint32_t rva, std::uint32_t size) const;
    template <typename T>
    bool read(std::uint32_t rva, T& out) const {
        const std::uint8_t* p = at_rva(rva, sizeof(T));
        if (!p) return false;
        std::memcpy(&out, p, sizeof(T));
        return true;
    }

private:
    const std::uint8_t* data_{};
    std::size_t size_{};
    std::uint16_t machine_{};
    std::uint16_t characteristics_{};
    std::uint64_t image_base_{};
    std::uint32_t size_of_image_{};
    std::uint32_t size_of_headers_{};
    std::uint32_t entry_rva_{};
    std::vector<Section> sections_;
    std::vector<DataDirectory> directories_;
};

// A file mapped read-only as data (FILE_MAP_READ). Never executable.
class MappedFile {
public:
    MappedFile() = default;
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    MappedFile(MappedFile&& other) noexcept { *this = std::move(other); }
    MappedFile& operator=(MappedFile&& other) noexcept;
    ~MappedFile() { close(); }

    // A read-only view, shared with every process viewing the file.
    // `share_write` allows mapping files another process is appending to.
    bool open(const std::wstring& path, bool share_write, std::wstring* error = nullptr);
    void close();
    [[nodiscard]] const std::uint8_t* data() const { return data_; }
    [[nodiscard]] std::size_t size() const { return size_; }
    [[nodiscard]] explicit operator bool() const { return data_ != nullptr; }

private:
    const std::uint8_t* data_{};
    std::size_t size_{};
};

// ARM64 RUNTIME_FUNCTION decoded into [begin, end) RVAs.
struct Function {
    std::uint32_t begin;
    std::uint32_t end;
};
// Every .pdata entry, in table order (normally sorted by begin). Entries
// whose length cannot be decoded get end == begin.
std::vector<Function> pdata_functions(const Image& image);

// Identity hash over PE headers through the end of the section table, with
// OptionalHeader.ImageBase and CheckSum masked (loaders may rewrite them).
// Gives the same value for the raw file and for a mapped image's headers.
// `data` must be readable for min(size, 64 KiB) bytes.
std::uint64_t image_identity(const std::uint8_t* data, std::size_t size) noexcept;
// The same for PE32+ headers only: false (out untouched) for anything else.
bool header_identity(const std::uint8_t* data, std::size_t size, std::uint64_t& out) noexcept;

} // namespace transmute::fx::pe
