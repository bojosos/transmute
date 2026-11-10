// Linux host backend, area "gpu": fonts over FreeType and fontconfig (include/alchemy/host/gpu.hpp).
// The system fonts are fontconfig's system font set and the registered fonts its application font
// set, both in one configuration that text_layout.cpp's Pango font map also uses. Faces are
// FreeType faces: names, metrics, glyphs, glyph metrics, outlines and OpenType tables come from
// the font files themselves, in design units as DirectWrite reports them.
#include "alchemy/host/linux/gui/fonts.hpp"

#include <fontconfig/fcfreetype.h>
#include FT_OUTLINE_H
#include FT_TRUETYPE_TABLES_H
#include FT_TRUETYPE_TAGS_H

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

namespace alchemy::host::gpu {

Font::Impl::~Impl() {
    if (!face) return;
    std::lock_guard lock(freetype::freetype_mutex());
    FT_Done_Face(face);
}

struct FontCollection::Impl {
    FcSetName set{FcSetSystem};
};

namespace freetype {
namespace {

std::recursive_mutex mutex_;
FT_Library library_{};
FcConfig* config_{};
std::uint64_t serial_{};
std::map<std::pair<std::string, int>, std::weak_ptr<Font::Impl>> faces_;
std::vector<int> registered_files_;   // memfds kept open: fontconfig and Pango open their /proc paths
FontCollection registered_;

void initialize() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (FT_Init_FreeType(&library_) != 0) library_ = nullptr;
        if (FcInit()) config_ = FcInitLoadConfigAndFonts();
    });
}

} // namespace

std::recursive_mutex& freetype_mutex() { return mutex_; }
FT_Library library() { initialize(); return library_; }
FcConfig* config() { initialize(); return config_; }
std::uint64_t registration_serial() {
    std::lock_guard lock(mutex_);
    return serial_;
}

Font font_for_file(const std::string& path, int index) {
    std::lock_guard lock(mutex_);
    auto& slot = faces_[{path, index}];
    if (auto existing = slot.lock()) return Font(std::move(existing));
    auto impl = std::make_shared<Font::Impl>();
    impl->path = path;
    impl->index = index;
    slot = impl;
    return Font(std::move(impl));
}

FT_Face face_of(const Font& font) {
    auto* impl = font.impl();
    if (!impl) return nullptr;
    if (impl->face) return impl->face;
    auto* lib = library();
    if (!lib) return nullptr;
    FT_Face face{};
    const auto error = impl->bytes ? FT_New_Memory_Face(lib, impl->bytes->data(), static_cast<FT_Long>(impl->bytes->size()), impl->index, &face)
                                   : FT_New_Face(lib, impl->path.c_str(), impl->index, &face);
    if (error != 0) return nullptr;
    // The Unicode charmap where the face has one (symbol fonts keep theirs).
    if (FT_Select_Charmap(face, FT_ENCODING_UNICODE) != 0) static_cast<void>(FT_Select_Charmap(face, FT_ENCODING_MS_SYMBOL));
    impl->face = face;
    return face;
}

} // namespace freetype

