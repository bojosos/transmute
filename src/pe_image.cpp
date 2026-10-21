#include "transmute/pe_image.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace transmute {
namespace {

constexpr std::uint16_t arm64_machine = 0xaa64;
constexpr std::size_t maximum_image_size = 512U * 1024U * 1024U;
constexpr std::uint64_t import_base = 0x0000600000000000ULL;

bool in_range(std::size_t offset, std::size_t length, std::size_t total) {
    return offset <= total && length <= total - offset;
}

std::uint16_t u16(std::span<const std::uint8_t> bytes, std::size_t offset) {
    if (!in_range(offset, 2, bytes.size())) throw std::runtime_error("truncated PE header");
    return static_cast<std::uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
}

std::uint32_t u32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(u16(bytes, offset)) |
           (static_cast<std::uint32_t>(u16(bytes, offset + 2)) << 16);
}

std::uint64_t u64(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint64_t>(u32(bytes, offset)) |
           (static_cast<std::uint64_t>(u32(bytes, offset + 4)) << 32);
}

std::string c_string(std::span<const std::uint8_t> bytes, std::size_t offset) {
    if (offset >= bytes.size()) throw std::runtime_error("PE string is outside image");
    std::string result;
    for (std::size_t i = 0; i < 65536 && offset + i < bytes.size(); ++i) {
        const auto value = bytes[offset + i];
        if (value == 0) return result;
        result.push_back(static_cast<char>(value));
    }
    throw std::runtime_error("unterminated PE string");
}

} // namespace

