#include "alchemy/darwin/appkit/asset_catalog.hpp"

#include <bit>
#include <cstring>
#include <set>
#include <stdexcept>
#include <unordered_map>

// Assets.car reader; the format is described in asset_catalog.hpp.
namespace alchemy::gui::car {
namespace {

[[noreturn]] void bad(std::string_view why) { throw std::runtime_error("asset catalog: " + std::string(why)); }

using Bytes = std::span<const std::uint8_t>;

std::uint32_t be32(Bytes b, std::size_t at) {
    if (at > b.size() || b.size() - at < 4) bad("truncated");
    return (std::uint32_t{b[at]} << 24) | (std::uint32_t{b[at + 1]} << 16) | (std::uint32_t{b[at + 2]} << 8) | b[at + 3];
}
std::uint16_t be16(Bytes b, std::size_t at) {
    if (at > b.size() || b.size() - at < 2) bad("truncated");
    return static_cast<std::uint16_t>((b[at] << 8) | b[at + 1]);
}
std::uint32_t le32(Bytes b, std::size_t at) {
    if (at > b.size() || b.size() - at < 4) bad("truncated");
    return std::uint32_t{b[at]} | (std::uint32_t{b[at + 1]} << 8) | (std::uint32_t{b[at + 2]} << 16) | (std::uint32_t{b[at + 3]} << 24);
}
std::uint16_t le16(Bytes b, std::size_t at) {
    if (at > b.size() || b.size() - at < 2) bad("truncated");
    return static_cast<std::uint16_t>(b[at] | (b[at + 1] << 8));
}
double le_double(Bytes b, std::size_t at) {
    if (at > b.size() || b.size() - at < 8) bad("truncated");
    std::uint64_t bits{};
    for (int i = 7; i >= 0; --i) bits = (bits << 8) | b[at + static_cast<std::size_t>(i)];
    return std::bit_cast<double>(bits);
}

struct Bom {
    Bytes file;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> blocks;   // (address, length)
    std::unordered_map<std::string, std::uint32_t> variables;

    Bytes block(std::uint32_t index) const {
        if (index >= blocks.size()) bad("block index out of range");
        const auto [address, length] = blocks[index];
        if (address > file.size() || file.size() - address < length) bad("block outside the file");
        return file.subspan(address, length);
    }
    std::optional<std::uint32_t> variable(std::string_view name) const {
        const auto found = variables.find(std::string(name));
        if (found == variables.end()) return std::nullopt;
        return found->second;
    }

    // Every (key, value) of a B-tree, in leaf order.
    std::vector<std::pair<Bytes, Bytes>> tree(std::uint32_t index) const {
        const auto header = block(index);
        if (header.size() < 21 || std::memcmp(header.data(), "tree", 4) != 0) bad("bad tree header");
        std::vector<std::pair<Bytes, Bytes>> entries;
        std::set<std::uint32_t> visited;
        auto node = be32(header, 8);
        // Descend to the leftmost leaf.
        for (;;) {
            if (!visited.insert(node).second) bad("tree cycle");
            const auto b = block(node);
            const auto leaf = be16(b, 0), count = be16(b, 2);
            if (leaf) break;
            if (!count) bad("empty tree branch");
            node = be32(b, 12);
        }
        visited.clear();
        while (node) {
            if (!visited.insert(node).second) bad("tree leaf cycle");
            const auto b = block(node);
            if (!be16(b, 0)) bad("tree leaf chain reaches a branch");
            const auto count = be16(b, 2);
            for (std::size_t i = 0; i < count; ++i) {
                const auto value = be32(b, 12 + 8 * i), key = be32(b, 16 + 8 * i);
                entries.emplace_back(block(key), block(value));
            }
            node = be32(b, 4);
        }
        return entries;
    }
};

Bom open(Bytes bytes) {
    if (!AssetCatalog::is_asset_catalog(bytes)) bad("not a BOMStore file");
    Bom bom{bytes, {}, {}};
    const auto index_offset = be32(bytes, 16), variables_offset = be32(bytes, 24);
    const auto count = be32(bytes, index_offset);
    if (count > (bytes.size() - index_offset) / 8) bad("block index larger than the file");
    bom.blocks.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i)
        bom.blocks.emplace_back(be32(bytes, index_offset + 4 + 8 * std::size_t{i}), be32(bytes, index_offset + 8 + 8 * std::size_t{i}));
    const auto variable_count = be32(bytes, variables_offset);
    std::size_t at = variables_offset + 4;
    for (std::uint32_t i = 0; i < variable_count; ++i) {
        const auto block = be32(bytes, at);
        if (at + 4 >= bytes.size()) bad("truncated variables");
        const std::size_t length = bytes[at + 4];
        if (bytes.size() - (at + 5) < length) bad("truncated variable name");
        bom.variables[std::string(reinterpret_cast<const char*>(bytes.data() + at + 5), length)] = block;
        at += 5 + length;
    }
    return bom;
}

std::string as_string(Bytes bytes) {
    std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (const auto nul = text.find('\0'); nul != std::string::npos) text.resize(nul);
    return text;
}

constexpr std::uint32_t attribute_appearance = 7;
constexpr std::uint32_t attribute_name = 17;
constexpr std::uint16_t layout_named_color = 1009;
constexpr std::size_t csi_header_size = 184;

} // namespace

