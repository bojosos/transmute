#pragma once

#include "transmute/a64.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy {

struct MachOSection {
    std::string name;
    std::string segment;
    std::uint64_t address{};
    std::uint64_t size{};
    std::uint32_t flags{};
    std::uint32_t alignment{};
};

struct MachOSegment {
    std::string name;
    std::uint64_t address{};
    std::uint64_t size{};
    std::uint64_t file_offset{};
    std::uint64_t file_size{};
    std::uint32_t initial_permissions{};
    std::uint32_t maximum_permissions{};
    std::uint32_t flags{};
    std::vector<MachOSection> sections;
};

struct MachODependency {
    std::string install_name;
    bool weak{};
    bool reexport{};
};

struct MachOExport {
    std::string name;
    std::uint64_t offset{};
    bool weak_definition{};
    bool absolute{};
    bool thread_local_descriptor{};
    std::optional<std::uint32_t> reexport_ordinal;
    std::string imported_name;
};

struct MachOImport {
    std::int32_t library_ordinal{};
    std::string symbol;
    bool weak{};
    std::int64_t addend{};
    // ADDEND64 is an unsigned bit pattern and can encode pointer tags.
    bool full_width_addend{};
};

// A null result means unresolved. Strong imports fail, weak imports bind null.
using SymbolResolver = std::function<std::optional<std::uint64_t>(
    std::int32_t library_ordinal, std::string_view symbol, bool weak)>;

// LC_UUID of a Mach-O file (thin, or the ARM64 slice of a universal file) as
// 32 lowercase hex digits; reads only the headers. nullopt without one.
[[nodiscard]] std::optional<std::string> macho_uuid(const std::filesystem::path& path);