PeImage PeImage::parse(std::span<const std::uint8_t> file,
                       std::optional<std::uint64_t> load_base) {
    if (file.size() < 0x40 || u16(file, 0) != 0x5a4d) {
        throw std::runtime_error("not a Windows PE executable");
    }
    const auto pe = static_cast<std::size_t>(u32(file, 0x3c));
    if (!in_range(pe, 24, file.size()) || u32(file, pe) != 0x00004550) {
        throw std::runtime_error("invalid PE signature");
    }
    if (u16(file, pe + 4) != arm64_machine) {
        throw std::runtime_error("only plain ARM64 PE images are supported by this milestone");
    }
    const auto section_count = u16(file, pe + 6);
    const auto optional_size = u16(file, pe + 20);
    const auto optional = pe + 24;
    const bool is_dll = (u16(file, pe + 22) & 0x2000U) != 0;
    if (section_count == 0 || section_count > 96 || optional_size < 112 ||
        !in_range(optional, optional_size, file.size()) || u16(file, optional) != 0x20b) {
        throw std::runtime_error("invalid PE32+ headers");
    }

    const auto image_size = u32(file, optional + 56);
    const auto header_size = u32(file, optional + 60);
    const auto entry_rva = u32(file, optional + 16);
    const auto image_base = u64(file, optional + 24);
    const auto actual_base = load_base.value_or(image_base);
    if (load_base && (actual_base < 0x10000 || (actual_base & 0xffffU) != 0)) {
        throw std::runtime_error("requested PE load base must be 64 KiB aligned");
    }
    if (image_size == 0 || image_size > maximum_image_size || header_size > image_size ||
        !in_range(0, header_size, file.size()) || entry_rva >= image_size ||
        image_base > std::numeric_limits<std::uint64_t>::max() - image_size ||
        actual_base > std::numeric_limits<std::uint64_t>::max() - image_size) {
        throw std::runtime_error("invalid PE image layout");
    }

    PeImage image;
    image.base_ = actual_base;
    image.preferred_base_ = image_base;
    image.entry_rva_ = entry_rva;
    image.stack_reserve_ = u64(file, optional + 72);
    image.stack_commit_ = u64(file, optional + 80);
    image.is_dll_ = is_dll;
    image.bytes_.assign(image_size, 0);
    std::copy_n(file.begin(), header_size, image.bytes_.begin());

    const auto directory_count = u32(file, optional + 108);
    if (directory_count > 16 ||
        static_cast<std::uint64_t>(directory_count) * 8 + 112 > optional_size) {
        throw std::runtime_error("invalid PE data directories");
    }
    for (std::size_t i = 0; i < directory_count; ++i) {
        const auto rva = u32(file, optional + 112 + i * 8);
        const auto size = u32(file, optional + 116 + i * 8);
        if (i != 4 && size != 0 && !in_range(rva, size, image.bytes_.size())) {
            throw std::runtime_error("PE data directory extends beyond image");
        }
    }
    if (directory_count > 2) {
        image.resource_rva_ = u32(file, optional + 128);
        image.resource_size_ = u32(file, optional + 132);
        if ((image.resource_rva_ == 0) != (image.resource_size_ == 0) ||
            (image.resource_size_ != 0 && image.resource_size_ < 16)) {
            throw std::runtime_error("invalid PE resource directory");
        }
    }
    if (directory_count > 1) {
        image.has_imports_ = u32(file, optional + 120) != 0;
    }

    const auto section_table = optional + optional_size;
    if (!in_range(section_table, static_cast<std::size_t>(section_count) * 40, file.size())) {
        throw std::runtime_error("truncated PE section table");
    }
    for (std::size_t i = 0; i < section_count; ++i) {
        const auto at = section_table + i * 40;
        std::string name;
        for (std::size_t j = 0; j < 8 && file[at + j] != 0; ++j) {
            name.push_back(static_cast<char>(file[at + j]));
        }
        const auto virtual_size = u32(file, at + 8);
        const auto rva = u32(file, at + 12);
        const auto raw_size = u32(file, at + 16);
        const auto raw_offset = u32(file, at + 20);
        const auto size = std::max(virtual_size, raw_size);
        const auto flags = u32(file, at + 36);
        if (!in_range(rva, size, image.bytes_.size()) ||
            (size != 0 && rva < header_size) ||
            (raw_size != 0 && !in_range(raw_offset, raw_size, file.size()))) {
            throw std::runtime_error("PE section extends beyond its file or image");
        }
        for (const auto& prior : image.sections_) {
            if (size != 0 && prior.size != 0 &&
                rva < static_cast<std::uint64_t>(prior.rva) + prior.size &&
                prior.rva < static_cast<std::uint64_t>(rva) + size) {
                throw std::runtime_error("overlapping PE sections");
            }
        }
        if (raw_size != 0) {
            std::copy_n(file.begin() + raw_offset, raw_size, image.bytes_.begin() + rva);
        }
        image.sections_.push_back({name, rva, size, (flags & 0x20000000U) != 0,
                                   (flags & 0x80000000U) != 0});
    }

    bool executable_entry = false;
    for (const auto& section : image.sections_) {
        if (section.executable && entry_rva >= section.rva &&
            entry_rva < static_cast<std::uint64_t>(section.rva) + section.size) {
            executable_entry = true;
        }
    }
    if (entry_rva != 0 && !executable_entry) {
        throw std::runtime_error("PE entry point is not executable");
    }
    if (!is_dll && entry_rva == 0) {
        throw std::runtime_error("executable PE has no entry point");
    }

    if (actual_base != image_base) {
        const bool stripped = (u16(file, pe + 22) & 0x0001U) != 0;
        const auto reloc_rva = directory_count > 5 ? u32(file, optional + 152) : 0;
        const auto reloc_size = directory_count > 5 ? u32(file, optional + 156) : 0;
        if (stripped || reloc_rva == 0 || reloc_size == 0 ||
            !in_range(reloc_rva, reloc_size, image.bytes_.size())) {
            throw std::runtime_error("PE image cannot be loaded away from its preferred base");
        }
        const auto delta = actual_base - image_base;
        for (std::size_t offset = 0; offset < reloc_size;) {
            const auto block = static_cast<std::size_t>(reloc_rva) + offset;
            if (reloc_size - offset < 8) throw std::runtime_error("truncated PE relocation block");
            const auto page_rva = u32(image.bytes_, block);
            const auto block_size = u32(image.bytes_, block + 4);
            if ((page_rva & 0xfffU) != 0 || block_size < 8 ||
                (block_size & 3U) != 0 || block_size > reloc_size - offset) {
                throw std::runtime_error("invalid PE relocation block");
            }
            for (std::size_t entry_offset = 8; entry_offset < block_size; entry_offset += 2) {
                const auto entry = u16(image.bytes_, block + entry_offset);
                const auto type = entry >> 12;
                if (type == 0) continue;
                if (type != 10) throw std::runtime_error("unsupported PE relocation type");
                const auto target = static_cast<std::uint64_t>(page_rva) + (entry & 0xfffU);
                if (!in_range(target, 8, image.bytes_.size())) {
                    throw std::runtime_error("PE relocation target extends beyond image");
                }
                const auto value = u64(image.bytes_, static_cast<std::size_t>(target)) + delta;
                for (unsigned byte = 0; byte < 8; ++byte) {
                    image.bytes_[static_cast<std::size_t>(target) + byte] =
                        static_cast<std::uint8_t>(value >> (byte * 8));
                }
            }
            offset += block_size;
        }
    }

    if (directory_count > 9) {
        const auto tls_rva = u32(file, optional + 184);
        const auto tls_size = u32(file, optional + 188);
        if (tls_rva != 0) {
            if (tls_size < 40 || !in_range(tls_rva, 40, image.bytes_.size())) {
                throw std::runtime_error("invalid PE TLS directory");
            }
            const auto raw_start = u64(image.bytes_, tls_rva);
            const auto raw_end = u64(image.bytes_, tls_rva + 8);
            const auto index_address = u64(image.bytes_, tls_rva + 16);
            const auto callback_address = u64(image.bytes_, tls_rva + 24);
            const auto zero_fill = u32(image.bytes_, tls_rva + 32);
            const auto rva_of = [&](std::uint64_t address, std::size_t length) {
                if (address < actual_base ||
                    !in_range(address - actual_base, length, image.bytes_.size())) {
                    throw std::runtime_error("PE TLS address is outside image");
                }
                return static_cast<std::size_t>(address - actual_base);
            };
            TlsDirectory tls;
            tls.index_address = index_address;
            static_cast<void>(rva_of(index_address, 4));
            if (raw_start != 0 || raw_end != 0) {
                if (raw_end < raw_start || raw_end - raw_start > 16U * 1024U * 1024U) {
                    throw std::runtime_error("invalid PE TLS template size");
                }
                const auto start = rva_of(raw_start,
                    static_cast<std::size_t>(raw_end - raw_start));
                tls.initial_data.assign(image.bytes_.begin() + start,
                    image.bytes_.begin() + start + (raw_end - raw_start));
            }
            if (zero_fill > 16U * 1024U * 1024U ||
                tls.initial_data.size() + zero_fill > 16U * 1024U * 1024U) {
                throw std::runtime_error("PE TLS allocation is too large");
            }
            tls.zero_fill = zero_fill;
            if (callback_address != 0) {
                const auto callbacks_rva = rva_of(callback_address, 8);
                bool terminated = false;
                for (std::size_t index = 0; index < 1024; ++index) {
                    const auto at = callbacks_rva + index * 8;
                    if (!in_range(at, 8, image.bytes_.size())) break;
                    const auto callback = u64(image.bytes_, at);
                    if (callback == 0) {
                        terminated = true;
                        break;
                    }
                    static_cast<void>(rva_of(callback, 4));
                    tls.callbacks.push_back(callback);
                }
                if (!terminated) throw std::runtime_error("unterminated PE TLS callbacks");
            }
            image.tls_ = std::move(tls);
        }
    }

    if (directory_count > 0) {
        const auto export_rva = u32(file, optional + 112);
        const auto export_size = u32(file, optional + 116);
        if (export_rva != 0) {
            if (export_size < 40 || !in_range(export_rva, export_size, image.bytes_.size())) {
                throw std::runtime_error("invalid PE export directory");
            }
            const auto names_count = u32(image.bytes_, export_rva + 24);
            const auto functions_count = u32(image.bytes_, export_rva + 20);
            const auto ordinal_base = u32(image.bytes_, export_rva + 16);
            const auto functions_rva = u32(image.bytes_, export_rva + 28);
            const auto names_rva = u32(image.bytes_, export_rva + 32);
            const auto ordinals_rva = u32(image.bytes_, export_rva + 36);
            if (names_count > 65536 || functions_count > 65536 ||
                names_count > functions_count ||
                !in_range(functions_rva, static_cast<std::size_t>(functions_count) * 4,
                          image.bytes_.size()) ||
                !in_range(names_rva, static_cast<std::size_t>(names_count) * 4,
                          image.bytes_.size()) ||
                !in_range(ordinals_rva, static_cast<std::size_t>(names_count) * 2,
                          image.bytes_.size())) {
                throw std::runtime_error("invalid PE export tables");
            }
            const auto export_function = [&](std::size_t index) -> Export {
                const auto function_rva = u32(image.bytes_, functions_rva + index * 4);
                if (function_rva == 0) return {};
                if (function_rva >= export_rva &&
                    function_rva - export_rva < export_size) {
                    const auto end = static_cast<std::size_t>(export_rva) + export_size;
                    const auto limit = std::min(end,
                        static_cast<std::size_t>(function_rva) + 65536);
                    auto terminator = static_cast<std::size_t>(function_rva);
                    while (terminator < limit && image.bytes_[terminator] != 0) {
                        ++terminator;
                    }
                    if (terminator == limit) {
                        throw std::runtime_error("unterminated PE export forwarder");
                    }
                    std::string forwarder(image.bytes_.begin() + function_rva,
                                          image.bytes_.begin() + terminator);
                    const auto separator = forwarder.rfind('.');
                    if (separator == std::string::npos || separator == 0 ||
                        separator + 1 == forwarder.size()) {
                        throw std::runtime_error("invalid PE export forwarder");
                    }
                    if (forwarder[separator + 1] == '#') {
                        if (separator + 2 == forwarder.size()) {
                            throw std::runtime_error("invalid PE export forwarder ordinal");
                        }
                        std::uint32_t ordinal = 0;
                        for (std::size_t digit = separator + 2;
                             digit < forwarder.size(); ++digit) {
                            if (forwarder[digit] < '0' || forwarder[digit] > '9') {
                                throw std::runtime_error("invalid PE export forwarder ordinal");
                            }
                            ordinal = ordinal * 10 + (forwarder[digit] - '0');
                            if (ordinal > 0xffffU) {
                                throw std::runtime_error("invalid PE export forwarder ordinal");
                            }
                        }
                    }
                    return {"", 0, std::move(forwarder)};
                }
                bool mapped = false;
                for (const auto& section : image.sections_) {
                    if (function_rva >= section.rva &&
                        function_rva < static_cast<std::uint64_t>(section.rva) + section.size) {
                        mapped = true;
                    }
                }
                if (!mapped) throw std::runtime_error("PE export is outside mapped sections");
                return {"", function_rva, ""};
            };
            for (std::size_t index = 0; index < functions_count; ++index) {
                auto target = export_function(index);
                const auto ordinal = static_cast<std::uint64_t>(ordinal_base) + index;
                if ((target.rva != 0 || !target.forwarder.empty()) && ordinal <= 0xffffU) {
                    target.name = "#" + std::to_string(ordinal);
                    image.exports_.push_back(std::move(target));
                }
            }
            for (std::size_t index = 0; index < names_count; ++index) {
                const auto name_rva = u32(image.bytes_, names_rva + index * 4);
                const auto ordinal = u16(image.bytes_, ordinals_rva + index * 2);
                if (ordinal >= functions_count) {
                    throw std::runtime_error("invalid PE export ordinal");
                }
                auto target = export_function(ordinal);
                if (target.rva == 0 && target.forwarder.empty()) {
                    throw std::runtime_error("named PE export has no target");
                }
                target.name = c_string(image.bytes_, name_rva);
                image.exports_.push_back(std::move(target));
            }
        }
    }

    if (image.has_imports_) {
        const auto directory_rva = u32(file, optional + 120);
        const auto directory_size = u32(file, optional + 124);
        if (directory_size < 40 || !in_range(directory_rva, directory_size, image.bytes_.size())) {
            throw std::runtime_error("invalid PE import directory");
        }
        bool terminated = false;
        for (std::size_t offset = 0; offset + 20 <= directory_size; offset += 20) {
            const auto descriptor = static_cast<std::size_t>(directory_rva) + offset;
            const auto lookup_rva = u32(image.bytes_, descriptor);
            const auto name_rva = u32(image.bytes_, descriptor + 12);
            const auto iat_rva = u32(image.bytes_, descriptor + 16);
            if (lookup_rva == 0 && name_rva == 0 && iat_rva == 0) {
                terminated = true;
                break;
            }
            if (name_rva == 0 || iat_rva == 0) {
                throw std::runtime_error("invalid PE import descriptor");
            }
            const auto dll = c_string(image.bytes_, name_rva);
            const auto thunk_rva = lookup_rva == 0 ? iat_rva : lookup_rva;
            bool thunk_terminated = false;
            for (std::size_t index = 0; index < 4096; ++index) {
                const auto lookup_at = static_cast<std::uint64_t>(thunk_rva) + index * 8;
                const auto iat_at = static_cast<std::uint64_t>(iat_rva) + index * 8;
                if (!in_range(lookup_at, 8, image.bytes_.size()) ||
                    !in_range(iat_at, 8, image.bytes_.size())) {
                    throw std::runtime_error("PE import table extends beyond image");
                }
                const auto target = u64(image.bytes_, static_cast<std::size_t>(lookup_at));
                if (target == 0) {
                    thunk_terminated = true;
                    break;
                }
                std::string function;
                if ((target & (1ULL << 63)) != 0) {
                    if ((target & 0x7fffffffffff0000ULL) != 0) {
                        throw std::runtime_error("invalid PE ordinal import in " + dll);
                    }
                    function = "#" + std::to_string(target & 0xffffU);
                } else {
                    if (target > 0xffffffffULL ||
                        !in_range(static_cast<std::size_t>(target), 2, image.bytes_.size())) {
                        throw std::runtime_error("invalid PE import in " + dll +
                                                 " at thunk " + std::to_string(index));
                    }
                    function = c_string(image.bytes_, static_cast<std::size_t>(target) + 2);
                }
                if (image.imports_.size() >= 4096) {
                    throw std::runtime_error("too many PE imports");
                }
                const auto address = import_base + image.imports_.size() * 16;
                if (image.base_ <= address && address - image.base_ < image.bytes_.size()) {
                    throw std::runtime_error("PE image overlaps import bridge range");
                }
                image.imports_.push_back({dll, function, address,
                                          static_cast<std::uint32_t>(iat_at)});
                for (unsigned byte = 0; byte < 8; ++byte) {
                    image.bytes_[static_cast<std::size_t>(iat_at) + byte] =
                        static_cast<std::uint8_t>(address >> (byte * 8));
                }
            }
            if (!thunk_terminated) throw std::runtime_error("unterminated PE import thunk table");
        }
        if (!terminated) throw std::runtime_error("unterminated PE import directory");
    }
    return image;
}