bool AssetCatalog::is_asset_catalog(Bytes bytes) {
    return bytes.size() >= 32 && std::memcmp(bytes.data(), "BOMStore", 8) == 0;
}

AssetCatalog AssetCatalog::parse(Bytes bytes) {
    const auto bom = open(bytes);
    const auto renditions_block = bom.variable("RENDITIONS"), facets_block = bom.variable("FACETKEYS"), format_block = bom.variable("KEYFORMAT");
    if (!renditions_block || !facets_block || !format_block) bad("RENDITIONS, FACETKEYS or KEYFORMAT is missing");

    const auto format = bom.block(*format_block);
    if (format.size() < 12 || std::memcmp(format.data(), "tmfk", 4) != 0) bad("bad KEYFORMAT");
    const auto attribute_count = le32(format, 8);
    if (attribute_count > (format.size() - 12) / 4) bad("KEYFORMAT larger than its block");
    std::optional<std::size_t> name_slot, appearance_slot;
    for (std::uint32_t i = 0; i < attribute_count; ++i) {
        const auto attribute = le32(format, 12 + 4 * std::size_t{i});
        if (attribute == attribute_name) name_slot = i;
        if (attribute == attribute_appearance) appearance_slot = i;
    }
    if (!name_slot) bad("rendition keys carry no name identifier");

    std::unordered_map<std::uint16_t, std::string> names;   // name id -> asset name
    for (const auto& [key, value] : bom.tree(*facets_block)) {
        const auto count = le16(value, 4);
        for (std::size_t i = 0; i < count; ++i)
            if (le16(value, 6 + 4 * i) == attribute_name) names[le16(value, 8 + 4 * i)] = as_string(key);
    }
    std::unordered_map<std::uint16_t, std::string> appearances;   // appearance id -> name
    if (const auto appearance_block = bom.variable("APPEARANCEKEYS"))
        for (const auto& [key, value] : bom.tree(*appearance_block)) appearances[le16(value, 0)] = as_string(key);

    AssetCatalog catalog;
    for (const auto& [key, value] : bom.tree(*renditions_block)) {
        if (key.size() < 2 * std::size_t{attribute_count}) bad("rendition key shorter than KEYFORMAT");
        if (value.size() < csi_header_size || std::memcmp(value.data(), "ISTC", 4) != 0) bad("bad rendition header");
        if (le16(value, 36) != layout_named_color) continue;   // images and other renditions: not read yet
        const auto tlv_length = le32(value, 168), data_length = le32(value, 180);
        if (tlv_length > value.size() - csi_header_size || data_length > value.size() - csi_header_size - tlv_length)
            bad("rendition data outside its block");
        const auto data = value.subspan(csi_header_size + tlv_length, data_length);
        if (data.size() < 16 || std::memcmp(data.data(), "RLOC", 4) != 0) bad("named color without color data");
        NamedColor color;
        color.color_space = le32(data, 8) & 0xff;
        const auto components = le32(data, 12);
        if (components == 0 || components > (data.size() - 16) / 8) bad("named color component count");
        for (std::uint32_t i = 0; i < components; ++i) color.components.push_back(le_double(data, 16 + 8 * std::size_t{i}));
        const auto name_id = le16(key, 2 * *name_slot);
        const auto name = names.find(name_id);
        if (name == names.end()) bad("named color without a facet name");
        if (appearance_slot) {
            const auto id = le16(key, 2 * *appearance_slot);
            if (const auto found = appearances.find(id); found != appearances.end()) color.appearance = found->second;
            else if (id != 0) bad("named color with an unknown appearance id");
        }
        catalog.colors_[name->second].push_back(std::move(color));
    }
    return catalog;
}

const std::vector<NamedColor>* AssetCatalog::color_variants(std::string_view name) const {
    const auto found = colors_.find(name);
    return found == colors_.end() ? nullptr : &found->second;
}

std::optional<NamedColor> AssetCatalog::color(std::string_view name, std::string_view appearance) const {
    const auto variants = color_variants(name);
    if (!variants || variants->empty()) return std::nullopt;
    for (const std::string_view wanted : {appearance, std::string_view("NSAppearanceNameSystem"), std::string_view()})
        for (const auto& variant : *variants)
            if (variant.appearance == wanted) return variant;
    return variants->front();
}

std::vector<std::string> AssetCatalog::color_names() const {
    std::vector<std::string> result;
    for (const auto& [name, variants] : colors_) result.push_back(name);
    return result;
}

} // namespace alchemy::gui::car
