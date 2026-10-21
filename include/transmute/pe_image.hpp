#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace transmute {

struct Section {
    std::string name;
    std::uint32_t rva;
    std::uint32_t size;
    bool executable;
    bool writable;
};

struct Import {
    std::string dll;
    std::string function;
    std::uint64_t entry;
    std::uint32_t iat_rva{};
};

struct TlsDirectory {
    std::uint64_t index_address{};
    std::vector<std::uint8_t> initial_data;
    std::uint32_t zero_fill{};
    std::vector<std::uint64_t> callbacks;
};

struct ExportTarget {
    std::optional<std::uint64_t> address;
    std::string forwarder;
};

class PeImage {
public:
    static PeImage parse(std::span<const std::uint8_t> file,
                         std::optional<std::uint64_t> load_base = std::nullopt);

    [[nodiscard]] std::uint64_t base() const noexcept { return base_; }
    [[nodiscard]] const std::string& source() const noexcept { return source_; }
    void set_source(std::string source) { source_ = std::move(source); }
    [[nodiscard]] std::uint64_t preferred_base() const noexcept { return preferred_base_; }
    [[nodiscard]] std::uint64_t entry() const noexcept { return base_ + entry_rva_; }
    [[nodiscard]] std::uint32_t entry_rva() const noexcept { return entry_rva_; }
    [[nodiscard]] bool is_dll() const noexcept { return is_dll_; }
    // Optional-header SizeOfStackReserve/SizeOfStackCommit.
    [[nodiscard]] std::uint64_t stack_reserve() const noexcept { return stack_reserve_; }
    [[nodiscard]] std::uint64_t stack_commit() const noexcept { return stack_commit_; }
    [[nodiscard]] bool has_imports() const noexcept { return has_imports_; }
    [[nodiscard]] const std::vector<Import>& imports() const noexcept { return imports_; }
    [[nodiscard]] const std::optional<TlsDirectory>& tls() const noexcept { return tls_; }
    [[nodiscard]] const Import* import_at(std::uint64_t address) const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> find_export(const std::string& name) const;
    [[nodiscard]] std::optional<ExportTarget> find_export_target(
        const std::string& name) const;
    [[nodiscard]] std::optional<std::span<const std::uint8_t>> find_resource(
        std::uint16_t type, std::uint16_t id,
        std::optional<std::uint16_t> language = std::nullopt) const;
    [[nodiscard]] std::optional<std::span<const std::uint8_t>> find_resource(
        std::uint16_t type, std::u16string_view name,
        std::optional<std::uint16_t> language = std::nullopt) const;
    void bind_import(std::size_t index, std::uint64_t target);
    void set_import_bridge_base(std::uint64_t bridge_base);
    [[nodiscard]] const std::vector<Section>& sections() const noexcept { return sections_; }
    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }

    [[nodiscard]] std::uint8_t read8(std::uint64_t address) const;
    void write8(std::uint64_t address, std::uint8_t value);
    // Whole-width accesses return failure without touching memory when the
    // image extent or a writable section cannot contain the complete access.
    [[nodiscard]] std::optional<std::uint64_t> try_read_contiguous(
        std::uint64_t address, unsigned bytes) const;
    bool try_write_contiguous(std::uint64_t address, unsigned bytes,
                              std::uint64_t value);
    [[nodiscard]] std::uint32_t instruction(std::uint64_t address) const;

private:
    // Engine-issued native read leases retain the address-space lock for the
    // entire borrowed bytes view; no raw view is exposed to general callers.
    friend class ReferenceEngine;
    struct Export {
        std::string name;
        std::uint32_t rva;
        std::string forwarder;
    };

    std::string source_;
    std::uint64_t base_{};
    std::uint64_t preferred_base_{};
    std::uint32_t entry_rva_{};
    std::uint64_t stack_reserve_{};
    std::uint64_t stack_commit_{};
    bool is_dll_{};
    bool has_imports_{};
    std::vector<Section> sections_;
    std::vector<Import> imports_;
    std::optional<TlsDirectory> tls_;
    std::uint32_t resource_rva_{};
    std::uint32_t resource_size_{};
    std::vector<Export> exports_;
    std::vector<std::uint8_t> bytes_;

    [[nodiscard]] std::optional<std::span<const std::uint8_t>> find_resource_impl(
        std::uint16_t type, std::optional<std::uint16_t> id,
        std::u16string_view name, std::optional<std::uint16_t> language) const;
};

} // namespace transmute