const Import* PeImage::import_at(std::uint64_t address) const noexcept {
    for (const auto& imported : imports_) {
        if (imported.entry != 0 && imported.entry == address) return &imported;
    }
    return nullptr;
}

std::optional<std::uint64_t> PeImage::find_export(const std::string& name) const {
    for (const auto& exported : exports_) {
        if (exported.name == name && exported.forwarder.empty()) {
            return base_ + exported.rva;
        }
    }
    return std::nullopt;
}

std::optional<ExportTarget> PeImage::find_export_target(
    const std::string& name) const {
    for (const auto& exported : exports_) {
        if (exported.name == name) {
            return ExportTarget{exported.forwarder.empty()
                ? std::optional<std::uint64_t>(base_ + exported.rva)
                : std::nullopt, exported.forwarder};
        }
    }
    return std::nullopt;
}

std::optional<std::span<const std::uint8_t>> PeImage::find_resource(
    std::uint16_t type, std::uint16_t id,
    std::optional<std::uint16_t> language) const {
    return find_resource_impl(type, id, {}, language);
}

std::optional<std::span<const std::uint8_t>> PeImage::find_resource(
    std::uint16_t type, std::u16string_view name,
    std::optional<std::uint16_t> language) const {
    return find_resource_impl(type, std::nullopt, name, language);
}

