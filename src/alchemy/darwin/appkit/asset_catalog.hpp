#pragma once

// Compiled asset catalogs (owner: AppKit). actool compiles an app's
// .xcassets into Contents/Resources/Assets.car, a "BOMStore" file. Only
// named colors are read so far (+[NSColor colorNamed:]); image renditions
// are skipped, because +[NSImage imageNamed:] does not use the catalog yet.
//
// Format (project-authored from the published descriptions of the BOM and
// CoreUI rendition layouts; no Apple code):
//   BOM header (big-endian): "BOMStore", uint32 version, block count, index
//     offset, index length, variables offset, variables length.
//   Block index: uint32 count, then count (address, length) pairs.
//   Variables: uint32 count, then (uint32 block, uint8 name length, name).
//     A catalog names the blocks CARHEADER, RENDITIONS, FACETKEYS,
//     APPEARANCEKEYS, KEYFORMAT, ...
//   B-tree block: "tree", uint32 version, uint32 root block, uint32 node
//     size, uint32 path count, uint8. Node: uint16 is-leaf, uint16 count,
//     uint32 forward leaf, uint32 backward leaf, then count (uint32 value
//     block, uint32 key block). The leftmost leaf is reached through first
//     children, and leaves are chained by their forward links.
//   The CoreUI payloads inside the blocks are little-endian:
//   KEYFORMAT: "tmfk", uint32 version, uint32 count, count uint32 attribute
//     identifiers: the order of the uint16 values in a rendition key.
//   FACETKEYS: key = asset name; value = uint16 hotspot x, y, uint16 count,
//     count (uint16 attribute, uint16 value). Attribute 17 is the name id.
//   APPEARANCEKEYS: key = appearance name ("NSAppearanceNameSystem"),
//     value = uint16 id: attribute 7 of a rendition key.
//   RENDITIONS: key = rendition key; value = CSI header: "ISTC", uint32
//     version, flags, width, height, scale, pixel format, color space,
//     modification time, uint16 layout, uint16, char name[128], uint32 TLV
//     length, uint32, uint32, uint32 rendition length, the TLV bytes, then
//     the rendition data. Named colors are layout 1009 with data "RLOC":
//     uint32 version, uint32 color space (low byte), uint32 component count,
//     count float64 components (color then alpha).
//
// The reader takes untrusted bytes and throws std::runtime_error on any
// inconsistency in the parts it reads.

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy::gui::car {

// Color space ids of named-color renditions seen in compiled catalogs.
enum class ColorSpace : std::uint8_t { srgb = 1, wide_rgb = 4, gray = 6 };

struct NamedColor {
    std::string appearance;            // "NSAppearanceNameSystem", "" when the catalog names none
    std::uint32_t color_space{};       // raw id (ColorSpace values are the known ones)
    std::vector<double> components;    // color components, then alpha
};

class AssetCatalog {
public:
    [[nodiscard]] static bool is_asset_catalog(std::span<const std::uint8_t> bytes);
    [[nodiscard]] static AssetCatalog parse(std::span<const std::uint8_t> bytes);

    // Every appearance variant of a named color (empty when the name is not a color).
    [[nodiscard]] const std::vector<NamedColor>* color_variants(std::string_view name) const;
    // The variant for an appearance: an exact match, else NSAppearanceNameSystem,
    // else the variant without an appearance, else the first. nullopt when absent.
    [[nodiscard]] std::optional<NamedColor> color(std::string_view name, std::string_view appearance) const;
    [[nodiscard]] std::size_t color_count() const { return colors_.size(); }
    [[nodiscard]] std::vector<std::string> color_names() const;

private:
    std::map<std::string, std::vector<NamedColor>, std::less<>> colors_;
};

} // namespace alchemy::gui::car