namespace {

using freetype::face_of;
using Lock = std::lock_guard<std::recursive_mutex>;

// ---- fontconfig patterns ----

std::string pattern_string(FcPattern* pattern, const char* object, const char* lang_object) {
    FcChar8* value{};
    if (lang_object) {
        FcChar8* lang{};
        for (int index = 0; FcPatternGetString(pattern, object, index, &value) == FcResultMatch; ++index)
            if (FcPatternGetString(pattern, lang_object, index, &lang) == FcResultMatch && std::strcmp(reinterpret_cast<char*>(lang), "en") == 0)
                return reinterpret_cast<char*>(value);
    }
    if (FcPatternGetString(pattern, object, 0, &value) == FcResultMatch) return reinterpret_cast<char*>(value);
    return {};
}

// OpenType scales of fontconfig's weight, slant and width.
int opentype_weight(double fc_weight) {
    return std::clamp(static_cast<int>(std::lround(FcWeightToOpenTypeDouble(fc_weight))), 1, 999);
}
int opentype_style(int slant) {
    return slant >= FC_SLANT_OBLIQUE ? font_style::oblique : slant >= FC_SLANT_ITALIC ? font_style::italic : font_style::normal;
}
int opentype_stretch(double width) {
    static constexpr double widths[] = {50, 62.5, 75, 87.5, 100, 112.5, 125, 150, 200};   // OS/2 usWidthClass 1..9
    int best = 5;
    for (int index = 0; index < 9; ++index)
        if (std::abs(widths[index] - width) < std::abs(widths[best - 1] - width)) best = index + 1;
    return best;
}
double fc_slant(int style) { return style == font_style::italic ? FC_SLANT_ITALIC : style == font_style::oblique ? FC_SLANT_OBLIQUE : FC_SLANT_ROMAN; }
double fc_width(int stretch) {
    static constexpr double widths[] = {50, 62.5, 75, 87.5, 100, 112.5, 125, 150, 200};
    return stretch >= 1 && stretch <= 9 ? widths[stretch - 1] : 100;
}

struct Candidate {
    std::string file;
    int index{};
    int weight{font_weight::normal}, style{font_style::normal}, stretch{font_stretch::normal};
};

Candidate candidate_of(FcPattern* pattern) {
    Candidate c;
    FcChar8* file{};
    if (FcPatternGetString(pattern, FC_FILE, 0, &file) == FcResultMatch) c.file = reinterpret_cast<char*>(file);
    static_cast<void>(FcPatternGetInteger(pattern, FC_INDEX, 0, &c.index));
    double weight{FC_WEIGHT_REGULAR}, width{FC_WIDTH_NORMAL};
    int slant{FC_SLANT_ROMAN};
    static_cast<void>(FcPatternGetDouble(pattern, FC_WEIGHT, 0, &weight));
    static_cast<void>(FcPatternGetInteger(pattern, FC_SLANT, 0, &slant));
    static_cast<void>(FcPatternGetDouble(pattern, FC_WIDTH, 0, &width));
    c.weight = opentype_weight(weight);
    c.style = opentype_style(slant);
    c.stretch = opentype_stretch(width);
    return c;
}

FcFontSet* font_set(const FontCollection& collection) {
    auto* config = freetype::config();
    return collection && config ? FcConfigGetFonts(config, collection.impl()->set) : nullptr;
}

// A whole variable font (not one of its named instances) is listed besides its instances.
bool variable_default(FcPattern* pattern) {
    FcBool variable{};
    return FcPatternGetBool(pattern, FC_VARIABLE, 0, &variable) == FcResultMatch && variable;
}

// Family names compare as fontconfig's do: ignoring case and blanks.
bool same_name(const FcChar8* a, std::string_view b) {
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    std::size_t j = 0;
    for (const auto* s = reinterpret_cast<const char*>(a); *s; ++s) {
        if (*s == ' ') continue;
        while (j < b.size() && b[j] == ' ') ++j;
        if (j == b.size() || lower(*s) != lower(b[j])) return false;
        ++j;
    }
    while (j < b.size() && b[j] == ' ') ++j;
    return j == b.size();
}

std::vector<Candidate> family_members(FcFontSet* set, std::string_view family) {
    std::vector<Candidate> members;
    if (!set) return members;
    for (int font = 0; font < set->nfont; ++font) {
        auto* pattern = set->fonts[font];
        if (variable_default(pattern)) continue;
        FcChar8* name{};
        for (int index = 0; FcPatternGetString(pattern, FC_FAMILY, index, &name) == FcResultMatch; ++index)
            if (same_name(name, family)) {
                auto c = candidate_of(pattern);
                if (!c.file.empty() &&
                    std::none_of(members.begin(), members.end(), [&](const Candidate& m) { return m.file == c.file && m.index == c.index; }))
                    members.push_back(std::move(c));
                break;
            }
    }
    return members;
}

// The CSS font-matching order (stretch, then style, then weight), which
// DirectWrite's GetFirstMatchingFont follows closely; it never synthesizes a
// bold or oblique face (DirectWrite may: unverified which it picks then).
std::tuple<int, int, int> match_key(const Candidate& c, int weight, int style, int stretch) {
    if (stretch == font_stretch::undefined) stretch = font_stretch::normal;
    const int stretch_key = c.stretch == stretch ? 0
                          : stretch <= font_stretch::normal ? (c.stretch < stretch ? stretch - c.stretch : 10 + c.stretch - stretch)
                                                            : (c.stretch > stretch ? c.stretch - stretch : 10 + stretch - c.stretch);
    static constexpr int style_order[3][3] = {{0, 1, 2}, {1, 0, 2}, {2, 1, 0}};   // [wanted][have]: normal, oblique, italic
    const int style_key = style_order[std::clamp(style, 0, 2)][std::clamp(c.style, 0, 2)];
    int weight_key;
    const int w = c.weight;
    if (w == weight) weight_key = 0;
    else if (weight >= 400 && weight <= 500) {
        if (w > weight && w <= 500) weight_key = w - weight;                   // up to 500 first
        else if (w < weight) weight_key = 1000 + (weight - w);                 // then lighter
        else weight_key = 2000 + (w - weight);                                 // then heavier
    } else if (weight < 400) weight_key = w < weight ? weight - w : 1000 + (w - weight);
    else weight_key = w > weight ? w - weight : 1000 + (weight - w);
    return {stretch_key, style_key, weight_key};
}

// ---- FreeType faces ----

const TT_OS2* os2_of(FT_Face face) {
    auto* os2 = static_cast<const TT_OS2*>(FT_Get_Sfnt_Table(face, FT_SFNT_OS2));
    return os2 && os2->version != 0xffff ? os2 : nullptr;
}

double glyph_top(FT_Face face, FT_ULong code) {
    const auto glyph = FT_Get_Char_Index(face, code);
    if (glyph == 0 || FT_Load_Glyph(face, glyph, FT_LOAD_NO_SCALE | FT_LOAD_NO_BITMAP) != 0) return 0;
    return static_cast<double>(face->glyph->metrics.horiBearingY);
}

bool symbol_cmap(FT_Face face) {
    for (int index = 0; index < face->num_charmaps; ++index)
        if (face->charmaps[index]->encoding == FT_ENCODING_MS_SYMBOL) return true;
    return false;
}

// Vertical metrics as Windows reads them: OS/2 typographic values when the
// face asks for them (USE_TYPO_METRICS), else usWin* with the line gap that
// completes hhea's line height.
FontMetrics design_metrics(FT_Face face) {
    FontMetrics m;
    m.units_per_em = face->units_per_EM ? face->units_per_EM : 1000;
    const auto* os2 = os2_of(face);
    const auto* hhea = static_cast<const TT_HoriHeader*>(FT_Get_Sfnt_Table(face, FT_SFNT_HHEA));
    const auto* post = static_cast<const TT_Postscript*>(FT_Get_Sfnt_Table(face, FT_SFNT_POST));
    if (os2 && (os2->fsSelection & (1U << 7)) != 0) {
        m.ascent = os2->sTypoAscender;
        m.descent = -os2->sTypoDescender;
        m.leading = os2->sTypoLineGap;
    } else if (os2) {
        m.ascent = os2->usWinAscent;
        m.descent = os2->usWinDescent;
        if (hhea) m.leading = std::max(0.0, static_cast<double>(hhea->Ascender - hhea->Descender + hhea->Line_Gap) - (m.ascent + m.descent));
    } else {
        m.ascent = face->ascender;
        m.descent = -face->descender;
        m.leading = std::max(0.0, static_cast<double>(face->height) - (m.ascent + m.descent));
    }
    if (os2 && os2->version >= 2) {
        m.cap_height = os2->sCapHeight;
        m.x_height = os2->sxHeight;
    }
    if (m.cap_height == 0) m.cap_height = glyph_top(face, 'H');
    if (m.x_height == 0) m.x_height = glyph_top(face, 'x');
    if (post) {
        m.underline_position = post->underlinePosition;
        m.underline_thickness = post->underlineThickness;
        m.slant = static_cast<double>(post->italicAngle) / 65536.0;
    } else {
        m.underline_position = face->underline_position;
        m.underline_thickness = face->underline_thickness;
    }
    m.box_left = static_cast<double>(face->bbox.xMin);
    m.box_bottom = static_cast<double>(face->bbox.yMin);
    m.box_width = static_cast<double>(face->bbox.xMax - face->bbox.xMin);
    m.box_height = static_cast<double>(face->bbox.yMax - face->bbox.yMin);
    return m;
}

// Glyph outlines in glyph space (y down), each glyph moved to its offset.
struct OutlineSink {
    std::vector<PathElement>& path;
    const Matrix& transform;
    double scale{}, ox{}, oy{};
    double cx{}, cy{};   // the current point, design units
    double sx{}, sy{};   // the contour's start
    bool open{}, last_line{};
    [[nodiscard]] Point map(double x, double y) const {
        const double gx = ox + x * scale;
        const double gy = -(oy + y * scale);
        return {transform.a * gx + transform.c * gy + transform.tx, transform.b * gx + transform.d * gy + transform.ty};
    }
    // FreeType ends a contour with a line back to its start; Direct2D's
    // closed figure has that segment implicitly, so the close replaces it.
    void close() {
        if (!open) return;
        if (last_line && cx == sx && cy == sy) path.pop_back();
        path.push_back({PathOp::close, {}});
        open = false;
    }
    static int move_to(const FT_Vector* to, void* user) {
        auto& s = *static_cast<OutlineSink*>(user);
        s.close();
        s.cx = s.sx = static_cast<double>(to->x);
        s.cy = s.sy = static_cast<double>(to->y);
        s.last_line = false;
        s.path.push_back({PathOp::move, {s.map(s.cx, s.cy)}});
        s.open = true;
        return 0;
    }
    static int line_to(const FT_Vector* to, void* user) {
        auto& s = *static_cast<OutlineSink*>(user);
        s.cx = static_cast<double>(to->x);
        s.cy = static_cast<double>(to->y);
        s.path.push_back({PathOp::line, {s.map(s.cx, s.cy)}});
        s.last_line = true;
        return 0;
    }
    static int conic_to(const FT_Vector* control, const FT_Vector* to, void* user) {
        auto& s = *static_cast<OutlineSink*>(user);
        const double qx = static_cast<double>(control->x), qy = static_cast<double>(control->y);
        const double x = static_cast<double>(to->x), y = static_cast<double>(to->y);
        // The quadratic as a cubic (as Direct2D's sink receives it).
        const Point c1 = s.map(s.cx + 2.0 / 3.0 * (qx - s.cx), s.cy + 2.0 / 3.0 * (qy - s.cy));
        const Point c2 = s.map(x + 2.0 / 3.0 * (qx - x), y + 2.0 / 3.0 * (qy - y));
        s.path.push_back({PathOp::cubic, {c1, c2, s.map(x, y)}});
        s.cx = x;
        s.cy = y;
        s.last_line = false;
        return 0;
    }
    static int cubic_to(const FT_Vector* control1, const FT_Vector* control2, const FT_Vector* to, void* user) {
        auto& s = *static_cast<OutlineSink*>(user);
        s.path.push_back({PathOp::cubic,
                          {s.map(static_cast<double>(control1->x), static_cast<double>(control1->y)),
                           s.map(static_cast<double>(control2->x), static_cast<double>(control2->y)),
                           s.map(static_cast<double>(to->x), static_cast<double>(to->y))}});
        s.cx = static_cast<double>(to->x);
        s.cy = static_cast<double>(to->y);
        s.last_line = false;
        return 0;
    }
};

std::uint16_t glyph_of(FT_Face face, std::uint32_t code_point, bool symbol) {
    FT_UInt glyph = FT_Get_Char_Index(face, code_point);
    // Symbol fonts map U+F020..U+F0FF; their low codes reach the same glyphs.
    if (glyph == 0 && symbol && code_point < 0x100) glyph = FT_Get_Char_Index(face, 0xf000 + code_point);
    return glyph <= 0xffff ? static_cast<std::uint16_t>(glyph) : 0;
}

std::vector<std::uint32_t> code_points(std::u16string_view text, std::vector<std::size_t>* starts = nullptr) {
    std::vector<std::uint32_t> result;
    for (std::size_t index = 0; index < text.size(); ++index) {
        std::uint32_t c = text[index];
        if (starts) starts->push_back(index);
        if (c >= 0xd800 && c <= 0xdbff && index + 1 < text.size() && text[index + 1] >= 0xdc00 && text[index + 1] <= 0xdfff) {
            c = 0x10000 + ((c - 0xd800) << 10) + (text[index + 1] - 0xdc00);
            ++index;
        }
        result.push_back(c);
    }
    if (starts) starts->push_back(text.size());
    return result;
}

std::mutex registry_mutex;

} // namespace