std::optional<std::span<const std::uint8_t>> PeImage::find_resource_impl(
    std::uint16_t type, std::optional<std::uint16_t> id,
    std::u16string_view name, std::optional<std::uint16_t> language) const {
    if (resource_size_ == 0) return std::nullopt;
    const auto require_resource = [&](std::size_t offset, std::size_t length) {
        if (!in_range(offset, length, resource_size_) ||
            !in_range(static_cast<std::size_t>(resource_rva_) + offset,
                      length, bytes_.size())) {
            throw std::runtime_error("invalid PE resource directory offset");
        }
        return static_cast<std::size_t>(resource_rva_) + offset;
    };
    const auto find_entry = [&](std::uint32_t directory,
                                std::optional<std::uint16_t> number,
                                std::u16string_view text, bool any)
        -> std::optional<std::uint32_t> {
        const auto at = require_resource(directory, 16);
        const auto named = u16(bytes_, at + 12);
        const auto numbered = u16(bytes_, at + 14);
        const auto count = static_cast<std::size_t>(named) + numbered;
        require_resource(static_cast<std::size_t>(directory) + 16, count * 8);
        const auto first = number || (any && numbered != 0) ? named : 0;
        const auto last = number || (any && numbered != 0) ? count : named;
        for (std::size_t index = first; index < last; ++index) {
            const auto entry = at + 16 + index * 8;
            const auto key = u32(bytes_, entry);
            if (number) {
                if (key != *number) continue;
            } else if (!any) {
                if ((key & 0x80000000U) == 0) continue;
                const auto string_offset = key & 0x7fffffffU;
                const auto string_at = require_resource(string_offset, 2);
                const auto length = u16(bytes_, string_at);
                require_resource(static_cast<std::size_t>(string_offset) + 2,
                                 static_cast<std::size_t>(length) * 2);
                if (length != text.size()) continue;
                bool matches = true;
                for (std::size_t character = 0; character < length; ++character) {
                    if (u16(bytes_, string_at + 2 + character * 2) !=
                        text[character]) {
                        matches = false;
                        break;
                    }
                }
                if (!matches) continue;
            }
            return u32(bytes_, entry + 4);
        }
        return std::nullopt;
    };
    const auto type_entry = find_entry(0, type, {}, false);
    if (!type_entry) return std::nullopt;
    if ((*type_entry & 0x80000000U) == 0) {
        throw std::runtime_error("invalid PE resource type directory");
    }
    const auto name_entry = find_entry(*type_entry & 0x7fffffffU, id, name, false);
    if (!name_entry) return std::nullopt;
    if ((*name_entry & 0x80000000U) == 0) {
        throw std::runtime_error("invalid PE resource name directory");
    }
    const auto language_entry = find_entry(*name_entry & 0x7fffffffU,
                                           language, {}, !language);
    if (!language_entry) return std::nullopt;
    if ((*language_entry & 0x80000000U) != 0) {
        throw std::runtime_error("invalid PE resource language entry");
    }
    const auto data = require_resource(*language_entry, 16);
    const auto data_rva = u32(bytes_, data);
    const auto data_size = u32(bytes_, data + 4);
    if (!in_range(data_rva, data_size, bytes_.size())) {
        throw std::runtime_error("PE resource data extends beyond image");
    }
    return std::span<const std::uint8_t>(bytes_).subspan(data_rva, data_size);
}

