#include "alchemy/darwin/coretext/ct_internal.hpp"

#include <array>
#include <mutex>
#include <unordered_map>

// Glyph names from the face's 'post' table (formats 1.0, 2.0 and 2.5, with
// the 258 standard Macintosh glyph names of the OpenType specification). A
// face whose 'post' table has no names (format 3.0, CFF fonts) has none.
namespace alchemy::gui::quartz2d {

struct GlyphNames {
    std::once_flag once;
    std::vector<std::string> names;                      // by glyph; empty: no name
    std::unordered_map<std::string, std::uint16_t> glyphs;   // the first glyph of each name
};

namespace {

constexpr std::array<std::string_view, 258> mac_names{
    ".notdef", ".null", "nonmarkingreturn", "space", "exclam", "quotedbl", "numbersign", "dollar", "percent", "ampersand",
    "quotesingle", "parenleft", "parenright", "asterisk", "plus", "comma", "hyphen", "period", "slash", "zero",
    "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "colon",
    "semicolon", "less", "equal", "greater", "question", "at", "A", "B", "C", "D",
    "E", "F", "G", "H", "I", "J", "K", "L", "M", "N",
    "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X",
    "Y", "Z", "bracketleft", "backslash", "bracketright", "asciicircum", "underscore", "grave", "a", "b",
    "c", "d", "e", "f", "g", "h", "i", "j", "k", "l",
    "m", "n", "o", "p", "q", "r", "s", "t", "u", "v",
    "w", "x", "y", "z", "braceleft", "bar", "braceright", "asciitilde", "Adieresis", "Aring",
    "Ccedilla", "Eacute", "Ntilde", "Odieresis", "Udieresis", "aacute", "agrave", "acircumflex", "adieresis", "atilde",
    "aring", "ccedilla", "eacute", "egrave", "ecircumflex", "edieresis", "iacute", "igrave", "icircumflex", "idieresis",
    "ntilde", "oacute", "ograve", "ocircumflex", "odieresis", "otilde", "uacute", "ugrave", "ucircumflex", "udieresis",
    "dagger", "degree", "cent", "sterling", "section", "bullet", "paragraph", "germandbls", "registered", "copyright",
    "trademark", "acute", "dieresis", "notequal", "AE", "Oslash", "infinity", "plusminus", "lessequal", "greaterequal",
    "yen", "mu", "partialdiff", "summation", "product", "pi", "integral", "ordfeminine", "ordmasculine", "Omega",
    "ae", "oslash", "questiondown", "exclamdown", "logicalnot", "radical", "florin", "approxequal", "Delta", "guillemotleft",
    "guillemotright", "ellipsis", "nonbreakingspace", "Agrave", "Atilde", "Otilde", "OE", "oe", "endash", "emdash",
    "quotedblleft", "quotedblright", "quoteleft", "quoteright", "divide", "lozenge", "ydieresis", "Ydieresis", "fraction", "currency",
    "guilsinglleft", "guilsinglright", "fi", "fl", "daggerdbl", "periodcentered", "quotesinglbase", "quotedblbase", "perthousand", "Acircumflex",
    "Ecircumflex", "Aacute", "Edieresis", "Egrave", "Iacute", "Icircumflex", "Idieresis", "Igrave", "Oacute", "Ocircumflex",
    "apple", "Ograve", "Uacute", "Ucircumflex", "Ugrave", "dotlessi", "circumflex", "tilde", "macron", "breve",
    "dotaccent", "ring", "cedilla", "hungarumlaut", "ogonek", "caron", "Lslash", "lslash", "Scaron", "scaron",
    "Zcaron", "zcaron", "brokenbar", "Eth", "eth", "Yacute", "yacute", "Thorn", "thorn", "minus",
    "multiply", "onesuperior", "twosuperior", "threesuperior", "onehalf", "onequarter", "threequarters", "franc", "Gbreve", "gbreve",
    "Idotaccent", "Scedilla", "scedilla", "Cacute", "cacute", "Ccaron", "ccaron", "dcroat"};

constexpr std::uint32_t post_tag = 0x706f7374;   // 'post'
constexpr std::size_t header_size = 32;

std::uint32_t be32(std::span<const std::uint8_t> bytes, std::size_t at) {
    return at + 4 <= bytes.size() ? (std::uint32_t{bytes[at]} << 24) | (std::uint32_t{bytes[at + 1]} << 16) | (std::uint32_t{bytes[at + 2]} << 8) | bytes[at + 3]
                                  : 0;
}
std::uint16_t be16(std::span<const std::uint8_t> bytes, std::size_t at) {
    return at + 2 <= bytes.size() ? static_cast<std::uint16_t>((bytes[at] << 8) | bytes[at + 1]) : 0;
}

std::vector<std::string> post_names(std::span<const std::uint8_t> post, std::size_t glyph_count) {
    std::vector<std::string> names;
    switch (be32(post, 0)) {
    case 0x00010000:   // the standard order
        for (std::size_t glyph = 0; glyph < std::min(glyph_count, mac_names.size()); ++glyph) names.emplace_back(mac_names[glyph]);
        break;
    case 0x00020000: {
        const std::size_t count = std::min<std::size_t>(be16(post, header_size), glyph_count);
        std::vector<std::string> extra;   // the Pascal strings after the index array
        for (std::size_t at = header_size + 2 + 2 * std::size_t{be16(post, header_size)}; at < post.size();) {
            const std::size_t length = post[at];
            if (at + 1 + length > post.size()) break;
            extra.emplace_back(reinterpret_cast<const char*>(post.data() + at + 1), length);
            at += 1 + length;
        }
        for (std::size_t glyph = 0; glyph < count; ++glyph) {
            const std::size_t index = be16(post, header_size + 2 + 2 * glyph);
            if (index < mac_names.size()) names.emplace_back(mac_names[index]);
            else names.push_back(index - mac_names.size() < extra.size() ? extra[index - mac_names.size()] : std::string());
        }
        break;
    }
    case 0x00025000: {   // offsets into the standard order
        const std::size_t count = std::min<std::size_t>(be16(post, header_size), glyph_count);
        for (std::size_t glyph = 0; glyph < count && header_size + 2 + glyph < post.size(); ++glyph) {
            const auto index = static_cast<std::ptrdiff_t>(glyph) + static_cast<std::int8_t>(post[header_size + 2 + glyph]);
            names.push_back(index >= 0 && static_cast<std::size_t>(index) < mac_names.size() ? std::string(mac_names[static_cast<std::size_t>(index)])
                                                                                               : std::string());
        }
        break;
    }
    default:   // 3.0: no names
        break;
    }
    return names;
}

const GlyphNames& names_of(const FontFace& face) {
    auto& names = *face.glyph_names;
    std::call_once(names.once, [&] {
        const auto post = face.font.table(post_tag);
        if (!post) return;
        names.names = post_names(*post, face.font.glyph_count());
        for (std::size_t glyph = 0; glyph < names.names.size(); ++glyph)
            if (!names.names[glyph].empty()) names.glyphs.emplace(names.names[glyph], static_cast<std::uint16_t>(glyph));
    });
    return names;
}

} // namespace

std::shared_ptr<GlyphNames> make_glyph_names() { return std::make_shared<GlyphNames>(); }

std::optional<std::string> glyph_name(const FontFace& face, std::uint16_t glyph) {
    const auto& names = names_of(face);
    if (glyph >= names.names.size() || names.names[glyph].empty()) return std::nullopt;
    return names.names[glyph];
}

std::uint16_t glyph_with_name(const FontFace& face, std::string_view name) {
    const auto& names = names_of(face);
    const auto found = names.glyphs.find(std::string(name));
    return found == names.glyphs.end() ? 0 : found->second;
}

} // namespace alchemy::gui::quartz2d