bool Font::same_face(const Font& other) const {
    auto* a = impl();
    auto* b = other.impl();
    if (!a || !b) return false;
    if (a == b) return true;
    if (a->bytes || b->bytes) return a->bytes == b->bytes && a->index == b->index;
    return a->path == b->path && a->index == b->index;
}

Result<FontInfo> Font::info() const {
    if (!impl()) return Error::bad_handle;
    Lock lock(freetype::freetype_mutex());
    auto* face = face_of(*this);
    if (!face) return Error::io;
    FontInfo result;
    // Names as fontconfig reads them (English where localized; the WWS family first).
    if (auto* pattern = FcFreeTypeQueryFace(face, reinterpret_cast<const FcChar8*>(impl()->path.c_str()), static_cast<unsigned>(impl()->index), nullptr)) {
        result.family = pattern_string(pattern, FC_FAMILY, FC_FAMILYLANG);
        result.style_name = pattern_string(pattern, FC_STYLE, FC_STYLELANG);
        result.full_name = pattern_string(pattern, FC_FULLNAME, FC_FULLNAMELANG);
        const auto c = candidate_of(pattern);
        result.weight = c.weight;
        result.style = c.style;
        result.stretch = c.stretch;
        FcPatternDestroy(pattern);
    }
    if (result.family.empty() && face->family_name) result.family = face->family_name;
    if (result.style_name.empty() && face->style_name) result.style_name = face->style_name;
    if (const char* postscript = FT_Get_Postscript_Name(face)) result.postscript = postscript;
    // Weight, width and slope from OS/2 where the face has it (as DirectWrite reads them).
    if (const auto* os2 = os2_of(face)) {
        if (os2->usWeightClass >= 1 && os2->usWeightClass <= 999) result.weight = os2->usWeightClass;
        if (os2->usWidthClass >= 1 && os2->usWidthClass <= 9) result.stretch = os2->usWidthClass;
        result.style = (os2->fsSelection & (1U << 9)) != 0 ? font_style::oblique
                     : (os2->fsSelection & 1U) != 0          ? font_style::italic
                                                              : font_style::normal;
    } else if ((face->style_flags & FT_STYLE_FLAG_ITALIC) != 0 && result.style == font_style::normal) {
        result.style = font_style::italic;
    }
    result.metrics = design_metrics(face);
    result.monospace = FT_IS_FIXED_WIDTH(face);
    result.symbol = symbol_cmap(face);
    result.color = FT_HAS_COLOR(face);
    return result;
}