void PeImage::bind_import(std::size_t index, std::uint64_t target) {
    if (index >= imports_.size()) throw std::runtime_error("invalid PE import index");
    const auto at = imports_[index].iat_rva;
    for (unsigned byte = 0; byte < 8; ++byte) {
        bytes_[at + byte] = static_cast<std::uint8_t>(target >> (byte * 8));
    }
    imports_[index].entry = 0;
}

void PeImage::set_import_bridge_base(std::uint64_t bridge_base) {
    if (bridge_base > std::numeric_limits<std::uint64_t>::max() - imports_.size() * 16) {
        throw std::runtime_error("invalid import bridge base");
    }
    for (std::size_t index = 0; index < imports_.size(); ++index) {
        if (imports_[index].entry == 0) continue;
        const auto address = bridge_base + index * 16;
        if (base_ <= address && address - base_ < bytes_.size()) {
            throw std::runtime_error("PE image overlaps import bridge range");
        }
        const auto at = imports_[index].iat_rva;
        for (unsigned byte = 0; byte < 8; ++byte) {
            bytes_[at + byte] = static_cast<std::uint8_t>(address >> (byte * 8));
        }
        imports_[index].entry = address;
    }
}

std::uint8_t PeImage::read8(std::uint64_t address) const {
    if (address < base_ || address - base_ >= bytes_.size()) {
        throw std::runtime_error("guest read outside PE image");
    }
    return bytes_[static_cast<std::size_t>(address - base_)];
}

