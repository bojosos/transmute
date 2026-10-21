// Null host backend, area "gpu": fonts and text layout. Every call fails
// with Error::unsupported (include/alchemy/host/host.hpp).
#include "alchemy/host/gpu.hpp"

namespace alchemy::host::gpu {

bool Font::same_face(const Font&) const { return false; }
Result<FontInfo> Font::info() const { return Error::unsupported; }
std::uint32_t Font::glyph_count() const { return 0; }
Result<std::vector<std::uint16_t>> Font::glyphs(std::span<const std::uint32_t>) const { return Error::unsupported; }
Result<std::vector<GlyphMetrics>> Font::glyph_metrics(std::span<const std::uint16_t>) const { return Error::unsupported; }
Result<std::vector<PathElement>> Font::outlines(double, std::span<const std::uint16_t>, std::span<const Point>, const Matrix&) const {
    return Error::unsupported;
}
Result<std::vector<std::uint8_t>> Font::table(std::uint32_t) const { return Error::unsupported; }
Result<FontCollection> system_fonts() { return Error::unsupported; }
FontCollection registered_fonts() { return {}; }
Result<Font> family_font(const FontCollection&, std::string_view, int, int, int) { return Error::unsupported; }
Result<Font> named_font(const FontCollection&, FontName, std::string_view) { return Error::unsupported; }
Result<std::vector<Font>> family_faces(const FontCollection&, std::string_view) { return Error::unsupported; }
Result<std::vector<std::string>> family_names(const FontCollection&) { return Error::unsupported; }
Status register_font_file(std::span<const std::uint8_t>) { return Error::unsupported; }
Result<Font> font_from_data(std::span<const std::uint8_t>) { return Error::unsupported; }
Result<Fallback> fallback_font(std::u16string_view, std::string_view, int, int, int) { return Error::unsupported; }
Result<TextLayout> layout_text(const TextLayoutRequest&) { return Error::unsupported; }

} // namespace alchemy::host::gpu