std::uint32_t Font::glyph_count() const {
    if (!impl()) return 0;
    Lock lock(freetype::freetype_mutex());
    auto* face = face_of(*this);
    return face ? static_cast<std::uint32_t>(face->num_glyphs) : 0;
}

Result<std::vector<std::uint16_t>> Font::glyphs(std::span<const std::uint32_t> code_points) const {
    if (!impl()) return Error::bad_handle;
    Lock lock(freetype::freetype_mutex());
    auto* face = face_of(*this);
    if (!face) return Error::io;
    const bool symbol = face->charmap && face->charmap->encoding == FT_ENCODING_MS_SYMBOL;
    std::vector<std::uint16_t> indices(code_points.size());
    for (std::size_t index = 0; index < code_points.size(); ++index) indices[index] = glyph_of(face, code_points[index], symbol);
    return indices;
}

Result<std::vector<GlyphMetrics>> Font::glyph_metrics(std::span<const std::uint16_t> glyphs) const {
    if (!impl()) return Error::bad_handle;
    Lock lock(freetype::freetype_mutex());
    auto* face = face_of(*this);
    if (!face) return Error::io;
    const auto metrics = design_metrics(face);
    const auto* os2 = os2_of(face);
    const double typo_ascent = os2 ? os2->sTypoAscender : metrics.ascent;
    const double typo_descent = os2 ? -os2->sTypoDescender : metrics.descent;
    const bool vertical = FT_HAS_VERTICAL(face);
    std::vector<GlyphMetrics> result(glyphs.size());
    for (std::size_t index = 0; index < glyphs.size(); ++index) {
        if (glyphs[index] >= face->num_glyphs) return Error::invalid_argument;
        if (FT_Load_Glyph(face, glyphs[index], FT_LOAD_NO_SCALE | FT_LOAD_NO_BITMAP | FT_LOAD_IGNORE_TRANSFORM) != 0) return Error::io;
        const auto& g = face->glyph->metrics;
        const bool empty = g.width == 0 && g.height == 0;
        // The glyph's bounding box (y up); an empty glyph's is at its origin.
        const auto x_min = static_cast<std::int32_t>(empty ? 0 : g.horiBearingX);
        const auto x_max = static_cast<std::int32_t>(empty ? 0 : g.horiBearingX + g.width);
        const auto y_max = static_cast<std::int32_t>(empty ? 0 : g.horiBearingY);
        const auto y_min = static_cast<std::int32_t>(empty ? 0 : g.horiBearingY - g.height);
        auto& out = result[index];
        out.advance_width = static_cast<std::uint32_t>(std::max<FT_Pos>(0, g.horiAdvance));
        out.left_bearing = x_min;
        out.right_bearing = static_cast<std::int32_t>(out.advance_width) - x_max;
        // Vertical metrics: vhea/vmtx where the face has them, else an advance
        // of the OS/2 typographic ascent + descent with the origin at that
        // ascent (DirectWrite's default, compared on arial.ttf).
        if (vertical) {
            out.advance_height = static_cast<std::uint32_t>(std::max<FT_Pos>(0, g.vertAdvance));
            out.vertical_origin_y = y_max + static_cast<std::int32_t>(g.vertBearingY);
        } else {
            out.advance_height = static_cast<std::uint32_t>(std::max(0.0, typo_ascent + typo_descent));
            out.vertical_origin_y = static_cast<std::int32_t>(typo_ascent);
        }
        out.top_bearing = out.vertical_origin_y - y_max;
        out.bottom_bearing = static_cast<std::int32_t>(out.advance_height) - out.vertical_origin_y + y_min;
    }
    return result;
}