void PeImage::write8(std::uint64_t address, std::uint8_t value) {
    if (address < base_ || address - base_ >= bytes_.size()) {
        throw std::runtime_error("guest write outside PE image");
    }
    const auto rva = address - base_;
    for (const auto& section : sections_) {
        if (section.writable && rva >= section.rva &&
            rva < static_cast<std::uint64_t>(section.rva) + section.size) {
            bytes_[static_cast<std::size_t>(rva)] = value;
            return;
        }
    }
    throw std::runtime_error("guest write to read-only PE memory");
}

std::optional<std::uint64_t> PeImage::try_read_contiguous(
    std::uint64_t address, unsigned bytes) const {
    if ((bytes != 1 && bytes != 2 && bytes != 4 && bytes != 8) ||
        address < base_ || address - base_ >= bytes_.size() ||
        bytes > bytes_.size() - static_cast<std::size_t>(address - base_)) {
        return std::nullopt;
    }
    const auto offset = static_cast<std::size_t>(address - base_);
    std::uint64_t value{};
    if constexpr (std::endian::native == std::endian::little) {
        std::memcpy(&value, bytes_.data() + offset, bytes);
    } else {
        for (unsigned index = 0; index < bytes; ++index) {
            value |= static_cast<std::uint64_t>(bytes_[offset + index]) << (index * 8);
        }
    }
    return value;
}

