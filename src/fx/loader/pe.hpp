#pragma once

// PE32+ parsing helpers for the fx loader (owner: loader). Header parsing works
// on file bytes; export, relocation and resource helpers work on images that
// are already mapped at their run-time address (guest images and host DLLs
// share the PE32+ layout).

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace transmute::fx::loader {

inline constexpr std::uint16_t kMachineArm64 = 0xAA64;
inline constexpr std::uint16_t kMachineAmd64 = 0x8664;

enum DirectoryIndex : unsigned {
    dir_export = 0,
    dir_import = 1,
    dir_resource = 2,
    dir_exception = 3,
    dir_security = 4,
    dir_basereloc = 5,
    dir_debug = 6,
    dir_tls = 9,
    dir_load_config = 10,
    dir_bound_import = 11,
    dir_iat = 12,
    dir_delay_import = 13,
};

struct PeSection {
    char name[9]{};
    std::uint32_t rva{};
    std::uint32_t virtual_size{};
    std::uint32_t raw_offset{};
    std::uint32_t raw_size{};
    std::uint32_t characteristics{};
};

struct PeDirectory {
    std::uint32_t rva{};
    std::uint32_t size{};
};

struct PeHeaders {
    std::uint16_t machine{};
    std::uint16_t file_characteristics{};
    std::uint16_t dll_characteristics{};
    std::uint16_t subsystem{};
    std::uint32_t timestamp{};
    std::uint32_t checksum{};
    std::uint64_t image_base{};
    std::uint32_t size_of_image{};
    std::uint32_t size_of_headers{};
    std::uint32_t entry_rva{};
    std::uint32_t section_alignment{};
    std::uint32_t file_alignment{};
    std::uint64_t stack_reserve{};
    std::uint64_t stack_commit{};
    std::uint32_t nt_offset{};
    std::uint32_t directory_count{};
    PeDirectory dirs[16]{};
    std::vector<PeSection> sections;

    [[nodiscard]] bool is_dll() const noexcept { return (file_characteristics & 0x2000u) != 0; }
    [[nodiscard]] bool relocs_stripped() const noexcept { return (file_characteristics & 0x0001u) != 0; }
    [[nodiscard]] PeDirectory dir(unsigned index) const noexcept {
        return index < directory_count && index < 16 ? dirs[index] : PeDirectory{};
    }
};

// Parses DOS, NT and optional headers and the section table. `data` holds at
// least the start of the file (or a mapped image). Only PE32+ is accepted.
// `available` is the number of bytes the caller could read (the section
// table must fit); returns false with `error` set on malformed headers.
bool parse_pe_headers(std::span<const std::uint8_t> data, PeHeaders& out, std::string& error);

// Machine field of the PE file in `data` (first bytes of a file), 0 if not PE.
std::uint16_t pe_machine(std::span<const std::uint8_t> data, bool* is_dll = nullptr);

// Validates image layout (sizes, section bounds) for mapping.
bool validate_image_layout(const PeHeaders& headers, std::uint64_t file_size, std::string& error);

struct ExportLookup {
    enum class Kind { not_found, address, forwarder };
    Kind kind = Kind::not_found;
    std::uint64_t address = 0;      // absolute address (Kind::address)
    std::string_view forwarder;     // "DLL.Function" / "DLL.#n" (Kind::forwarder), points into the image
    std::uint32_t ordinal = 0;      // biased ordinal of the export
};

// Export directory of an image mapped at `base` (guest image or host HMODULE).
class ExportView {
public:
    // Reads the headers at `base`. False if the image has no export directory.
    bool open(const std::uint8_t* base);
    [[nodiscard]] bool valid() const noexcept { return directory_ != nullptr; }
    // Name lookup. `hint` (from IMAGE_IMPORT_BY_NAME) is tried first.
    [[nodiscard]] ExportLookup by_name(std::string_view name, std::uint32_t hint = 0xffffffffu) const;
    // Biased ordinal lookup (as written in import thunks / GetProcAddress).
    [[nodiscard]] ExportLookup by_ordinal(std::uint32_t ordinal) const;
    // Name of the export with the given biased ordinal, empty if unnamed.
    [[nodiscard]] std::string_view name_of_ordinal(std::uint32_t ordinal) const;
    [[nodiscard]] std::uint32_t function_count() const noexcept { return function_count_; }
    [[nodiscard]] std::uint32_t name_count() const noexcept { return name_count_; }
    [[nodiscard]] std::uint32_t ordinal_base() const noexcept { return ordinal_base_; }
    [[nodiscard]] std::string_view name_at(std::uint32_t index) const;
    [[nodiscard]] ExportLookup by_index(std::uint32_t function_index) const;

private:
    [[nodiscard]] std::string_view string_at(std::uint32_t rva) const;
    const std::uint8_t* base_ = nullptr;
    std::uint32_t image_size_ = 0;
    const void* directory_ = nullptr;
    std::uint32_t directory_rva_ = 0;
    std::uint32_t directory_size_ = 0;
    std::uint32_t ordinal_base_ = 0;
    std::uint32_t function_count_ = 0;
    std::uint32_t name_count_ = 0;
    const std::uint32_t* functions_ = nullptr;
    const std::uint32_t* names_ = nullptr;
    const std::uint16_t* name_ordinals_ = nullptr;
};

// Applies base relocations to an image mapped at `base`. Supports ABSOLUTE,
// HIGH, LOW, HIGHLOW and DIR64 entries. Returns the number of fixups applied
// or -1 (with `error`) on malformed or unsupported relocations.
std::int64_t apply_base_relocations(std::uint8_t* base, std::uint32_t image_size,
                                    PeDirectory relocations, std::int64_t delta,
                                    std::string& error);

// Data of resource (type, id) with the first language, from an image mapped
// at `base`. Empty if absent.
std::span<const std::uint8_t> find_resource(const std::uint8_t* base, std::uint32_t image_size,
                                            PeDirectory resources, std::uint16_t type,
                                            std::uint16_t id);

} // namespace transmute::fx::loader