Result<std::vector<PathElement>> Font::outlines(double size, std::span<const std::uint16_t> glyphs, std::span<const Point> offsets,
                                                 const Matrix& transform) const {
    if (!impl()) return Error::bad_handle;
    Lock lock(freetype::freetype_mutex());
    auto* face = face_of(*this);
    if (!face) return Error::io;
    std::vector<PathElement> path;
    const double units = face->units_per_EM ? face->units_per_EM : 1000;
    OutlineSink sink{path, transform, size / units};
    static const FT_Outline_Funcs funcs{&OutlineSink::move_to, &OutlineSink::line_to, &OutlineSink::conic_to, &OutlineSink::cubic_to, 0, 0};
    for (std::size_t index = 0; index < glyphs.size(); ++index) {
        const auto p = index < offsets.size() ? offsets[index] : Point{};
        sink.ox = p.x;
        sink.oy = p.y;
        if (FT_Load_Glyph(face, glyphs[index], FT_LOAD_NO_SCALE | FT_LOAD_NO_BITMAP | FT_LOAD_IGNORE_TRANSFORM) != 0) return Error::io;
        if (face->glyph->format != FT_GLYPH_FORMAT_OUTLINE) continue;   // a bitmap-only glyph has no outline
        if (FT_Outline_Decompose(&face->glyph->outline, &funcs, &sink) != 0) return Error::io;
        sink.close();
    }
    return path;
}