bool PeImage::try_write_contiguous(std::uint64_t address, unsigned bytes,
                                   std::uint64_t value) {
    if ((bytes != 1 && bytes != 2 && bytes != 4 && bytes != 8) ||
        address < base_ || address - base_ >= bytes_.size() ||
        bytes > bytes_.size() - static_cast<std::size_t>(address - base_)) {
        return false;
    }
    const auto rva = address - base_;
    for (const auto& section : sections_) {
        if (section.writable && rva >= section.rva &&
            rva - section.rva < section.size &&
            bytes <= section.size - (rva - section.rva)) {
            if constexpr (std::endian::native == std::endian::little) {
                std::memcpy(bytes_.data() + static_cast<std::size_t>(rva), &value, bytes);
            } else {
                for (unsigned index = 0; index < bytes; ++index) {
                    bytes_[static_cast<std::size_t>(rva) + index] =
                        static_cast<std::uint8_t>(value >> (index * 8));
                }
            }
            return true;
        }
    }
    return false;
}

std::uint32_t PeImage::instruction(std::uint64_t address) const {
    if ((address & 3U) != 0 || address < base_ || address - base_ > bytes_.size() ||
        bytes_.size() - static_cast<std::size_t>(address - base_) < 4) {
        throw std::runtime_error("invalid guest instruction address");
    }
    const auto rva = address - base_;
    bool executable = false;
    for (const auto& section : sections_) {
        if (section.executable && rva >= section.rva &&
            rva + 4 <= static_cast<std::uint64_t>(section.rva) + section.size) {
            executable = true;
        }
    }
    if (!executable) throw std::runtime_error("guest attempted to execute non-code memory");
    const auto offset = static_cast<std::size_t>(rva);
    return u32(bytes_, offset);
}

} // namespace transmute
