#pragma once

// CoreText over the host's fonts and text layout (host::gpu; DirectWrite on win32;
// owner: Q): fonts, attributed strings, paragraph
// styles and line layout. Shared by src/alchemy/darwin/coretext/*.cpp, the Quartz
// context (glyph drawing) and the AppKit string drawing additions.

#include "alchemy/darwin/coregraphics/q2d_internal.hpp"
#include "alchemy/host/gpu.hpp"

#include <string>
#include <vector>

namespace alchemy::gui::quartz2d {

// The glyph names of a face's 'post' table, read on first use (glyph_names.cpp).
struct GlyphNames;
[[nodiscard]] std::shared_ptr<GlyphNames> make_glyph_names();

// A host font face with the names and metrics CoreText reports.
struct FontFace {
    host::gpu::Font font;
    host::gpu::FontCollection collection;   // where `family` resolves (system or registered fonts)
    std::string family, full_name, postscript, style_name;   // the host's names
    int weight{host::gpu::font_weight::normal};
    int style{host::gpu::font_style::normal};
    int stretch{host::gpu::font_stretch::normal};
    FontMetrics metrics;
    bool monospace{}, symbol{}, color{};
    std::shared_ptr<GlyphNames> glyph_names = make_glyph_names();
};

// CTFontSymbolicTraits
namespace font_trait {
inline constexpr std::uint32_t italic = 1U << 0, bold = 1U << 1, expanded = 1U << 5, condensed = 1U << 6,
    monospace = 1U << 10, vertical = 1U << 11, ui_optimized = 1U << 12, color_glyphs = 1U << 13, composite = 1U << 14;
}

// CTFontRef / NSFont (toll-free bridged: the CTFont host class is NSFont).
struct FontState final : ObjectPayload {
    std::shared_ptr<FontFace> face;
    double size{12};
    Affine matrix{};
    std::string postscript, family, display;   // the names reported to the guest (Apple names for mapped fonts)
    [[nodiscard]] double scale() const { return size / std::max(1.0, face->metrics.units_per_em); }
    [[nodiscard]] std::uint32_t traits() const;
};
// CGFontRef.
struct GraphicsFontState final : ObjectPayload {
    std::shared_ptr<FontFace> face;
    std::string postscript, family;
};

// Font resolution (fonts.cpp).
struct ResolvedFont {
    std::shared_ptr<FontFace> face;
    std::string postscript, family, display;
};
// An Apple font name (PostScript, full or family name, including the
// system UI names) through the mapping table, then the host's system and
// registered fonts; nullopt when nothing matches.
[[nodiscard]] std::optional<ResolvedFont> resolve_font(std::string_view name);
// The CTFontUIFontType font and its default size.
[[nodiscard]] std::optional<std::pair<ResolvedFont, double>> ui_font(std::int64_t type);
[[nodiscard]] std::shared_ptr<FontFace> face_from(const host::gpu::Font& font, const host::gpu::FontCollection& collection = {});
// The face of `face`'s family at another weight (its style and stretch), or null.
[[nodiscard]] std::shared_ptr<FontFace> family_match(const FontFace& face, int weight);
// The same family with other weight / style (CTFontCreateCopyWithSymbolicTraits).
[[nodiscard]] std::optional<ResolvedFont> with_traits(const FontState& font, std::uint32_t traits, std::uint32_t mask);
[[nodiscard]] std::optional<ResolvedFont> with_family(const FontState& font, std::string_view family);
// Glyphs for UTF-16 units (0 where the font has none); for a surrogate pair
// the glyph goes to the first unit and the second unit gets 0, as CoreText does.
[[nodiscard]] std::vector<std::uint16_t> glyphs_for(const FontFace& face, std::span<const std::uint16_t> units);
[[nodiscard]] std::vector<double> advances_for(const FontFace& face, double size, std::span<const std::uint16_t> glyphs);
[[nodiscard]] std::vector<Rect> glyph_bounds(const FontFace& face, double size, std::span<const std::uint16_t> glyphs);
// A glyph's 'post' table name (nullopt: the face names no glyphs, or not this one)
// and the first glyph of a name (0: none) (glyph_names.cpp).
[[nodiscard]] std::optional<std::string> glyph_name(const FontFace& face, std::uint16_t glyph);
[[nodiscard]] std::uint16_t glyph_with_name(const FontFace& face, std::string_view name);
// Registers a font file's faces for name lookup (CTFontManagerRegisterFontsForURL).
bool register_font_file(std::span<const std::uint8_t> bytes, std::string* error);
[[nodiscard]] std::vector<std::string> available_families();
// The faces of a family (an Apple family name through the mapping table),
// without simulated styles; empty when the family is unknown.
[[nodiscard]] std::vector<std::shared_ptr<FontFace>> family_members(std::string_view family);

// ---- paragraph styles (attributed.cpp) --------------------------------------------------
// CTTextAlignment values (NSTextAlignment on arm64 macOS: left 0, center 1,
// right 2, justified 3, natural 4 -- converted where NSParagraphStyle is read).
namespace alignment {
inline constexpr std::int32_t left = 0, right = 1, center = 2, justified = 3, natural = 4;
}
struct ParagraphStyle {
    std::int32_t alignment{alignment::natural};
    std::int32_t line_break{0};               // CTLineBreakMode / NSLineBreakMode
    double first_head_indent{}, head_indent{}, tail_indent{};
    double line_height_multiple{}, maximum_line_height{}, minimum_line_height{}, line_spacing{};
    double paragraph_spacing{}, paragraph_spacing_before{};
    double default_tab_interval{};
    std::vector<double> tab_stops;
    std::int32_t writing_direction{-1};       // natural
    float hyphenation_factor{};               // NSParagraphStyle (kept; lines are not hyphenated)
    bool allows_tightening{};                 // -allowsDefaultTighteningForTruncation (kept)
};
// CTParagraphStyleRef and NSParagraphStyle / NSMutableParagraphStyle.
struct ParagraphState final : ObjectPayload {
    ParagraphStyle style;
    // NSTextTab / CTTextTab objects (+1) once tab stops were set; until then
    // the style has AppKit's default stops (12 left tabs, 28 points apart).
    std::vector<Address> tabs;
    bool tabs_set{};
};

// ---- attributed strings (attributed.cpp) --------------------------------------------------
struct AttributeRun {
    std::size_t length{};
    Address attributes{};                     // +1 CFDictionary (never 0)
};
// NSAttributedString / NSMutableAttributedString / CFAttributedString.
struct AttributedState final : ObjectPayload {
    std::mutex mutex;
    std::vector<std::uint16_t> text;
    std::vector<AttributeRun> runs;           // lengths add up to text.size()
    Address string_cache{};                   // +1 CFString of `text` (-string)
    bool mutable_string{};
    std::int64_t editing{};
};
// A snapshot of an attributed string (any NSAttributedString, through
// messages when it is not one of ours). Attribute dictionaries are +1.
struct AttributedText {
    std::vector<std::uint16_t> text;
    std::vector<AttributeRun> runs;
};
// The resolved text style of an attribute dictionary.
struct TextStyle {
    Address font{};                           // +1 CTFont (the attribute's, or the default Helvetica 12)
    std::shared_ptr<FontState> font_state;
    std::optional<Rgba> color;                // nullopt: the context's fill color (kCTForegroundColorFromContext)
    std::optional<Rgba> background;
    double kern{};
    std::int64_t underline{}, strikethrough{};
    std::optional<Rgba> underline_color, strikethrough_color;
    double baseline_offset{};
    double stroke_width{};                    // percent of the font size; negative: fill and stroke
    std::optional<Rgba> stroke_color;
    ParagraphStyle paragraph;
    bool has_paragraph{};
};

// ---- line layout (typesetter.cpp) ----------------------------------------------------------
struct Range { std::int64_t location{}, length{}; };

struct RunLayout {
    std::shared_ptr<FontFace> face;
    Address font{};                           // +1 CTFont actually used (fallback fonts differ from the attribute)
    Address attributes{};                     // +1 the source attribute dictionary
    bool substituted{};                       // font differs from the attribute's
    double size{12};
    std::vector<std::uint16_t> glyphs;
    std::vector<Point> positions;             // relative to the line origin, y up
    std::vector<Size> advances;
    std::vector<std::int64_t> indices;        // string index of each glyph
    Range range;                              // string range covered
    bool rtl{};
    double width{}, ascent{}, descent{}, leading{};
    std::size_t style{};                      // index into LineLayout::styles
};
struct LineLayout {
    std::vector<RunLayout> runs;
    std::vector<TextStyle> styles;            // +1 references inside
    Range range;
    double width{}, trailing_whitespace{}, ascent{}, descent{}, leading{};
    AttributedText source;                    // the line's text (truncation), +1 dictionaries
    std::int64_t source_offset{};             // string index of source.text[0]
    bool paragraph_end{};
    ParagraphStyle paragraph;
};

// The CoreText part of the provider: classes and shared objects.
struct Coretext {
    Address font_class{};                     // NSFont == CTFont
    Address descriptor_class{};               // NSFontDescriptor == CTFontDescriptor
    Address paragraph_class{};                // CTParagraphStyle
    Address ns_paragraph_class{}, ns_mutable_paragraph_class{};
    Address attributed_class{}, mutable_attributed_class{};
    Address line_class{}, run_class{}, framesetter_class{}, frame_class{}, typesetter_class{};
    Address default_font{};                   // immortal Helvetica 12
    Address empty_attributes{};               // immortal empty CFDictionary
    Address default_paragraph{};              // immortal +[NSParagraphStyle defaultParagraphStyle]
    Address font_manager{};                   // immortal +[NSFontManager sharedFontManager]
};
[[nodiscard]] Coretext& coretext();

void install_font_exports(Quartz& q);         // font_exports.cpp
void install_attributed_strings(Quartz& q);   // attributed.cpp
void install_typesetter(Quartz& q);           // typesetter.cpp

// +1 NSFont / CTFont for a resolved font.
[[nodiscard]] Address make_font(Quartz& q, const ResolvedFont& font, double size, const Affine& matrix = {});
[[nodiscard]] std::shared_ptr<FontState> font_state(Quartz& q, Address font);   // nullptr: not a font
// Color of an attribute value: a CGColor, or an NSColor (through -getRed:green:blue:alpha:).
[[nodiscard]] std::optional<Rgba> object_color(Quartz& q, Address color);
// The style of an attribute dictionary; `font` is +1 (release_style).
[[nodiscard]] TextStyle resolve_style(Quartz& q, Address attributes);
void release_style(Quartz& q, TextStyle& style);
// A snapshot of any NSAttributedString / CFAttributedString (+1 dictionaries).
[[nodiscard]] AttributedText attributed_text(Quartz& q, Address string);
// A plain string with one attribute dictionary (retained).
[[nodiscard]] AttributedText plain_text(Quartz& q, Address string, Address attributes);
void release_text(Quartz& q, AttributedText& text);
[[nodiscard]] AttributedText copy_text(Quartz& q, const AttributedText& text);   // retains the dictionaries again
// +1 NSAttributedString / NSMutableAttributedString adopting `text`'s references.
[[nodiscard]] Address make_attributed(Quartz& q, AttributedText text, bool mutable_string);
[[nodiscard]] ParagraphStyle paragraph_of(Quartz& q, Address paragraph);   // CTParagraphStyle or NSParagraphStyle

// Lays out text[range] (length 0: to the end) into lines at most `width`
// wide (single_line: one line, newlines laid out as zero-width breaks).
// Lines own references; release them with release_line.
[[nodiscard]] std::vector<LineLayout> layout_lines(Quartz& q, const AttributedText& text, Range range, double width, bool single_line);
void release_line(Quartz& q, LineLayout& line);

// Lines of text[range] placed CoreText-style in a box `width` wide and at
// most `height` tall (unlimited: 1e7): wrapping per paragraph, truncation
// modes, line height rules, alignment. Origins are {x, baseline distance
// from the top of the box}.
struct PlacedText {
    std::vector<LineLayout> lines;
    std::vector<Point> origins;
    double used_height{}, max_width{};
    std::int64_t end{};
};
[[nodiscard]] PlacedText place_text(Quartz& q, const AttributedText& text, Range range, double width, double height);

struct ContextState;
// Draws a line / run at the context's text matrix (CTLineDraw / CTRunDraw);
// `backgrounds` also paints NSBackgroundColor runs (AppKit string drawing).
void draw_line_at(Quartz& q, ContextState& c, const LineLayout& line, bool backgrounds);
void draw_run_at(Quartz& q, ContextState& c, const LineLayout& line, const RunLayout& run, std::size_t first, std::size_t last);
[[nodiscard]] Address default_font(Quartz& q);   // the immortal Helvetica 12 (created on first use)

} // namespace alchemy::gui::quartz2d