class MachOImage {
public:
    // Maps `size` bytes at `base` copy-on-write from the image file: the first
    // `file_bytes` from `offset` of the host file section `section` (a native
    // handle value), the rest zero. An engine's map_file_copy: the file's pages
    // stay shared with every process mapping the image until written. False
    // when the engine has no file mappings; a refused range throws.
    using FileMapper = std::function<bool(std::uint64_t base, std::uint64_t size, void* section,
                                          std::uint64_t offset, std::uint64_t file_bytes)>;
    // load() views the file read-only when the host can (its pages stay the
    // file's, shared and not copied) and reads it otherwise, then only what
    // parsing and mapping need. An ARM64 slice whose LC_SYMTAB string table is
    // very large (an installer payload, for example) is read without that
    // table; the table's pages are reserved, not mapped.
    [[nodiscard]] static MachOImage load(const std::filesystem::path& path);
    [[nodiscard]] static MachOImage parse(std::span<const std::uint8_t> bytes,
                                         std::filesystem::path source = {});
    // Bytes of the selected slice omitted from memory by load() (0 if none).
    [[nodiscard]] std::uint64_t omitted_bytes() const noexcept { return omitted_size_; }
    [[nodiscard]] const std::filesystem::path& source() const noexcept { return source_; }
    [[nodiscard]] bool is_dylib() const noexcept { return dylib_; }
    [[nodiscard]] bool is_bundle() const noexcept { return bundle_; }
    [[nodiscard]] bool flat_namespace() const noexcept { return flat_namespace_; }
    // MH_PIE: an executable that may load at any base (every arm64 one).
    [[nodiscard]] bool position_independent() const noexcept { return pie_; }
    // Whether map() can map segments from the file (load() viewed it).
    [[nodiscard]] bool file_backed() const noexcept { return file_ != nullptr; }
    [[nodiscard]] std::uint64_t preferred_base() const noexcept { return preferred_base_; }
    [[nodiscard]] std::uint64_t image_size() const noexcept { return image_size_; }
    [[nodiscard]] std::uint64_t entry(std::uint64_t load_base) const;
    [[nodiscard]] const std::vector<MachOSegment>& segments() const noexcept { return segments_; }
    // Entries are unique install identities in bind-ordinal order. A separate
    // reexport command augments the existing identity without adding an ordinal.
    [[nodiscard]] const std::vector<MachODependency>& dependencies() const noexcept { return dependencies_; }
    [[nodiscard]] const std::vector<MachOImport>& imports() const noexcept { return imports_; }
    [[nodiscard]] const std::vector<MachOExport>& exports() const noexcept { return exports_; }
    [[nodiscard]] const std::vector<std::string>& runpaths() const noexcept { return runpaths_; }
    [[nodiscard]] bool has_classic_fixups() const noexcept { return classic_fixups_; }
    [[nodiscard]] const std::string& install_name() const noexcept { return install_name_; }
    [[nodiscard]] std::uint32_t minimum_os() const noexcept { return minimum_os_; }
    [[nodiscard]] std::uint32_t sdk_version() const noexcept { return sdk_version_; }
    [[nodiscard]] std::uint64_t stack_size() const noexcept { return stack_size_; }
    [[nodiscard]] const std::vector<std::uint16_t>& pointer_formats() const noexcept { return pointer_formats_; }
    // With `map_file` and a file-backed image whose segments are page aligned
    // in the file, every segment is mapped from the file (copy-on-write);
    // otherwise, or when the engine refuses, segments are copied from bytes.
    void map(transmute::GuestMemory& memory, std::uint64_t load_base, const FileMapper& map_file = {}) const;
    // Drops the file bytes (load()'s view or copy) once the image is mapped;
    // everything parsed stays. A later map() fails.
    void release_file() noexcept;
    // Applies the fixups and then frees them (Chrome Framework's 1.5 million
    // take about 60 MB, which every process kept): an image binds once, and a
    // second bind() fails.
    void bind(transmute::GuestMemory& memory, std::uint64_t load_base,
              const SymbolResolver& resolver) const;
    [[nodiscard]] std::size_t fixup_count() const noexcept { return fixups_.size(); }
    void protect(transmute::GuestMemory& memory, std::uint64_t load_base) const;
    [[nodiscard]] std::optional<std::uint64_t> exported_symbol(
        std::string_view name, std::uint64_t load_base) const;
    [[nodiscard]] std::vector<std::uint64_t> initializers(
        const transmute::GuestMemory& memory, std::uint64_t load_base) const;

private:
    struct Fixup {
        std::uint64_t offset{};
        std::optional<std::uint32_t> import;
        std::int64_t addend{};
        std::uint64_t target_offset{};
        std::uint8_t high8{};
    };
    // `physical` holds the file without [hole_offset, hole_offset + hole_size).
    // `owned`, when it holds exactly `physical`, is moved into the image
    // instead of copied (load() of a thin file). With `file_view`, `physical` lies
    // in that file view and is referenced, not copied.
    struct MappedFile;
    [[nodiscard]] static MachOImage parse_omitting(std::span<const std::uint8_t> physical,
        std::uint64_t hole_offset, std::uint64_t hole_size, std::filesystem::path source,
        std::vector<std::uint8_t>* owned, const std::shared_ptr<const MappedFile>& file_view = {});
    void map_copied(transmute::GuestMemory& memory, std::uint64_t load_base) const;
    // False, with nothing mapped, when a segment is not page aligned in the
    // file or the engine has no file mappings.
    [[nodiscard]] bool map_from_file(transmute::GuestMemory& memory, std::uint64_t load_base,
                                     const FileMapper& map_file) const;
    std::filesystem::path source_;
    // The image's bytes (the selected slice): in file_'s view, or owned_.
    std::span<const std::uint8_t> bytes_;
    std::shared_ptr<const std::vector<std::uint8_t>> owned_;
    std::shared_ptr<const MappedFile> file_;
    std::uint64_t file_offset_{};   // offset of bytes_ in the file
    bool flat_namespace_{};
    bool pie_{};
    // Slice-relative omitted range of bytes_ (an LC_SYMTAB string table) and
    // the __LINKEDIT segment whose tail is reserved instead of mapped.
    std::uint64_t omitted_offset_{};
    std::uint64_t omitted_size_{};
    std::size_t reserved_tail_segment_{SIZE_MAX};
    std::uint64_t reserved_tail_mapped_{};
    bool dylib_{};
    bool bundle_{};
    bool classic_fixups_{};
    mutable bool bound_{};   // bind() applied and freed the fixups
    std::uint64_t preferred_base_{};
    std::uint64_t image_size_{};
    std::optional<std::uint64_t> entry_address_;
    std::uint32_t minimum_os_{};
    std::uint32_t sdk_version_{};
    std::uint64_t stack_size_{};
    std::string install_name_;
    std::vector<MachOSegment> segments_;
    std::vector<MachODependency> dependencies_;
    std::vector<MachOImport> imports_;
    std::vector<MachOExport> exports_;
    std::vector<std::string> runpaths_;
    mutable std::vector<Fixup> fixups_;
    std::vector<std::uint16_t> pointer_formats_;
    std::uint64_t fixups_offset_{};
    std::uint64_t fixups_size_{};
};

} // namespace alchemy
