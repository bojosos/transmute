// Linux host backend, area "gpu": system text rasterizing for AppKit's software text drawing
// (include/alchemy/host/gpu.hpp), the counterpart of GDI on win32. fontconfig picks the face
// for a family, weight and slant (substituting as GDI's font mapper does); FreeType gives
// hinted pixel metrics and advances and grayscale coverage. A family without a bold or italic
// face is emboldened or slanted, as GDI simulates them. One mutex (FreeType's) serves all.
#include "alchemy/host/linux/gui/fonts.hpp"

#include FT_SYNTHESIS_H
#include FT_TRUETYPE_TABLES_H

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <tuple>
#include <vector>

namespace alchemy::host::gpu {
namespace {

struct RasterFace {
    FT_Face face{};
    bool embolden{}, oblique{};
    int ppem{};
    std::vector<std::uint8_t> device_widths;   // hdmx widths at this size, per glyph (empty: none)
    std::optional<int> vdmx_ascent, vdmx_descent;
};

std::map<std::tuple<std::string, int, bool, bool>, RasterFace> faces;   // under freetype_mutex()

std::vector<std::uint8_t> sfnt_table(FT_Face face, FT_ULong tag) {
    FT_ULong length = 0;
    if (FT_Load_Sfnt_Table(face, tag, 0, nullptr, &length) != 0 || length == 0) return {};
    std::vector<std::uint8_t> bytes(length);
    if (FT_Load_Sfnt_Table(face, tag, 0, bytes.data(), &length) != 0) return {};
    return bytes;
}
int u16(const std::vector<std::uint8_t>& b, std::size_t at) { return at + 2 <= b.size() ? b[at] << 8 | b[at + 1] : 0; }
int s16(const std::vector<std::uint8_t>& b, std::size_t at) { return static_cast<std::int16_t>(u16(b, at)); }

// The Windows device tables GDI reads at a pixel size: hdmx (hinted advance
// widths) and VDMX (the ascent and descent of the hinted glyphs).
void device_tables(RasterFace& raster) {
    auto* face = raster.face;
    const auto hdmx = sfnt_table(face, FT_MAKE_TAG('h', 'd', 'm', 'x'));
    if (hdmx.size() >= 8) {
        const int records = s16(hdmx, 2);
        const std::size_t size = static_cast<std::size_t>(u16(hdmx, 4)) << 16 | static_cast<std::size_t>(u16(hdmx, 6));
        const auto glyphs = static_cast<std::size_t>(face->num_glyphs);
        for (int r = 0; r < records && size >= glyphs + 2; ++r) {
            const std::size_t at = 8 + static_cast<std::size_t>(r) * size;
            if (at + 2 + glyphs > hdmx.size()) break;
            if (hdmx[at] == raster.ppem) {
                raster.device_widths.assign(hdmx.begin() + static_cast<std::ptrdiff_t>(at + 2), hdmx.begin() + static_cast<std::ptrdiff_t>(at + 2 + glyphs));
                break;
            }
        }
    }
    const auto vdmx = sfnt_table(face, FT_MAKE_TAG('V', 'D', 'M', 'X'));
    if (vdmx.size() >= 6) {
        const int ratios = u16(vdmx, 4);
        for (int r = 0; r < ratios; ++r) {
            const std::size_t ratio = 6 + static_cast<std::size_t>(r) * 4;
            if (ratio + 4 > vdmx.size()) break;
            const int x = vdmx[ratio + 1], y_start = vdmx[ratio + 2], y_end = vdmx[ratio + 3];
            if (!(x == 0 && y_start == 0 && y_end == 0) && !(x == 1 && y_start <= 1 && y_end >= 1)) continue;   // 1:1 or any
            const std::size_t group = static_cast<std::size_t>(u16(vdmx, 6 + static_cast<std::size_t>(ratios) * 4 + static_cast<std::size_t>(r) * 2));
            const int records = u16(vdmx, group);
            for (int e = 0; e < records; ++e) {
                const std::size_t entry = group + 4 + static_cast<std::size_t>(e) * 6;
                if (entry + 6 > vdmx.size()) break;
                if (u16(vdmx, entry) == raster.ppem) {
                    raster.vdmx_ascent = s16(vdmx, entry + 2);
                    raster.vdmx_descent = -s16(vdmx, entry + 4);
                    return;
                }
            }
            return;
        }
    }
}

// The face for `spec` at its pixel size, or nullptr; under freetype_mutex().
RasterFace* raster_face(const RasterFont& spec) {
    const int quarters = std::max(4, spec.quarter_pixels);
    const auto key = std::make_tuple(spec.family, quarters, spec.bold, spec.italic);
    if (const auto found = faces.find(key); found != faces.end()) return &found->second;
    auto* config = freetype::config();
    auto* library = freetype::library();
    if (!config || !library) return nullptr;
    FcPattern* pattern = FcPatternCreate();
    FcPatternAddString(pattern, FC_FAMILY, reinterpret_cast<const FcChar8*>(spec.family.c_str()));
    FcPatternAddInteger(pattern, FC_WEIGHT, spec.bold ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR);
    FcPatternAddInteger(pattern, FC_SLANT, spec.italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);
    FcPatternAddBool(pattern, FC_SCALABLE, FcTrue);
    FcConfigSubstitute(config, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult result{};
    FcPattern* match = FcFontMatch(config, pattern, &result);
    FcPatternDestroy(pattern);
    if (!match) return nullptr;
    FcChar8* file{};
    int index{}, weight{FC_WEIGHT_REGULAR}, slant{FC_SLANT_ROMAN};
    RasterFace raster;
    if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch) {
        static_cast<void>(FcPatternGetInteger(match, FC_INDEX, 0, &index));
        static_cast<void>(FcPatternGetInteger(match, FC_WEIGHT, 0, &weight));
        static_cast<void>(FcPatternGetInteger(match, FC_SLANT, 0, &slant));
        if (FT_New_Face(library, reinterpret_cast<const char*>(file), index, &raster.face) != 0) raster.face = nullptr;
    }
    FcPatternDestroy(match);
    if (!raster.face) return nullptr;
    static_cast<void>(FT_Select_Charmap(raster.face, FT_ENCODING_UNICODE));
    raster.ppem = std::max(1, static_cast<int>(std::lround(quarters / 4.0)));
    if (FT_Set_Pixel_Sizes(raster.face, 0, static_cast<FT_UInt>(raster.ppem)) != 0) {
        FT_Done_Face(raster.face);
        return nullptr;
    }
    device_tables(raster);
    raster.embolden = spec.bold && weight < FC_WEIGHT_DEMIBOLD;
    raster.oblique = spec.italic && slant == FC_SLANT_ROMAN;
    return &faces.emplace(key, raster).first->second;
}

std::vector<std::uint32_t> code_points(std::u16string_view text, std::vector<std::size_t>& ends) {
    std::vector<std::uint32_t> result;
    for (std::size_t index = 0; index < text.size(); ++index) {
        std::uint32_t c = text[index];
        if (c >= 0xd800 && c <= 0xdbff && index + 1 < text.size() && text[index + 1] >= 0xdc00 && text[index + 1] <= 0xdfff) {
            c = 0x10000 + ((c - 0xd800) << 10) + (text[index + 1] - 0xdc00);
            ++index;
        }
        result.push_back(c);
        ends.push_back(index + 1);
    }
    return result;
}

// Loads `c` hinted (and simulated); the advance in whole pixels, or -1.
int load(RasterFace& raster, std::uint32_t c, bool render) {
    auto* face = raster.face;
    if (FT_Load_Char(face, c, FT_LOAD_DEFAULT | FT_LOAD_TARGET_NORMAL | FT_LOAD_NO_BITMAP) != 0) return -1;
    if (raster.oblique) FT_GlyphSlot_Oblique(face->glyph);
    if (raster.embolden) FT_GlyphSlot_Embolden(face->glyph);
    if (render && FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0) return -1;
    // GDI's advance: the hdmx width at this size where the face has one.
    const auto glyph = face->glyph->glyph_index;
    if (!raster.embolden && glyph < raster.device_widths.size()) return raster.device_widths[glyph];
    return static_cast<int>((face->glyph->advance.x + 32) >> 6);
}

} // namespace

Result<RasterMetrics> raster_metrics(const RasterFont& font) {
    std::lock_guard lock(freetype::freetype_mutex());
    auto* raster = raster_face(font);
    if (!raster) return Error::other;
    auto* face = raster->face;
    const double scale = static_cast<double>(raster->ppem) / std::max<FT_UShort>(1, face->units_per_EM);
    const auto pixels = [&](double units) { return static_cast<double>(std::lround(units * scale)); };
    RasterMetrics result;
    // GDI's text metrics of a TrueType face: the VDMX ascent and descent at
    // this size (else usWin*), the external leading that completes hhea's line
    // height. Cap and x heights are the face's real ones: GDI's otmsCapEmHeight
    // and otmsXHeight are documented as unsupported (win32 reports em/2, em/4).
    const auto* os2 = static_cast<const TT_OS2*>(FT_Get_Sfnt_Table(face, FT_SFNT_OS2));
    const auto* hhea = static_cast<const TT_HoriHeader*>(FT_Get_Sfnt_Table(face, FT_SFNT_HHEA));
    if (os2 && os2->version != 0xffff) {
        result.ascent = raster->vdmx_ascent ? *raster->vdmx_ascent : pixels(os2->usWinAscent);
        result.descent = raster->vdmx_descent ? *raster->vdmx_descent : pixels(os2->usWinDescent);
        if (hhea) result.leading = std::max(0.0, pixels(hhea->Ascender - hhea->Descender + hhea->Line_Gap - (os2->usWinAscent + os2->usWinDescent)));
        result.average_width = pixels(os2->xAvgCharWidth);
        if (os2->version >= 2 && os2->sCapHeight > 0) result.cap_height = pixels(os2->sCapHeight);
        if (os2->version >= 2 && os2->sxHeight > 0) result.x_height = pixels(os2->sxHeight);
    } else {
        result.ascent = static_cast<double>((face->size->metrics.ascender + 63) >> 6);
        result.descent = static_cast<double>((-face->size->metrics.descender + 63) >> 6);
        result.average_width = static_cast<double>((face->size->metrics.max_advance + 63) >> 6) / 2;
    }
    result.max_width = pixels(static_cast<double>(face->bbox.xMax - face->bbox.xMin));   // GDI: the glyph box's width
    if (!result.cap_height && load(*raster, 'H', false) >= 0) result.cap_height = static_cast<double>((face->glyph->metrics.horiBearingY + 32) >> 6);
    if (!result.x_height && load(*raster, 'x', false) >= 0) result.x_height = static_cast<double>((face->glyph->metrics.horiBearingY + 32) >> 6);
    return result;
}

Result<double> raster_width(const RasterFont& font, std::u16string_view text) {
    auto prefix = raster_prefix_widths(font, text);
    if (!prefix) return prefix.error();
    return prefix->empty() ? 0.0 : prefix->back();
}

Result<std::vector<double>> raster_prefix_widths(const RasterFont& font, std::u16string_view text) {
    std::vector<double> result(text.size());
    if (text.empty()) return result;
    std::lock_guard lock(freetype::freetype_mutex());
    auto* raster = raster_face(font);
    if (!raster) return Error::other;
    std::vector<std::size_t> ends;
    const auto points = code_points(text, ends);
    double pen = 0;
    std::size_t unit = 0;
    for (std::size_t i = 0; i < points.size(); ++i) {
        pen += std::max(0, load(*raster, points[i], false));
        for (; unit < ends[i]; ++unit) result[unit] = pen;   // both halves of a surrogate pair end there
    }
    return result;
}

Result<std::vector<float>> raster_coverage(const RasterFont& font, std::u16string_view text, int width, int height, int x, int baseline) {
    if (width <= 0 || height <= 0) return Error::invalid_argument;
    std::vector<float> coverage(static_cast<std::size_t>(width) * height);
    std::lock_guard lock(freetype::freetype_mutex());
    auto* raster = raster_face(font);
    if (!raster) return Error::other;
    std::vector<std::size_t> ends;
    int pen = x;
    for (const auto c : code_points(text, ends)) {
        const int advance = load(*raster, c, true);
        if (advance < 0) continue;
        const auto& slot = *raster->face->glyph;
        const auto& bitmap = slot.bitmap;
        if (bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
            const int left = pen + slot.bitmap_left, top = baseline - slot.bitmap_top;
            for (unsigned row = 0; row < bitmap.rows; ++row) {
                const int y = top + static_cast<int>(row);
                if (y < 0 || y >= height) continue;
                const auto* source = bitmap.buffer + static_cast<std::ptrdiff_t>(row) * bitmap.pitch;
                for (unsigned column = 0; column < bitmap.width; ++column) {
                    const int px = left + static_cast<int>(column);
                    if (px < 0 || px >= width) continue;
                    auto& cell = coverage[static_cast<std::size_t>(y) * width + px];
                    cell = std::max(cell, source[column] / 255.0f);
                }
            }
        }
        pen += advance;
    }
    return coverage;
}

} // namespace alchemy::host::gpu
