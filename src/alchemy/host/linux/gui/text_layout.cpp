// Linux host backend, area "gpu": text layout over Pango (include/alchemy/host/gpu.hpp). Pango
// does what DirectWrite's text layout does on win32: itemizing, bidi (FriBidi), font fallback
// (fontconfig, the same configuration as fonts.cpp), shaping (HarfBuzz) and line breaking. The
// runs of the laid-out lines are read back as DirectWrite's renderer would receive them: glyphs
// in logical order (right-to-left runs reversed), advances, offsets (y up), cluster maps per
// UTF-16 unit, and each span's tag, carried through Pango as a foreground colour.
#include "alchemy/host/linux/gui/fonts.hpp"

#include <pango/pangofc-font.h>
#include <pango/pangofc-fontmap.h>
#include <pango/pangoft2.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <map>

namespace alchemy::host::gpu {
namespace {

std::mutex pango_mutex;

// The font map over fontconfig's configuration, refreshed after a registration.
PangoFontMap* font_map() {
    static PangoFontMap* map{};
    static std::uint64_t serial{};
    auto* config = freetype::config();
    if (!config) return nullptr;
    if (!map) {
        map = pango_ft2_font_map_new();
        if (!map) return nullptr;
        pango_fc_font_map_set_config(PANGO_FC_FONT_MAP(map), config);
        serial = freetype::registration_serial();
    } else if (const auto now = freetype::registration_serial(); now != serial) {
        pango_fc_font_map_config_changed(PANGO_FC_FONT_MAP(map));
        serial = now;
    }
    return map;
}

// UTF-16 text as UTF-8, with the UTF-16 position of every byte (and the end).
struct Utf8 {
    std::string text;
    std::vector<std::uint32_t> unit_of_byte;   // size text.size() + 1
    std::vector<std::uint32_t> byte_of_unit;   // size units + 1
};

Utf8 to_utf8(std::u16string_view units) {
    Utf8 out;
    out.byte_of_unit.assign(units.size() + 1, 0);
    for (std::size_t index = 0; index < units.size();) {
        const auto start = index;
        std::uint32_t c = units[index++];
        if (c >= 0xd800 && c <= 0xdbff && index < units.size() && units[index] >= 0xdc00 && units[index] <= 0xdfff)
            c = 0x10000 + ((c - 0xd800) << 10) + (units[index++] - 0xdc00);
        else if (c >= 0xd800 && c <= 0xdfff) c = 0xfffd;   // a lone surrogate
        if (c == 0) c = 0x20;                               // Pango stops at NUL
        char bytes[4];
        int length;
        if (c < 0x80) { bytes[0] = static_cast<char>(c); length = 1; }
        else if (c < 0x800) { bytes[0] = static_cast<char>(0xc0 | c >> 6); bytes[1] = static_cast<char>(0x80 | (c & 0x3f)); length = 2; }
        else if (c < 0x10000) {
            bytes[0] = static_cast<char>(0xe0 | c >> 12); bytes[1] = static_cast<char>(0x80 | (c >> 6 & 0x3f));
            bytes[2] = static_cast<char>(0x80 | (c & 0x3f)); length = 3;
        } else {
            bytes[0] = static_cast<char>(0xf0 | c >> 18); bytes[1] = static_cast<char>(0x80 | (c >> 12 & 0x3f));
            bytes[2] = static_cast<char>(0x80 | (c >> 6 & 0x3f)); bytes[3] = static_cast<char>(0x80 | (c & 0x3f)); length = 4;
        }
        out.byte_of_unit[start] = static_cast<std::uint32_t>(out.text.size());
        for (int b = 0; b < length; ++b) {
            out.text.push_back(bytes[b]);
            out.unit_of_byte.push_back(static_cast<std::uint32_t>(start));
        }
        // A position inside a surrogate pair rounds up to the character's end.
        for (auto inner = start + 1; inner < index; ++inner) out.byte_of_unit[inner] = static_cast<std::uint32_t>(out.text.size());
    }
    out.unit_of_byte.push_back(static_cast<std::uint32_t>(units.size()));
    out.byte_of_unit[units.size()] = static_cast<std::uint32_t>(out.text.size());
    return out;
}

PangoFontDescription* description_of(const TextSpan& span) {
    auto* description = pango_font_description_new();
    pango_font_description_set_family(description, span.family.c_str());
    pango_font_description_set_weight(description, static_cast<PangoWeight>(std::clamp(span.weight, 100, 1000)));
    pango_font_description_set_style(description, span.style == font_style::italic    ? PANGO_STYLE_ITALIC
                                                  : span.style == font_style::oblique ? PANGO_STYLE_OBLIQUE
                                                                                      : PANGO_STYLE_NORMAL);
    const int stretch = span.stretch >= 1 && span.stretch <= 9 ? span.stretch : font_stretch::normal;
    pango_font_description_set_stretch(description, static_cast<PangoStretch>(stretch - 1));
    // Sizes are in layout units (DIPs), not points.
    pango_font_description_set_absolute_size(description, std::max(0.0, span.size) * PANGO_SCALE);
    return description;
}

int pango_units(double value) { return static_cast<int>(std::lround(std::clamp(value * PANGO_SCALE, double(INT_MIN / 2), double(INT_MAX / 2)))); }

bool newline(char16_t c) { return c == u'\n' || c == u'\r' || c == 0x0b || c == 0x0c || c == 0x85 || c == 0x2028 || c == 0x2029; }

struct Owned {
    PangoContext* context{};
    PangoLayout* layout{};
    PangoLayoutIter* iter{};
    ~Owned() {
        if (iter) pango_layout_iter_free(iter);
        if (layout) g_object_unref(layout);
        if (context) g_object_unref(context);
    }
};

} // namespace

Result<TextLayout> layout_text(const TextLayoutRequest& request) {
    std::lock_guard lock(pango_mutex);
    auto* map = font_map();
    if (!map) return Error::unsupported;
    const auto utf8 = to_utf8(request.text);
    Owned owned;
    owned.context = pango_font_map_create_context(map);
    if (!owned.context) return Error::unsupported;
    pango_context_set_round_glyph_positions(owned.context, FALSE);
    pango_context_set_language(owned.context, pango_language_from_string("en-us"));
    pango_context_set_base_dir(owned.context, request.rtl ? PANGO_DIRECTION_RTL : PANGO_DIRECTION_LTR);
    owned.layout = pango_layout_new(owned.context);
    auto* layout = owned.layout;
    pango_layout_set_auto_dir(layout, FALSE);
    pango_layout_set_text(layout, utf8.text.data(), static_cast<int>(utf8.text.size()));
    auto* base = description_of(request.base);
    pango_layout_set_font_description(layout, base);
    pango_font_description_free(base);
    // Wrapping: DirectWrite's word wrapping breaks inside a word that does not fit, without a hyphen.
    const int width = request.wrapping == Wrapping::none || !(request.width > 0) || request.width * PANGO_SCALE >= INT_MAX / 2
                          ? -1
                          : pango_units(request.width);
    pango_layout_set_width(layout, width);
    pango_layout_set_wrap(layout, request.wrapping == Wrapping::character ? PANGO_WRAP_CHAR : PANGO_WRAP_WORD_CHAR);
    pango_layout_set_justify(layout, request.justified);
    if (request.tab_interval > 0) {
        auto* tabs = pango_tab_array_new_with_positions(1, FALSE, PANGO_TAB_LEFT, pango_units(request.tab_interval));
        pango_layout_set_tabs(layout, tabs);
        pango_tab_array_free(tabs);
    }
    auto* attributes = pango_attr_list_new();
    auto* no_hyphens = pango_attr_insert_hyphens_new(FALSE);
    pango_attr_list_insert(attributes, no_hyphens);
    const auto bytes_of = [&](std::size_t unit) { return utf8.byte_of_unit[std::min(unit, request.text.size())]; };
    for (const auto& span : request.spans) {
        const auto start = bytes_of(span.start);
        const auto end = bytes_of(span.start + std::min(span.length, request.text.size()));
        if (end <= start) continue;
        const auto place = [&](PangoAttribute* attribute) {
            attribute->start_index = start;
            attribute->end_index = end;
            pango_attr_list_insert(attributes, attribute);
        };
        auto* description = description_of(span);
        place(pango_attr_font_desc_new(description));
        pango_font_description_free(description);
        // Pango puts half the spacing before a character and half after; DirectWrite all after (unverified mid-line difference).
        if (span.character_spacing != 0) place(pango_attr_letter_spacing_new(pango_units(span.character_spacing)));
        const auto tag = static_cast<std::uint64_t>(span.tag);
        place(pango_attr_foreground_new(static_cast<guint16>(tag), static_cast<guint16>(tag >> 16), static_cast<guint16>(tag >> 32)));
    }
    pango_layout_set_attributes(layout, attributes);
    pango_attr_list_unref(attributes);

    TextLayout result;
    // Lines: each runs to the next line's start, its paragraph separator included.
    const auto* lines = pango_layout_get_lines_readonly(layout);
    std::vector<std::uint32_t> line_starts;
    for (const auto* item = lines; item; item = item->next)
        line_starts.push_back(utf8.unit_of_byte[std::min<std::size_t>(static_cast<const PangoLayoutLine*>(item->data)->start_index, utf8.text.size())]);
    for (std::size_t index = 0; index < line_starts.size(); ++index) {
        const auto start = line_starts[index];
        const auto end = index + 1 < line_starts.size() ? line_starts[index + 1] : static_cast<std::uint32_t>(request.text.size());
        LaidOutLine line{end > start ? end - start : 0, 0};
        if (line.length >= 2 && request.text[end - 2] == u'\r' && request.text[end - 1] == u'\n') line.newline_length = 2;
        else if (line.length >= 1 && newline(request.text[end - 1])) line.newline_length = 1;
        result.lines.push_back(line);
    }

    // Runs, in drawing order.
    std::map<PangoFont*, Font> fonts;
    owned.iter = pango_layout_get_iter(layout);
    do {
        auto* run = pango_layout_iter_get_run_readonly(owned.iter);
        if (!run || !run->glyphs || run->glyphs->num_glyphs == 0) continue;
        const auto* item = run->item;
        auto* pango_font = item->analysis.font;
        auto& font = fonts[pango_font];
        if (!font && pango_font && PANGO_IS_FC_FONT(pango_font)) {
            FcPattern* pattern = pango_fc_font_get_pattern(PANGO_FC_FONT(pango_font));
            FcChar8* file{};
            int face_index{};
            if (pattern && FcPatternGetString(pattern, FC_FILE, 0, &file) == FcResultMatch) {
                static_cast<void>(FcPatternGetInteger(pattern, FC_INDEX, 0, &face_index));
                font = freetype::font_for_file(reinterpret_cast<const char*>(file), face_index);
            }
        }
        LaidOutRun recorded;
        recorded.font = font;
        if (pango_font) {
            auto* description = pango_font_describe_with_absolute_size(pango_font);
            recorded.size = static_cast<double>(pango_font_description_get_size(description)) / PANGO_SCALE;
            pango_font_description_free(description);
        }
        recorded.rtl = (item->analysis.level & 1) != 0;
        const auto offset = static_cast<std::size_t>(item->offset);
        const auto length = static_cast<std::size_t>(item->length);
        recorded.text_position = utf8.unit_of_byte[std::min(offset, utf8.text.size())];
        recorded.text_length = utf8.unit_of_byte[std::min(offset + length, utf8.text.size())] - recorded.text_position;
        // An empty glyph (a tab, a control) draws as the face's space.
        std::uint16_t space = 0;
        if (font) {
            std::lock_guard face_lock(freetype::freetype_mutex());
            if (auto* face = freetype::face_of(font)) space = static_cast<std::uint16_t>(std::min<FT_UInt>(FT_Get_Char_Index(face, ' '), 0xffff));
        }
        const auto* glyphs = run->glyphs;
        const int count = glyphs->num_glyphs;
        std::vector<std::uint32_t> glyph_units;   // per glyph (logical order): its cluster's UTF-16 position in the run
        for (int visual = 0; visual < count; ++visual) {
            const int index = recorded.rtl ? count - 1 - visual : visual;
            const auto& info = glyphs->glyphs[index];
            const auto glyph = info.glyph;
            recorded.glyphs.push_back(glyph == PANGO_GLYPH_EMPTY                                       ? space
                                      : (glyph & PANGO_GLYPH_UNKNOWN_FLAG) != 0 || glyph > 0xffff ? std::uint16_t{0}
                                                                                                  : static_cast<std::uint16_t>(glyph));
            recorded.advances.push_back(static_cast<double>(info.geometry.width) / PANGO_SCALE);
            const double dx = static_cast<double>(info.geometry.x_offset) / PANGO_SCALE;
            recorded.offsets.push_back({recorded.rtl ? -dx : dx, -static_cast<double>(info.geometry.y_offset) / PANGO_SCALE});
            const auto cluster_byte = std::min(offset + static_cast<std::size_t>(std::max(0, glyphs->log_clusters[index])), utf8.text.size());
            glyph_units.push_back(utf8.unit_of_byte[cluster_byte] - recorded.text_position);
        }
        // Cluster map: each UTF-16 unit of the run -> the first glyph of its cluster.
        std::vector<int> first(recorded.text_length, -1);
        for (std::size_t g = 0; g < glyph_units.size(); ++g)
            if (glyph_units[g] < first.size() && first[glyph_units[g]] < 0) first[glyph_units[g]] = static_cast<int>(g);
        int current = 0;
        for (std::uint32_t unit = 0; unit < recorded.text_length; ++unit) {
            if (first[unit] >= 0) current = first[unit];
            recorded.clusters.push_back(static_cast<std::uint16_t>(current));
        }
        // The pen origin: the run's left edge, or its right edge for a right-to-left run; y the
        // baseline (Pango's: lines are ascent + descent apart, where DirectWrite also adds the
        // face's line gap above each line; CoreText places lines itself and reads only x).
        PangoRectangle logical{};
        pango_layout_iter_get_run_extents(owned.iter, nullptr, &logical);
        recorded.x = static_cast<double>(recorded.rtl ? logical.x + logical.width : logical.x) / PANGO_SCALE;
        recorded.y = static_cast<double>(pango_layout_iter_get_baseline(owned.iter)) / PANGO_SCALE;
        for (const auto* extra = item->analysis.extra_attrs; extra; extra = extra->next) {
            const auto* attribute = static_cast<const PangoAttribute*>(extra->data);
            if (attribute->klass->type != PANGO_ATTR_FOREGROUND) continue;
            const auto& color = reinterpret_cast<const PangoAttrColor*>(attribute)->color;
            recorded.tag = static_cast<std::size_t>(color.red | static_cast<std::uint64_t>(color.green) << 16 | static_cast<std::uint64_t>(color.blue) << 32);
        }
        result.runs.push_back(std::move(recorded));
    } while (pango_layout_iter_next_run(owned.iter));
    return result;
}

} // namespace alchemy::host::gpu
