#include "alchemy/darwin/coretext/ct_internal.hpp"
#include "alchemy/darwin/coregraphics/host_gpu.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <mutex>
#include <tuple>

// Fonts: Apple font names resolved to host fonts (host::gpu; DirectWrite on win32), metrics, glyphs and
// outlines. CTFont objects are NSFont instances (toll-free bridging).
//
// Font mapping table (Apple name -> Windows family, weight). The names the
// guest sees (CTFontCopyPostScriptName / CopyFamilyName) stay the Apple names
// so they round-trip through CTFontCreateWithName:
//
//   .AppleSystemUIFont, .SFNS-*, SF Pro *, System Font, Lucida Grande,
//   Avenir (Next), Futura, Gill Sans, Optima           -> Segoe UI
//   .AppleSystemUIFontMonospaced, SF Mono, Menlo,
//   Monaco, Andale Mono                                 -> Consolas
//   Helvetica, Helvetica Neue, Arial                    -> Arial
//   Courier, Courier New                                -> Courier New
//   Times, Times New Roman                              -> Times New Roman
//   Geneva, Verdana                                     -> Verdana
//   Georgia                                             -> Georgia
//   Apple Color Emoji                                   -> Segoe UI Emoji
//   Apple Symbols, Zapf Dingbats                        -> Segoe UI Symbol
//   PingFang SC/TC/HK, Heiti SC                         -> Microsoft YaHei / JhengHei
//   Hiragino Sans, Hiragino Kaku Gothic ProN            -> Yu Gothic UI
//   Apple SD Gothic Neo                                 -> Malgun Gothic
//
// A PostScript name "Family-Style" maps its family through the table and its
// style suffix (Bold, Italic, Light, Medium, Semibold, Heavy, Black, Thin,
// UltraLight, Oblique, combinations) to weight and slope. Other names resolve
// through DirectWrite (family names, then PostScript and full names of the
// system and registered fonts). An unknown name gives Helvetica (Arial), as
// CTFontCreateWithName gives Helvetica on macOS.
namespace alchemy::gui::quartz2d {
namespace {

struct Alias {
    std::string_view apple;            // exact Apple name (family, PostScript or full name)
    std::string_view windows;          // Windows family
    int weight;                        // host::gpu::font_weight
    int style;                         // host::gpu::font_style
    std::string_view postscript;       // reported names
    std::string_view family;
};
namespace fw = host::gpu::font_weight;
constexpr int W400 = fw::normal, W700 = fw::bold, W500 = fw::medium, W600 = fw::semi_bold, W300 = fw::light,
              W200 = fw::extra_light, W100 = fw::thin, W800 = fw::extra_bold, W900 = fw::black;
constexpr int UP = host::gpu::font_style::normal, IT = host::gpu::font_style::italic;
constexpr Alias aliases[] = {
    {".AppleSystemUIFont", "Segoe UI", W400, UP, ".SFNS-Regular", ".AppleSystemUIFont"},
    {".AppleSystemUIFontBold", "Segoe UI", W700, UP, ".SFNS-Bold", ".AppleSystemUIFont"},
    {".AppleSystemUIFontEmphasized", "Segoe UI", W700, UP, ".SFNS-Bold", ".AppleSystemUIFont"},
    {".AppleSystemUIFontItalic", "Segoe UI", W400, IT, ".SFNS-RegularItalic", ".AppleSystemUIFont"},
    {".SFNS-Regular", "Segoe UI", W400, UP, ".SFNS-Regular", ".AppleSystemUIFont"},
    {".SFNS-Bold", "Segoe UI", W700, UP, ".SFNS-Bold", ".AppleSystemUIFont"},
    {".SFNS-Medium", "Segoe UI", W500, UP, ".SFNS-Medium", ".AppleSystemUIFont"},
    {".SFNS-Semibold", "Segoe UI", W600, UP, ".SFNS-Semibold", ".AppleSystemUIFont"},
    {".SFNS-Light", "Segoe UI", W300, UP, ".SFNS-Light", ".AppleSystemUIFont"},
    {".SFNS-Thin", "Segoe UI", W100, UP, ".SFNS-Thin", ".AppleSystemUIFont"},
    {".SFNS-Heavy", "Segoe UI", W800, UP, ".SFNS-Heavy", ".AppleSystemUIFont"},
    {".SFNS-Black", "Segoe UI", W900, UP, ".SFNS-Black", ".AppleSystemUIFont"},
    {".SFNS-RegularItalic", "Segoe UI", W400, IT, ".SFNS-RegularItalic", ".AppleSystemUIFont"},
    {".SFNSText", "Segoe UI", W400, UP, ".SFNSText", ".SF NS Text"},
    {".SFNSText-Regular", "Segoe UI", W400, UP, ".SFNSText", ".SF NS Text"},
    {".SFNSText-Bold", "Segoe UI", W700, UP, ".SFNSText-Bold", ".SF NS Text"},
    {".SFNSDisplay", "Segoe UI", W400, UP, ".SFNSDisplay", ".SF NS Display"},
    {".SFNSDisplay-Regular", "Segoe UI", W400, UP, ".SFNSDisplay", ".SF NS Display"},
    {".SFNSDisplay-Bold", "Segoe UI", W700, UP, ".SFNSDisplay-Bold", ".SF NS Display"},
    {".SF NS", "Segoe UI", W400, UP, ".SFNS-Regular", ".SF NS"},
    {".SF NS Text", "Segoe UI", W400, UP, ".SFNSText", ".SF NS Text"},
    {".SF NS Display", "Segoe UI", W400, UP, ".SFNSDisplay", ".SF NS Display"},
    {"System Font", "Segoe UI", W400, UP, ".SFNS-Regular", ".AppleSystemUIFont"},
    {"SF Pro", "Segoe UI", W400, UP, "SFPro-Regular", "SF Pro"},
    {"SF Pro Text", "Segoe UI", W400, UP, "SFProText-Regular", "SF Pro Text"},
    {"SF Pro Display", "Segoe UI", W400, UP, "SFProDisplay-Regular", "SF Pro Display"},
    {"SFProText-Regular", "Segoe UI", W400, UP, "SFProText-Regular", "SF Pro Text"},
    {"SFProText-Bold", "Segoe UI", W700, UP, "SFProText-Bold", "SF Pro Text"},
    {"SFProDisplay-Regular", "Segoe UI", W400, UP, "SFProDisplay-Regular", "SF Pro Display"},
    {"Lucida Grande", "Segoe UI", W400, UP, "LucidaGrande", "Lucida Grande"},
    {"LucidaGrande", "Segoe UI", W400, UP, "LucidaGrande", "Lucida Grande"},
    {"LucidaGrande-Bold", "Segoe UI", W700, UP, "LucidaGrande-Bold", "Lucida Grande"},
    {"Avenir", "Segoe UI", W400, UP, "Avenir-Book", "Avenir"},
    {"Avenir Next", "Segoe UI", W400, UP, "AvenirNext-Regular", "Avenir Next"},
    {"Futura", "Segoe UI", W500, UP, "Futura-Medium", "Futura"},
    {"Gill Sans", "Segoe UI", W400, UP, "GillSans", "Gill Sans"},
    {"Optima", "Segoe UI", W400, UP, "Optima-Regular", "Optima"},
    {".AppleSystemUIFontMonospaced", "Consolas", W400, UP, ".SFNSMono-Regular", ".AppleSystemUIFontMonospaced"},
    {".SFNSMono-Regular", "Consolas", W400, UP, ".SFNSMono-Regular", ".AppleSystemUIFontMonospaced"},
    {"SF Mono", "Consolas", W400, UP, "SFMono-Regular", "SF Mono"},
    {"SFMono-Regular", "Consolas", W400, UP, "SFMono-Regular", "SF Mono"},
    {"Menlo", "Consolas", W400, UP, "Menlo-Regular", "Menlo"},
    {"Menlo-Regular", "Consolas", W400, UP, "Menlo-Regular", "Menlo"},
    {"Monaco", "Consolas", W400, UP, "Monaco", "Monaco"},
    {"Andale Mono", "Consolas", W400, UP, "AndaleMono", "Andale Mono"},
    {"Helvetica", "Arial", W400, UP, "Helvetica", "Helvetica"},
    {"Helvetica Neue", "Arial", W400, UP, "HelveticaNeue", "Helvetica Neue"},
    {"HelveticaNeue", "Arial", W400, UP, "HelveticaNeue", "Helvetica Neue"},
    {"ArialMT", "Arial", W400, UP, "ArialMT", "Arial"},
    {"Courier", "Courier New", W400, UP, "Courier", "Courier"},
    {"CourierNewPSMT", "Courier New", W400, UP, "CourierNewPSMT", "Courier New"},
    {"Times", "Times New Roman", W400, UP, "Times-Roman", "Times"},
    {"Times-Roman", "Times New Roman", W400, UP, "Times-Roman", "Times"},
    {"TimesNewRomanPSMT", "Times New Roman", W400, UP, "TimesNewRomanPSMT", "Times New Roman"},
    {"Geneva", "Verdana", W400, UP, "Geneva", "Geneva"},
    {"Apple Color Emoji", "Segoe UI Emoji", W400, UP, "AppleColorEmoji", "Apple Color Emoji"},
    {"AppleColorEmoji", "Segoe UI Emoji", W400, UP, "AppleColorEmoji", "Apple Color Emoji"},
    {"Apple Symbols", "Segoe UI Symbol", W400, UP, "AppleSymbols", "Apple Symbols"},
    {"Zapf Dingbats", "Segoe UI Symbol", W400, UP, "ZapfDingbatsITC", "Zapf Dingbats"},
    {"PingFang SC", "Microsoft YaHei", W400, UP, "PingFangSC-Regular", "PingFang SC"},
    {"PingFang TC", "Microsoft JhengHei", W400, UP, "PingFangTC-Regular", "PingFang TC"},
    {"PingFang HK", "Microsoft JhengHei", W400, UP, "PingFangHK-Regular", "PingFang HK"},
    {"Heiti SC", "Microsoft YaHei", W400, UP, "STHeitiSC-Light", "Heiti SC"},
    {"Hiragino Sans", "Yu Gothic UI", W400, UP, "HiraginoSans-W3", "Hiragino Sans"},
    {"Hiragino Kaku Gothic ProN", "Yu Gothic UI", W400, UP, "HiraKakuProN-W3", "Hiragino Kaku Gothic ProN"},
    {"Apple SD Gothic Neo", "Malgun Gothic", W400, UP, "AppleSDGothicNeo-Regular", "Apple SD Gothic Neo"},
};
const Alias* find_alias(std::string_view name) {
    for (const auto& alias : aliases) if (alias.apple == name) return &alias;
    return nullptr;
}

struct StyleSuffix { std::string_view suffix; int weight; int style; };
constexpr StyleSuffix suffixes[] = {
    {"Regular", W400, UP}, {"Roman", W400, UP}, {"Book", W400, UP}, {"Plain", W400, UP}, {"Normal", W400, UP},
    {"Italic", W400, IT}, {"Oblique", W400, IT}, {"Bold", W700, UP}, {"BoldItalic", W700, IT}, {"BoldOblique", W700, IT},
    {"Medium", W500, UP}, {"MediumItalic", W500, IT}, {"Semibold", W600, UP}, {"SemiBold", W600, UP}, {"SemiboldItalic", W600, IT},
    {"Demi", W600, UP}, {"DemiBold", W600, UP}, {"Light", W300, UP}, {"LightItalic", W300, IT}, {"UltraLight", W200, UP},
    {"ExtraLight", W200, UP}, {"Thin", W100, UP}, {"Heavy", W800, UP}, {"ExtraBold", W800, UP}, {"Black", W900, UP},
    {"W3", W400, UP}, {"W6", W600, UP},
};
const StyleSuffix* find_suffix(std::string_view text) {
    for (const auto& entry : suffixes) if (entry.suffix == text) return &entry;
    return nullptr;
}

using FaceKey = std::tuple<std::string, int, int, int, const host::gpu::FontCollection::Impl*>;
std::mutex registry_mutex;
std::map<FaceKey, std::shared_ptr<FontFace>> face_cache;

host::gpu::FontCollection system_collection() {
    auto fonts = host::gpu::system_fonts();
    if (!fonts) fail("reading the system fonts failed (" + std::string(host::name(fonts.error())) + ")");
    return *fonts;
}

std::shared_ptr<FontFace> build_face(host::gpu::Font font, host::gpu::FontCollection collection) {
    const auto info = font.info();
    if (!info) fail("reading a font face failed (" + std::string(host::name(info.error())) + ")");
    auto result = std::make_shared<FontFace>();
    result->font = std::move(font);
    result->collection = std::move(collection);
    result->family = info->family;
    result->style_name = info->style_name;
    result->postscript = info->postscript;
    result->full_name = info->full_name;
    if (result->full_name.empty()) result->full_name = result->family + (result->style_name == "Regular" ? "" : " " + result->style_name);
    if (result->postscript.empty()) {
        for (const auto c : result->family) if (c != ' ') result->postscript += c;
        if (!result->style_name.empty() && result->style_name != "Regular") result->postscript += "-" + result->style_name;
    }
    result->weight = info->weight;
    result->style = info->style;
    result->stretch = info->stretch;
    const auto& m = info->metrics;
    result->metrics.units_per_em = m.units_per_em;
    result->metrics.ascent = m.ascent;
    result->metrics.descent = m.descent;
    result->metrics.leading = m.leading;
    result->metrics.cap_height = m.cap_height;
    result->metrics.x_height = m.x_height;
    result->metrics.underline_position = m.underline_position;
    result->metrics.underline_thickness = m.underline_thickness;
    result->metrics.bounding_box = {{m.box_left, m.box_bottom}, {m.box_width, m.box_height}};
    result->metrics.slant = m.slant;
    result->monospace = info->monospace;
    result->symbol = info->symbol;
    result->color = info->color;
    return result;
}

std::shared_ptr<FontFace> family_face(const host::gpu::FontCollection& collection, std::string_view family, int weight, int style,
                                      int stretch = host::gpu::font_stretch::normal) {
    if (!collection) return nullptr;
    const FaceKey key{std::string(family), weight, style, stretch, collection.impl()};
    {
        std::lock_guard lock(registry_mutex);
        if (const auto found = face_cache.find(key); found != face_cache.end()) return found->second;
    }
    auto font = host::gpu::family_font(collection, family, weight, style, stretch);
    if (!font) return nullptr;
    auto result = build_face(std::move(*font), collection);
    std::lock_guard lock(registry_mutex);
    face_cache.emplace(key, result);
    return result;
}

std::shared_ptr<FontFace> set_face(const host::gpu::FontCollection& collection, host::gpu::FontName kind, std::string_view name) {
    if (!collection) return nullptr;
    auto font = host::gpu::named_font(collection, kind, name);
    if (!font) return nullptr;
    return build_face(std::move(*font), collection);
}

// Host lookup of a name: registered fonts first, then the system.
std::shared_ptr<FontFace> lookup(std::string_view name, int weight = W400, int style = UP) {
    if (const auto registered = host::gpu::registered_fonts()) {
        if (auto face = family_face(registered, name, weight, style)) return face;
        if (auto face = set_face(registered, host::gpu::FontName::postscript, name)) return face;
        if (auto face = set_face(registered, host::gpu::FontName::full, name)) return face;
    }
    const auto system = system_collection();
    if (auto face = family_face(system, name, weight, style)) return face;
    if (auto face = set_face(system, host::gpu::FontName::postscript, name)) return face;
    if (auto face = set_face(system, host::gpu::FontName::full, name)) return face;
    return nullptr;
}

ResolvedFont from_face(std::shared_ptr<FontFace> face) {
    ResolvedFont result;
    result.postscript = face->postscript;
    result.family = face->family;
    result.display = face->full_name;
    result.face = std::move(face);
    return result;
}

} // namespace

std::uint32_t FontState::traits() const {
    std::uint32_t value{};
    if (face->style != host::gpu::font_style::normal) value |= font_trait::italic;
    if (face->weight >= host::gpu::font_weight::semi_bold) value |= font_trait::bold;
    if (face->stretch > host::gpu::font_stretch::normal) value |= font_trait::expanded;
    if (face->stretch < host::gpu::font_stretch::normal && face->stretch != host::gpu::font_stretch::undefined) value |= font_trait::condensed;
    if (face->monospace) value |= font_trait::monospace;
    if (face->color) value |= font_trait::color_glyphs;
    if (!postscript.empty() && postscript.front() == '.') value |= font_trait::ui_optimized;
    return value;
}

std::optional<ResolvedFont> resolve_font(std::string_view name) {
    if (name.empty()) return std::nullopt;
    const auto mapped = [](const Alias& alias, int weight, int style, std::string postscript) -> std::optional<ResolvedFont> {
        auto face = lookup(alias.windows, weight, style);
        if (!face) return std::nullopt;
        ResolvedFont result;
        result.face = std::move(face);
        result.postscript = std::move(postscript);
        result.family = alias.family;
        result.display = std::string(alias.family) + (weight >= W600 ? " Bold" : "") + (style != UP ? " Italic" : "");
        return result;
    };
    if (const auto* alias = find_alias(name)) return mapped(*alias, alias->weight, alias->style, std::string(alias->postscript));
    // "Family-Style" PostScript names.
    if (const auto dash = name.rfind('-'); dash != std::string_view::npos && dash > 0) {
        auto family = name.substr(0, dash);
        auto style_text = std::string(name.substr(dash + 1));
        for (const std::string_view tail : {"PSMT", "MT"})
            if (style_text.size() > tail.size() && style_text.ends_with(tail)) style_text.resize(style_text.size() - tail.size());
        if (const auto* suffix = find_suffix(style_text)) {
            if (const auto* alias = find_alias(family)) return mapped(*alias, suffix->weight, suffix->style, std::string(name));
            std::string trimmed(family);
            for (const std::string_view tail : {"PSMT", "MT"})
                if (trimmed.size() > tail.size() && trimmed.ends_with(tail)) trimmed.resize(trimmed.size() - tail.size());
            if (auto face = lookup(trimmed, suffix->weight, suffix->style)) return from_face(std::move(face));
        }
    }
    if (auto face = lookup(name)) return from_face(std::move(face));
    // "ArialMT"-style names without a style part.
    for (const std::string_view tail : {"PSMT", "MT"})
        if (name.size() > tail.size() && name.ends_with(tail))
            if (auto face = lookup(name.substr(0, name.size() - tail.size()))) return from_face(std::move(face));
    return std::nullopt;
}

std::optional<std::pair<ResolvedFont, double>> ui_font(std::int64_t type) {
    // CTFontUIFontType -> (Apple name, default size).
    struct Entry { std::string_view name; double size; };
    static constexpr Entry entries[] = {
        {"Helvetica", 12},                          // User
        {".AppleSystemUIFontMonospaced", 10},       // UserFixedPitch
        {".AppleSystemUIFont", 13},                 // System
        {".AppleSystemUIFontBold", 13},             // EmphasizedSystem
        {".AppleSystemUIFont", 11},                 // SmallSystem
        {".AppleSystemUIFontBold", 11},             // SmallEmphasizedSystem
        {".AppleSystemUIFont", 9},                  // MiniSystem
        {".AppleSystemUIFontBold", 9},              // MiniEmphasizedSystem
        {".AppleSystemUIFont", 12},                 // Views
        {".AppleSystemUIFont", 13},                 // Application
        {".AppleSystemUIFont", 10},                 // Label
        {".AppleSystemUIFont", 14},                 // MenuTitle
        {".AppleSystemUIFont", 14},                 // MenuItem
        {".AppleSystemUIFont", 14},                 // MenuItemMark
        {".AppleSystemUIFont", 14},                 // MenuItemCmdKey
        {".AppleSystemUIFont", 13},                 // WindowTitle
        {".AppleSystemUIFont", 13},                 // PushButton
        {".AppleSystemUIFont", 11},                 // UtilityWindowTitle
        {".AppleSystemUIFontBold", 13},             // AlertHeader
        {".AppleSystemUIFont", 12},                 // SystemDetail
        {".AppleSystemUIFontBold", 12},             // EmphasizedSystemDetail
        {".AppleSystemUIFont", 11},                 // Toolbar
        {".AppleSystemUIFont", 10},                 // SmallToolbar
        {".AppleSystemUIFont", 13},                 // Message
        {".AppleSystemUIFont", 11},                 // Palette
        {".AppleSystemUIFont", 11},                 // ToolTip
        {".AppleSystemUIFont", 13},                 // ControlContent
    };
    if (type < 0 || type >= static_cast<std::int64_t>(std::size(entries))) return std::nullopt;
    const auto& entry = entries[type];
    auto resolved = resolve_font(entry.name);
    if (!resolved) return std::nullopt;
    return std::pair{std::move(*resolved), entry.size};
}

std::shared_ptr<FontFace> face_from(const host::gpu::Font& font, const host::gpu::FontCollection& collection) {
    if (!font) return nullptr;
    // Reuse the cached face of the same family / weight / style when the font
    // is a system font (keeps one FontFace per font).
    auto built = build_face(font, collection ? collection : system_collection());
    if (auto cached = family_face(built->collection, built->family, built->weight, built->style, built->stretch))
        if (cached->font.same_face(font)) return cached;
    return built;
}

std::shared_ptr<FontFace> family_match(const FontFace& face, int weight) {
    auto font = host::gpu::family_font(face.collection, face.family, weight, face.style, face.stretch);
    return font ? face_from(*font, face.collection) : nullptr;
}

std::optional<ResolvedFont> with_traits(const FontState& font, std::uint32_t traits, std::uint32_t mask) {
    auto weight = font.face->weight;
    auto style = font.face->style;
    if (mask & font_trait::bold) weight = (traits & font_trait::bold) ? W700 : W400;
    if (mask & font_trait::italic) style = (traits & font_trait::italic) ? IT : UP;
    if (mask & font_trait::monospace && (traits & font_trait::monospace) && !font.face->monospace) return std::nullopt;
    auto face = family_face(font.face->collection, font.face->family, weight, style, font.face->stretch);
    if (!face) return std::nullopt;
    // A request the family cannot satisfy fails (CoreText returns NULL).
    if ((mask & font_trait::bold) && (traits & font_trait::bold) && face->weight < host::gpu::font_weight::semi_bold) return std::nullopt;
    if ((mask & font_trait::italic) && (traits & font_trait::italic) && face->style == UP) return std::nullopt;
    ResolvedFont result;
    result.face = face;
    // Mapped Apple families keep their Apple names with the new style.
    if (!font.family.empty() && font.family != font.face->family) {
        result.family = font.family;
        auto base = font.postscript.substr(0, font.postscript.find('-'));
        std::string style_name = face->weight >= W600 ? (face->style != UP ? "BoldItalic" : "Bold") : (face->style != UP ? "Italic" : "Regular");
        result.postscript = base + "-" + style_name;
        result.display = font.family + (style_name == "Regular" ? "" : " " + style_name);
        return result;
    }
    return from_face(face);
}

std::optional<ResolvedFont> with_family(const FontState& font, std::string_view family) {
    auto resolved = resolve_font(family);
    if (!resolved) return std::nullopt;
    if (auto face = family_face(resolved->face->collection, resolved->face->family, font.face->weight, font.face->style, font.face->stretch))
        resolved->face = face;
    return resolved;
}

std::vector<std::uint16_t> glyphs_for(const FontFace& face, std::span<const std::uint16_t> units) {
    std::vector<std::uint16_t> glyphs(units.size(), 0);
    std::vector<std::uint32_t> points;
    std::vector<std::size_t> positions;
    for (std::size_t index = 0; index < units.size(); ++index) {
        std::uint32_t point = units[index];
        if (point >= 0xd800 && point <= 0xdbff && index + 1 < units.size() && units[index + 1] >= 0xdc00 && units[index + 1] <= 0xdfff) {
            point = 0x10000 + ((point - 0xd800) << 10) + (units[index + 1] - 0xdc00);
            points.push_back(point);
            positions.push_back(index);
            ++index;   // the second unit keeps glyph 0, as in CoreText
            continue;
        }
        points.push_back(point);
        positions.push_back(index);
    }
    if (points.empty()) return glyphs;
    const auto indices = face.font.glyphs(points);
    if (!indices) fail("mapping characters to glyphs failed (" + std::string(host::name(indices.error())) + ")");
    for (std::size_t index = 0; index < points.size(); ++index) glyphs[positions[index]] = (*indices)[index];
    return glyphs;
}

namespace {
std::vector<host::gpu::GlyphMetrics> design_metrics(const FontFace& face, std::span<const std::uint16_t> glyphs) {
    if (glyphs.empty()) return {};
    auto metrics = face.font.glyph_metrics(glyphs);
    if (!metrics) fail("reading glyph metrics failed (" + std::string(host::name(metrics.error())) + ")");
    return std::move(*metrics);
}
} // namespace

std::vector<double> advances_for(const FontFace& face, double size, std::span<const std::uint16_t> glyphs) {
    const auto metrics = design_metrics(face, glyphs);
    std::vector<double> result(glyphs.size());
    const auto scale = size / std::max(1.0, face.metrics.units_per_em);
    for (std::size_t index = 0; index < glyphs.size(); ++index) result[index] = metrics[index].advance_width * scale;
    return result;
}

std::vector<Rect> glyph_bounds(const FontFace& face, double size, std::span<const std::uint16_t> glyphs) {
    const auto metrics = design_metrics(face, glyphs);
    std::vector<Rect> result(glyphs.size());
    const auto scale = size / std::max(1.0, face.metrics.units_per_em);
    for (std::size_t index = 0; index < glyphs.size(); ++index) {
        const auto& m = metrics[index];
        const double left = m.left_bearing;
        const double width = static_cast<double>(m.advance_width) - m.left_bearing - m.right_bearing;
        const double top = static_cast<double>(m.vertical_origin_y) - m.top_bearing;
        const double bottom = static_cast<double>(m.vertical_origin_y) - static_cast<double>(m.advance_height) + m.bottom_bearing;
        if (width <= 0 || top <= bottom) { result[index] = {{0, 0}, {0, 0}}; continue; }
        result[index] = {{left * scale, bottom * scale}, {width * scale, (top - bottom) * scale}};
    }
    return result;
}

FontMetrics font_metrics(const FontFace& face) { return face.metrics; }

PathData glyph_outlines(const GlyphRunDraw& run) {
    PathData path;
    if (run.glyphs.empty() || !run.face) return path;
    const auto read = [&](std::span<const std::uint16_t> glyphs, std::span<const host::gpu::Point> offsets, const Affine& transform) {
        const auto outlines = run.face->font.outlines(run.size, glyphs, offsets, to_host(transform));
        if (!outlines) fail("reading glyph outlines failed (" + std::string(host::name(outlines.error())) + ")");
        append(path, *outlines);
    };
    if (run.matrix.is_identity()) {
        std::vector<host::gpu::Point> offsets(run.glyphs.size());
        for (std::size_t index = 0; index < offsets.size(); ++index)
            if (index < run.positions.size()) offsets[index] = to_host(run.positions[index]);
        read(run.glyphs, offsets, Affine::scaling(1, -1));
        return path;
    }
    for (std::size_t index = 0; index < run.glyphs.size(); ++index) {
        const auto p = index < run.positions.size() ? run.positions[index] : Point{};
        const host::gpu::Point origin{};
        read(std::span(&run.glyphs[index], 1), std::span(&origin, 1),
             Affine::scaling(1, -1).then(run.matrix).then(Affine::translation(p.x, p.y)));
    }
    return path;
}

bool register_font_file(std::span<const std::uint8_t> bytes, std::string* error) {
    const auto registered = host::gpu::register_font_file(bytes);
    if (!registered) {
        if (error) {
            switch (registered.error()) {
            case host::Error::invalid_argument: *error = "empty font data"; break;
            case host::Error::io: *error = "the font data could not be loaded"; break;
            case host::Error::unsupported: *error = "unsupported font file format"; break;
            default: *error = "the font file could not be added"; break;
            }
        }
        return false;
    }
    std::lock_guard lock(registry_mutex);
    face_cache.clear();
    return true;
}

std::vector<std::shared_ptr<FontFace>> family_members(std::string_view family) {
    std::vector<std::shared_ptr<FontFace>> members;
    // An Apple family name maps to its stand-in family first.
    std::string name(family);
    if (const auto resolved = resolve_font(family); resolved && resolved->face) name = resolved->face->family;
    const auto from = [&](const host::gpu::FontCollection& collection) {
        if (!collection || !members.empty()) return;
        const auto faces = host::gpu::family_faces(collection, name);
        if (!faces) return;
        for (const auto& font : *faces) members.push_back(build_face(font, collection));
    };
    from(host::gpu::registered_fonts());
    from(system_collection());
    return members;
}

std::vector<std::string> available_families() {
    std::vector<std::string> names;
    const auto add = [&](const host::gpu::FontCollection& collection) {
        if (!collection) return;
        if (const auto listed = host::gpu::family_names(collection)) names.insert(names.end(), listed->begin(), listed->end());
    };
    add(host::gpu::registered_fonts());
    add(system_collection());
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

} // namespace alchemy::gui::quartz2d