Result<std::vector<std::uint8_t>> Font::table(std::uint32_t tag) const {
    if (!impl()) return Error::bad_handle;
    Lock lock(freetype::freetype_mutex());
    auto* face = face_of(*this);
    if (!face) return Error::io;
    FT_ULong length = 0;
    if (!FT_IS_SFNT(face) || FT_Load_Sfnt_Table(face, tag, 0, nullptr, &length) != 0) return Error::not_found;
    std::vector<std::uint8_t> bytes(length);
    if (length && FT_Load_Sfnt_Table(face, tag, 0, bytes.data(), &length) != 0) return Error::io;
    return bytes;
}

Result<FontCollection> system_fonts() {
    static std::once_flag once;
    static FontCollection system;
    std::call_once(once, [] {
        if (!freetype::config() || !freetype::library()) return;
        system = FontCollection(std::make_shared<FontCollection::Impl>(FontCollection::Impl{FcSetSystem}));
    });
    if (!system) return Error::unsupported;
    return system;
}

FontCollection registered_fonts() {
    std::lock_guard lock(registry_mutex);
    return freetype::registered_;
}

namespace {

// The Windows families the macOS layer names: coretext/fonts.cpp maps Apple
// families to them, and win32 has them installed. Where this system lacks
// one, fontconfig picks its stand-in. The family's own aliases come first
// (Arial gives Liberation Sans, metric-compatible), then the generic family
// in the family's language. Other unknown families stay not found.
// `close` lists installed families nearer in width than the generic one,
// best first. For Segoe UI: Selawik (Microsoft's metric-compatible open
// font), then Open Sans (+4.5% wider on mixed text, against DejaVu Sans's
// +13%, measured with HarfBuzz on Windows's segoeui.ttf).
struct StandIn {
    std::string_view family, close, generic, lang;
};
constexpr StandIn stand_ins[] = {
    {"Segoe UI", "Selawik,Open Sans", "sans-serif", ""},
    {"Arial", "", "sans-serif", ""},
    {"Verdana", "", "sans-serif", ""},
    {"Tahoma", "", "sans-serif", ""},
    {"Times New Roman", "", "serif", ""},
    {"Georgia", "", "serif", ""},
    {"Consolas", "", "monospace", ""},
    {"Courier New", "", "monospace", ""},
    {"Segoe UI Emoji", "", "emoji", ""},
    {"Segoe UI Symbol", "", "sans-serif", ""},
    {"Microsoft YaHei", "", "sans-serif", "zh-cn"},
    {"Microsoft JhengHei", "", "sans-serif", "zh-tw"},
    {"Yu Gothic UI", "", "sans-serif", "ja"},
    {"Malgun Gothic", "", "sans-serif", "ko"},
};

// The stand-in family for `family`, or nothing; under freetype_mutex().
std::optional<std::string> stand_in_family(std::string_view family, int weight, int style, int stretch) {
    auto* config = freetype::config();
    const auto entry = std::find_if(std::begin(stand_ins), std::end(stand_ins), [&](const StandIn& s) {
        return same_name(reinterpret_cast<const FcChar8*>(std::string(s.family).c_str()), family);
    });
    if (!config || entry == std::end(stand_ins)) return std::nullopt;
    FcPattern* pattern = FcPatternCreate();
    const std::string name(family), generic(entry->generic), lang(entry->lang);
    FcPatternAddString(pattern, FC_FAMILY, reinterpret_cast<const FcChar8*>(name.c_str()));
    for (std::size_t start = 0; start < entry->close.size();) {
        const auto end = std::min(entry->close.find(',', start), entry->close.size());
        const std::string close(entry->close.substr(start, end - start));
        FcPatternAddString(pattern, FC_FAMILY, reinterpret_cast<const FcChar8*>(close.c_str()));
        start = end + 1;
    }
    FcPatternAddString(pattern, FC_FAMILY, reinterpret_cast<const FcChar8*>(generic.c_str()));
    if (!lang.empty()) FcPatternAddString(pattern, FC_LANG, reinterpret_cast<const FcChar8*>(lang.c_str()));
    FcPatternAddDouble(pattern, FC_WEIGHT, FcWeightFromOpenTypeDouble(weight));
    FcPatternAddInteger(pattern, FC_SLANT, static_cast<int>(fc_slant(style)));
    FcPatternAddDouble(pattern, FC_WIDTH, fc_width(stretch));
    FcConfigSubstitute(config, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult result{};
    FcPattern* match = FcFontMatch(config, pattern, &result);
    FcPatternDestroy(pattern);
    if (!match) return std::nullopt;
    auto found = pattern_string(match, FC_FAMILY, FC_FAMILYLANG);
    FcPatternDestroy(match);
    if (found.empty()) return std::nullopt;
    return found;
}

// The members of `family`, or of its stand-in in the system collection.
std::vector<Candidate> members_or_stand_in(const FontCollection& collection, std::string_view family, int weight, int style, int stretch) {
    auto* set = font_set(collection);
    auto members = family_members(set, family);
    if (members.empty() && collection.impl()->set == FcSetSystem)
        if (const auto stand_in = stand_in_family(family, weight, style, stretch)) members = family_members(set, *stand_in);
    return members;
}

} // namespace

Result<Font> family_font(const FontCollection& collection, std::string_view family, int weight, int style, int stretch) {
    if (!collection) return Error::bad_handle;
    Lock lock(freetype::freetype_mutex());
    const auto members = members_or_stand_in(collection, family, weight, style, stretch);
    if (members.empty()) return Error::not_found;
    const auto best = std::min_element(members.begin(), members.end(), [&](const Candidate& a, const Candidate& b) {
        return match_key(a, weight, style, stretch) < match_key(b, weight, style, stretch);
    });
    return freetype::font_for_file(best->file, best->index);
}

Result<Font> named_font(const FontCollection& collection, FontName kind, std::string_view name) {
    if (!collection) return Error::bad_handle;
    Lock lock(freetype::freetype_mutex());
    auto* set = font_set(collection);
    if (!set) return Error::not_found;
    const std::string wanted(name);
    for (int font = 0; font < set->nfont; ++font) {
        auto* pattern = set->fonts[font];
        if (variable_default(pattern)) continue;
        FcChar8* value{};
        const char* object = kind == FontName::postscript ? FC_POSTSCRIPT_NAME : FC_FULLNAME;
        for (int index = 0; FcPatternGetString(pattern, object, index, &value) == FcResultMatch; ++index)
            if (FcStrCmpIgnoreCase(value, reinterpret_cast<const FcChar8*>(wanted.c_str())) == 0) {
                const auto c = candidate_of(pattern);
                if (c.file.empty()) break;
                return freetype::font_for_file(c.file, c.index);
            }
    }
    return Error::not_found;
}

Result<std::vector<Font>> family_faces(const FontCollection& collection, std::string_view family) {
    if (!collection) return Error::bad_handle;
    Lock lock(freetype::freetype_mutex());
    auto members = members_or_stand_in(collection, family, font_weight::normal, font_style::normal, font_stretch::normal);
    if (members.empty()) return Error::not_found;
    std::stable_sort(members.begin(), members.end(), [](const Candidate& a, const Candidate& b) {
        return std::tie(a.stretch, a.style, a.weight) < std::tie(b.stretch, b.style, b.weight);
    });
    std::vector<Font> faces;
    for (const auto& member : members) faces.push_back(freetype::font_for_file(member.file, member.index));
    return faces;
}

Result<std::vector<std::string>> family_names(const FontCollection& collection) {
    if (!collection) return Error::bad_handle;
    Lock lock(freetype::freetype_mutex());
    std::vector<std::string> names;
    if (auto* set = font_set(collection))
        for (int font = 0; font < set->nfont; ++font) {
            auto name = pattern_string(set->fonts[font], FC_FAMILY, FC_FAMILYLANG);
            if (!name.empty() && std::find(names.begin(), names.end(), name) == names.end()) names.push_back(std::move(name));
        }
    std::sort(names.begin(), names.end());
    return names;
}

Status register_font_file(std::span<const std::uint8_t> bytes) {
    if (bytes.empty()) return Error::invalid_argument;
    auto* config = freetype::config();
    auto* library = freetype::library();
    if (!config || !library) return Error::unsupported;
    {
        Lock lock(freetype::freetype_mutex());
        FT_Face probe{};
        if (FT_New_Memory_Face(library, bytes.data(), static_cast<FT_Long>(bytes.size()), -1, &probe) != 0) return Error::unsupported;
        const auto faces = probe->num_faces;
        FT_Done_Face(probe);
        if (faces <= 0) return Error::unsupported;
    }
    // fontconfig adds files by path: an anonymous memory file, kept open for
    // the process's life, is reached through /proc/self/fd.
    const int fd = memfd_create("alchemy-font", MFD_CLOEXEC);
    if (fd < 0) return Error::io;
    std::size_t written = 0;
    while (written < bytes.size()) {
        const auto n = write(fd, bytes.data() + written, bytes.size() - written);
        if (n <= 0) {
            ::close(fd);
            return Error::io;
        }
        written += static_cast<std::size_t>(n);
    }
    const auto path = "/proc/self/fd/" + std::to_string(fd);
    std::lock_guard registry(registry_mutex);
    Lock lock(freetype::freetype_mutex());
    if (!FcConfigAppFontAddFile(config, reinterpret_cast<const FcChar8*>(path.c_str()))) {
        ::close(fd);
        return Error::other;
    }
    freetype::registered_files_.push_back(fd);
    ++freetype::serial_;
    freetype::registered_ = FontCollection(std::make_shared<FontCollection::Impl>(FontCollection::Impl{FcSetApplication}));
    return {};
}

Result<Font> font_from_data(std::span<const std::uint8_t> bytes) {
    auto* library = freetype::library();
    if (!library) return Error::unsupported;
    if (bytes.empty()) return Error::unsupported;
    auto impl = std::make_shared<Font::Impl>();
    impl->bytes = std::make_shared<const std::vector<std::uint8_t>>(bytes.begin(), bytes.end());
    Font font(impl);
    Lock lock(freetype::freetype_mutex());
    if (!face_of(font)) return Error::unsupported;
    return font;
}

Result<Fallback> fallback_font(std::u16string_view text, std::string_view family, int weight, int style, int stretch) {
    auto* config = freetype::config();
    if (!config || !freetype::library()) return Error::unsupported;
    std::vector<std::size_t> starts;
    const auto points = code_points(text, &starts);
    if (points.empty()) return Error::not_found;
    Lock lock(freetype::freetype_mutex());
    // The family's own face where it covers the start (DirectWrite maps that to the family).
    Font base;
    if (auto system = system_fonts()) {
        if (auto found = family_font(*system, family, weight, style, stretch)) base = *found;
        else if (auto registered = registered_fonts()) {
            if (auto in_registered = family_font(registered, family, weight, style, stretch)) base = *in_registered;
        }
    }
    FT_Face base_face = base ? face_of(base) : nullptr;
    const auto base_has = [&](std::uint32_t c) { return base_face && FT_Get_Char_Index(base_face, c) != 0; };
    if (base_has(points[0])) {
        std::size_t count = 1;
        while (count < points.size() && base_has(points[count])) ++count;
        return Fallback{starts[count], base, 1};
    }
    // fontconfig's fallback order for the family: the first face with the character.
    FcPattern* pattern = FcPatternCreate();
    const std::string name(family);
    if (!name.empty()) FcPatternAddString(pattern, FC_FAMILY, reinterpret_cast<const FcChar8*>(name.c_str()));
    FcPatternAddDouble(pattern, FC_WEIGHT, FcWeightFromOpenTypeDouble(weight));
    FcPatternAddInteger(pattern, FC_SLANT, static_cast<int>(fc_slant(style)));
    FcPatternAddDouble(pattern, FC_WIDTH, fc_width(stretch));
    FcConfigSubstitute(config, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult result{};
    FcFontSet* sorted = FcFontSort(config, pattern, FcFalse, nullptr, &result);
    FcPatternDestroy(pattern);
    if (!sorted) return Error::not_found;
    FcCharSet* chosen_charset{};
    Candidate chosen;
    for (int font = 0; font < sorted->nfont && chosen.file.empty(); ++font) {
        FcCharSet* charset{};
        if (FcPatternGetCharSet(sorted->fonts[font], FC_CHARSET, 0, &charset) != FcResultMatch || !FcCharSetHasChar(charset, points[0])) continue;
        chosen = candidate_of(sorted->fonts[font]);
        if (!chosen.file.empty()) chosen_charset = FcCharSetCopy(charset);
    }
    FcFontSetDestroy(sorted);
    if (chosen.file.empty()) return Error::not_found;
    // The run continues while the family still lacks the characters and the fallback has them.
    std::size_t count = 1;
    while (count < points.size() && !base_has(points[count]) && FcCharSetHasChar(chosen_charset, points[count])) ++count;
    FcCharSetDestroy(chosen_charset);
    return Fallback{starts[count], freetype::font_for_file(chosen.file, chosen.index), 1};
}

} // namespace alchemy::host::gpu
