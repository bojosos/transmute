#include "alchemy/darwin/coregraphics/pdf/pdf_render.hpp"

#include "alchemy/darwin/coregraphics/imageio/codec.hpp"
#include "alchemy/darwin/coretext/ct_internal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// The page renderer (CGContextDrawPDFPage), written from the PDF
// specification (ISO 32000-1). Drawn:
// * graphics state: q / Q, cm, line width, cap, join, miter limit, dashes,
//   ExtGState (line parameters, constant alpha /CA /ca, blend modes, fonts);
// * paths: construction, fill (nonzero, even-odd), stroke, both, clipping;
// * colour: DeviceGray / RGB / CMYK, ICCBased (by component count, through
//   its alternate when it has one), CalGray / CalRGB, Lab, Indexed,
//   Separation and DeviceN (through their tint transforms), shading
//   patterns;
// * functions: sampled, exponential, stitching and PostScript calculator;
// * shadings: axial and radial (sh, and shading patterns);
// * images: XObjects and inline images (1 to 16 bits per component, Decode
//   arrays, JPEG through ImageIO, stencil masks, /SMask, /Mask streams and
//   colour-key masks);
// * form XObjects (matrix, bounding-box clip, their resources);
// * text: Tc Tw Tz TL Ts Tr, positioning, Tj TJ ' ", render modes; fonts:
//   embedded TrueType and OpenType programs (FontFile2, FontFile3
//   /OpenType), simple fonts through their encodings (WinAnsi, MacRoman,
//   Standard, /Differences with glyph names), composite fonts (Identity CIDs,
//   CIDToGIDMap, /W widths, ToUnicode), Type 3 fonts (glyph procedures);
//   other fonts (Type 1 and bare CFF programs, standard 14 fonts) use the
//   system font their name maps to, through Unicode.
// Not drawn (a diagnostic says so once per page): tiling patterns, shading
// types 1 and 4 to 7, soft masks in ExtGState, JPEG 2000 images, CCITT and
// JBIG2 images. Annotations are not drawn (the page content only).
namespace alchemy::gui::quartz2d {
namespace {

using pdf::Kind;
using pdf::ObjectPtr;

// ---- glyph names and encodings --------------------------------------------------------------------------
constexpr std::array<const char*, 95> ascii_names{
    "space", "exclam", "quotedbl", "numbersign", "dollar", "percent", "ampersand", "quotesingle", "parenleft", "parenright",
    "asterisk", "plus", "comma", "hyphen", "period", "slash", "zero", "one", "two", "three", "four", "five", "six", "seven",
    "eight", "nine", "colon", "semicolon", "less", "equal", "greater", "question", "at", "A", "B", "C", "D", "E", "F", "G", "H",
    "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "bracketleft", "backslash",
    "bracketright", "asciicircum", "underscore", "grave", "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n",
    "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z", "braceleft", "bar", "braceright", "asciitilde"};
// Latin-1 0xA1-0xFF.
constexpr std::array<const char*, 95> latin_names{
    "exclamdown", "cent", "sterling", "currency", "yen", "brokenbar", "section", "dieresis", "copyright", "ordfeminine",
    "guillemotleft", "logicalnot", "uni00AD", "registered", "macron", "degree", "plusminus", "twosuperior", "threesuperior",
    "acute", "mu", "paragraph", "periodcentered", "cedilla", "onesuperior", "ordmasculine", "guillemotright", "onequarter",
    "onehalf", "threequarters", "questiondown", "Agrave", "Aacute", "Acircumflex", "Atilde", "Adieresis", "Aring", "AE",
    "Ccedilla", "Egrave", "Eacute", "Ecircumflex", "Edieresis", "Igrave", "Iacute", "Icircumflex", "Idieresis", "Eth", "Ntilde",
    "Ograve", "Oacute", "Ocircumflex", "Otilde", "Odieresis", "multiply", "Oslash", "Ugrave", "Uacute", "Ucircumflex",
    "Udieresis", "Yacute", "Thorn", "germandbls", "agrave", "aacute", "acircumflex", "atilde", "adieresis", "aring", "ae",
    "ccedilla", "egrave", "eacute", "ecircumflex", "edieresis", "igrave", "iacute", "icircumflex", "idieresis", "eth", "ntilde",
    "ograve", "oacute", "ocircumflex", "otilde", "odieresis", "divide", "oslash", "ugrave", "uacute", "ucircumflex", "udieresis",
    "yacute", "thorn", "ydieresis"};
constexpr std::array<std::pair<const char*, char16_t>, 46> special_names{{
    {"bullet", 0x2022}, {"endash", 0x2013}, {"emdash", 0x2014}, {"quoteleft", 0x2018}, {"quoteright", 0x2019},
    {"quotedblleft", 0x201c}, {"quotedblright", 0x201d}, {"quotesinglbase", 0x201a}, {"quotedblbase", 0x201e},
    {"ellipsis", 0x2026}, {"dagger", 0x2020}, {"daggerdbl", 0x2021}, {"perthousand", 0x2030}, {"guilsinglleft", 0x2039},
    {"guilsinglright", 0x203a}, {"fi", 0xfb01}, {"fl", 0xfb02}, {"Euro", 0x20ac}, {"trademark", 0x2122}, {"florin", 0x0192},
    {"circumflex", 0x02c6}, {"tilde", 0x02dc}, {"OE", 0x0152}, {"oe", 0x0153}, {"Scaron", 0x0160}, {"scaron", 0x0161},
    {"Zcaron", 0x017d}, {"zcaron", 0x017e}, {"Ydieresis", 0x0178}, {"dotlessi", 0x0131}, {"minus", 0x2212}, {"fraction", 0x2044},
    {"Lslash", 0x0141}, {"lslash", 0x0142}, {"breve", 0x02d8}, {"dotaccent", 0x02d9}, {"ring", 0x02da}, {"ogonek", 0x02db},
    {"hungarumlaut", 0x02dd}, {"caron", 0x02c7}, {"nbspace", 0x00a0}, {"space", 0x0020}, {"nonbreakingspace", 0x00a0},
    {"sfthyphen", 0x00ad}, {"periodcentered", 0x00b7}, {"middot", 0x00b7}}};

std::optional<char16_t> unicode_for_name(std::string_view name) {
    static const auto table = [] {
        std::unordered_map<std::string, char16_t> out;
        for (std::size_t i = 0; i < ascii_names.size(); ++i) out.emplace(ascii_names[i], static_cast<char16_t>(0x20 + i));
        for (std::size_t i = 0; i < latin_names.size(); ++i) out.emplace(latin_names[i], static_cast<char16_t>(0xa1 + i));
        for (const auto& [n, u] : special_names) out.emplace(n, u);
        return out;
    }();
    if (const auto dot = name.find('.'); dot != std::string_view::npos && dot > 0) name = name.substr(0, dot);   // "a.sc"
    if (const auto found = table.find(std::string(name)); found != table.end()) return found->second;
    const auto hex = [](std::string_view text) -> std::optional<char16_t> {
        if (text.empty() || text.size() > 4) return std::nullopt;
        unsigned value = 0;
        for (const auto c : text) {
            const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (d < 0) return std::nullopt;
            value = value * 16 + static_cast<unsigned>(d);
        }
        return static_cast<char16_t>(value);
    };
    if (name.starts_with("uni") && name.size() == 7) return hex(name.substr(3));
    if (name.starts_with("u") && name.size() == 5) return hex(name.substr(1));
    return std::nullopt;
}

char16_t win_ansi(std::uint8_t code) {
    static constexpr std::array<char16_t, 32> high{0x20ac, 0, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160,
                                                   0x2039, 0x0152, 0, 0x017d, 0, 0, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013,
                                                   0x2014, 0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0, 0x017e, 0x0178};
    if (code >= 0x80 && code < 0xa0) return high[code - 0x80];
    return code;
}
char16_t mac_roman(std::uint8_t code) {
    static constexpr std::array<char16_t, 128> high{
        0x00c4, 0x00c5, 0x00c7, 0x00c9, 0x00d1, 0x00d6, 0x00dc, 0x00e1, 0x00e0, 0x00e2, 0x00e4, 0x00e3, 0x00e5, 0x00e7, 0x00e9, 0x00e8,
        0x00ea, 0x00eb, 0x00ed, 0x00ec, 0x00ee, 0x00ef, 0x00f1, 0x00f3, 0x00f2, 0x00f4, 0x00f6, 0x00f5, 0x00fa, 0x00f9, 0x00fb, 0x00fc,
        0x2020, 0x00b0, 0x00a2, 0x00a3, 0x00a7, 0x2022, 0x00b6, 0x00df, 0x00ae, 0x00a9, 0x2122, 0x00b4, 0x00a8, 0x2260, 0x00c6, 0x00d8,
        0x221e, 0x00b1, 0x2264, 0x2265, 0x00a5, 0x00b5, 0x2202, 0x2211, 0x220f, 0x03c0, 0x222b, 0x00aa, 0x00ba, 0x03a9, 0x00e6, 0x00f8,
        0x00bf, 0x00a1, 0x00ac, 0x221a, 0x0192, 0x2248, 0x2206, 0x00ab, 0x00bb, 0x2026, 0x00a0, 0x00c0, 0x00c3, 0x00d5, 0x0152, 0x0153,
        0x2013, 0x2014, 0x201c, 0x201d, 0x2018, 0x2019, 0x00f7, 0x25ca, 0x00ff, 0x0178, 0x2044, 0x20ac, 0x2039, 0x203a, 0xfb01, 0xfb02,
        0x2021, 0x00b7, 0x201a, 0x201e, 0x2030, 0x00c2, 0x00ca, 0x00c1, 0x00cb, 0x00c8, 0x00cd, 0x00ce, 0x00cf, 0x00cc, 0x00d3, 0x00d4,
        0xf8ff, 0x00d2, 0x00da, 0x00db, 0x00d9, 0x0131, 0x02c6, 0x02dc, 0x00af, 0x02d8, 0x02d9, 0x02da, 0x00b8, 0x02dd, 0x02db, 0x02c7};
    return code < 0x80 ? code : high[code - 0x80];
}
char16_t standard(std::uint8_t code) {
    if (code == 0x27) return 0x2019;
    if (code == 0x60) return 0x2018;
    if (code < 0x80) return code;
    static const std::map<std::uint8_t, const char*> high{
        {0xa1, "exclamdown"}, {0xa2, "cent"}, {0xa3, "sterling"}, {0xa4, "fraction"}, {0xa5, "yen"}, {0xa6, "florin"},
        {0xa7, "section"}, {0xa8, "currency"}, {0xa9, "quotesingle"}, {0xaa, "quotedblleft"}, {0xab, "guillemotleft"},
        {0xac, "guilsinglleft"}, {0xad, "guilsinglright"}, {0xae, "fi"}, {0xaf, "fl"}, {0xb1, "endash"}, {0xb2, "dagger"},
        {0xb3, "daggerdbl"}, {0xb4, "periodcentered"}, {0xb6, "paragraph"}, {0xb7, "bullet"}, {0xb8, "quotesinglbase"},
        {0xb9, "quotedblbase"}, {0xba, "quotedblright"}, {0xbb, "guillemotright"}, {0xbc, "ellipsis"}, {0xbd, "perthousand"},
        {0xbf, "questiondown"}, {0xc1, "grave"}, {0xc2, "acute"}, {0xc3, "circumflex"}, {0xc4, "tilde"}, {0xc5, "macron"},
        {0xc6, "breve"}, {0xc7, "dotaccent"}, {0xc8, "dieresis"}, {0xca, "ring"}, {0xcb, "cedilla"}, {0xcd, "hungarumlaut"},
        {0xce, "ogonek"}, {0xcf, "caron"}, {0xd0, "emdash"}, {0xe1, "AE"}, {0xe3, "ordfeminine"}, {0xe8, "Lslash"}, {0xe9, "Oslash"},
        {0xea, "OE"}, {0xeb, "ordmasculine"}, {0xf1, "ae"}, {0xf5, "dotlessi"}, {0xf8, "lslash"}, {0xf9, "oslash"}, {0xfa, "oe"},
        {0xfb, "germandbls"}};
    const auto found = high.find(code);
    return found == high.end() ? 0 : unicode_for_name(found->second).value_or(0);
}

// ---- functions ---------------------------------------------------------------------------------------------
struct Function {
    virtual ~Function() = default;
    [[nodiscard]] virtual std::vector<double> eval(std::vector<double> in) const = 0;
    std::vector<double> domain, range;
    [[nodiscard]] std::vector<double> clip_in(std::vector<double> in) const {
        for (std::size_t i = 0; i < in.size() && 2 * i + 1 < domain.size(); ++i) in[i] = std::clamp(in[i], domain[2 * i], domain[2 * i + 1]);
        return in;
    }
    [[nodiscard]] std::vector<double> clip_out(std::vector<double> out) const {
        for (std::size_t i = 0; i < out.size() && 2 * i + 1 < range.size(); ++i) out[i] = std::clamp(out[i], range[2 * i], range[2 * i + 1]);
        return out;
    }
};
using FunctionPtr = std::shared_ptr<const Function>;

std::vector<double> numbers_of(pdf::Document& doc, const ObjectPtr& object) {
    std::vector<double> out;
    const auto array = doc.resolve(object);
    if (!array || array->kind != Kind::array) return out;
    for (const auto& item : array->array) {
        const auto value = doc.resolve(item);
        out.push_back(value && value->is_number() ? value->number_value() : 0);
    }
    return out;
}

struct SampledFunction final : Function {
    std::vector<int> size;
    int bits{8};
    std::vector<double> encode, decode, samples;   // samples normalized to 0..1
    std::size_t outputs{};
    std::vector<double> eval(std::vector<double> in) const override {
        in = clip_in(std::move(in));
        // Multilinear interpolation over the sample grid.
        const auto m = size.size();
        std::vector<double> e(m);
        for (std::size_t i = 0; i < m; ++i) {
            const double d0 = domain[2 * i], d1 = domain[2 * i + 1];
            const double e0 = encode.size() > 2 * i ? encode[2 * i] : 0, e1 = encode.size() > 2 * i + 1 ? encode[2 * i + 1] : size[i] - 1;
            e[i] = std::clamp(d1 == d0 ? e0 : e0 + (in[i] - d0) * (e1 - e0) / (d1 - d0), 0.0, static_cast<double>(size[i] - 1));
        }
        std::vector<double> out(outputs, 0);
        const std::size_t corners = std::size_t{1} << std::min<std::size_t>(m, 10);
        for (std::size_t corner = 0; corner < corners; ++corner) {
            double weight = 1;
            std::size_t index = 0, stride = 1;
            for (std::size_t i = 0; i < m; ++i) {
                const auto low = static_cast<std::size_t>(std::floor(e[i]));
                const auto high = std::min<std::size_t>(low + 1, static_cast<std::size_t>(size[i] - 1));
                const double t = e[i] - static_cast<double>(low);
                const bool up = (corner >> i) & 1;
                weight *= up ? t : 1 - t;
                index += (up ? high : low) * stride;
                stride *= static_cast<std::size_t>(size[i]);
            }
            if (weight == 0) continue;
            for (std::size_t j = 0; j < outputs; ++j) {
                const auto at = index * outputs + j;
                out[j] += weight * (at < samples.size() ? samples[at] : 0);
            }
        }
        for (std::size_t j = 0; j < outputs; ++j) {
            const double r0 = decode.size() > 2 * j ? decode[2 * j] : range[2 * j], r1 = decode.size() > 2 * j + 1 ? decode[2 * j + 1] : range[2 * j + 1];
            out[j] = r0 + out[j] * (r1 - r0);
        }
        return clip_out(std::move(out));
    }
};

struct ExponentialFunction final : Function {
    std::vector<double> c0{0}, c1{1};
    double n{1};
    std::vector<double> eval(std::vector<double> in) const override {
        in = clip_in(std::move(in));
        const double x = in.empty() ? 0 : in[0];
        std::vector<double> out(std::max(c0.size(), c1.size()));
        const double p = std::pow(std::max(x, 0.0), n);
        for (std::size_t i = 0; i < out.size(); ++i) {
            const double a = i < c0.size() ? c0[i] : 0, b = i < c1.size() ? c1[i] : 1;
            out[i] = a + p * (b - a);
        }
        return clip_out(std::move(out));
    }
};

struct StitchingFunction final : Function {
    std::vector<FunctionPtr> functions;
    std::vector<double> bounds, encode;
    std::vector<double> eval(std::vector<double> in) const override {
        in = clip_in(std::move(in));
        if (functions.empty()) return {};
        const double x = in.empty() ? 0 : in[0];
        std::size_t k = 0;
        while (k < bounds.size() && x >= bounds[k]) ++k;
        k = std::min(k, functions.size() - 1);
        const double low = k == 0 ? domain[0] : bounds[k - 1], high = k < bounds.size() ? bounds[k] : domain[1];
        const double e0 = encode.size() > 2 * k ? encode[2 * k] : 0, e1 = encode.size() > 2 * k + 1 ? encode[2 * k + 1] : 1;
        const double t = high == low ? e0 : e0 + (x - low) * (e1 - e0) / (high - low);
        return clip_out(functions[k]->eval({t}));
    }
};

// PostScript calculator functions (type 4).
struct PostScriptFunction final : Function {
    struct Op {
        std::string name;          // an operator, or "" for a number, or "{" for a procedure body
        double value{};
        std::vector<Op> body, alternative;   // if / ifelse procedures
    };
    std::vector<Op> program;
    std::vector<double> eval(std::vector<double> in) const override {
        std::vector<double> stack = clip_in(std::move(in));
        run(program, stack, 0);
        const auto outputs = range.size() / 2;
        std::vector<double> out(outputs, 0);
        for (std::size_t i = 0; i < outputs && i < stack.size(); ++i) out[outputs - 1 - i] = stack[stack.size() - 1 - i];
        return clip_out(std::move(out));
    }
    static void run(const std::vector<Op>& ops, std::vector<double>& s, int depth) {
        if (depth > 64) return;
        const auto pop = [&s] { if (s.empty()) return 0.0; const auto v = s.back(); s.pop_back(); return v; };
        const auto as_int = [](double v) { return static_cast<std::int64_t>(v); };
        for (const auto& op : ops) {
            const auto& n = op.name;
            if (n.empty()) { s.push_back(op.value); continue; }
            if (n == "if") { if (pop() != 0) run(op.body, s, depth + 1); continue; }
            if (n == "ifelse") { run(pop() != 0 ? op.body : op.alternative, s, depth + 1); continue; }
            if (n == "add") { const auto b = pop(), a = pop(); s.push_back(a + b); }
            else if (n == "sub") { const auto b = pop(), a = pop(); s.push_back(a - b); }
            else if (n == "mul") { const auto b = pop(), a = pop(); s.push_back(a * b); }
            else if (n == "div") { const auto b = pop(), a = pop(); s.push_back(b == 0 ? 0 : a / b); }
            else if (n == "idiv") { const auto b = as_int(pop()), a = as_int(pop()); s.push_back(b == 0 ? 0 : static_cast<double>(a / b)); }
            else if (n == "mod") { const auto b = as_int(pop()), a = as_int(pop()); s.push_back(b == 0 ? 0 : static_cast<double>(a % b)); }
            else if (n == "neg") s.push_back(-pop());
            else if (n == "abs") s.push_back(std::abs(pop()));
            else if (n == "ceiling") s.push_back(std::ceil(pop()));
            else if (n == "floor") s.push_back(std::floor(pop()));
            else if (n == "round") s.push_back(std::floor(pop() + 0.5));
            else if (n == "truncate" || n == "cvi") s.push_back(std::trunc(pop()));
            else if (n == "cvr") {}
            else if (n == "sqrt") s.push_back(std::sqrt(std::max(0.0, pop())));
            else if (n == "sin") s.push_back(std::sin(pop() * 3.14159265358979323846 / 180));
            else if (n == "cos") s.push_back(std::cos(pop() * 3.14159265358979323846 / 180));
            else if (n == "atan") {
                const auto den = pop(), num = pop();
                auto a = std::atan2(num, den) * 180 / 3.14159265358979323846;
                s.push_back(a < 0 ? a + 360 : a);
            }
            else if (n == "exp") { const auto e = pop(), b = pop(); s.push_back(std::pow(b, e)); }
            else if (n == "ln") s.push_back(std::log(std::max(1e-300, pop())));
            else if (n == "log") s.push_back(std::log10(std::max(1e-300, pop())));
            else if (n == "eq") { const auto b = pop(), a = pop(); s.push_back(a == b); }
            else if (n == "ne") { const auto b = pop(), a = pop(); s.push_back(a != b); }
            else if (n == "gt") { const auto b = pop(), a = pop(); s.push_back(a > b); }
            else if (n == "ge") { const auto b = pop(), a = pop(); s.push_back(a >= b); }
            else if (n == "lt") { const auto b = pop(), a = pop(); s.push_back(a < b); }
            else if (n == "le") { const auto b = pop(), a = pop(); s.push_back(a <= b); }
            else if (n == "and") { const auto b = as_int(pop()), a = as_int(pop()); s.push_back(static_cast<double>(a & b)); }
            else if (n == "or") { const auto b = as_int(pop()), a = as_int(pop()); s.push_back(static_cast<double>(a | b)); }
            else if (n == "xor") { const auto b = as_int(pop()), a = as_int(pop()); s.push_back(static_cast<double>(a ^ b)); }
            else if (n == "not") { const auto a = pop(); s.push_back(a == 0 || a == 1 ? (a == 0 ? 1.0 : 0.0) : static_cast<double>(~as_int(a))); }
            else if (n == "bitshift") { const auto shift = as_int(pop()), a = as_int(pop()); s.push_back(static_cast<double>(shift >= 0 ? a << shift : a >> -shift)); }
            else if (n == "true") s.push_back(1);
            else if (n == "false") s.push_back(0);
            else if (n == "pop") pop();
            else if (n == "dup") { const auto a = pop(); s.push_back(a); s.push_back(a); }
            else if (n == "exch") { const auto b = pop(), a = pop(); s.push_back(b); s.push_back(a); }
            else if (n == "copy") {
                const auto count = static_cast<std::size_t>(std::max<std::int64_t>(0, as_int(pop())));
                if (count <= s.size()) for (std::size_t i = s.size() - count, end = s.size(); i < end; ++i) s.push_back(s[i]);
            }
            else if (n == "index") {
                const auto i = static_cast<std::size_t>(std::max<std::int64_t>(0, as_int(pop())));
                s.push_back(i < s.size() ? s[s.size() - 1 - i] : 0);
            }
            else if (n == "roll") {
                const auto j = as_int(pop());
                const auto count = static_cast<std::size_t>(std::max<std::int64_t>(0, as_int(pop())));
                if (count > 0 && count <= s.size()) {
                    const auto shift = ((j % static_cast<std::int64_t>(count)) + static_cast<std::int64_t>(count)) % static_cast<std::int64_t>(count);
                    std::rotate(s.end() - static_cast<std::ptrdiff_t>(count), s.end() - static_cast<std::ptrdiff_t>(shift), s.end());
                }
            }
        }
    }
    static std::vector<Op> parse(pdf::Lexer& lexer, int depth) {
        std::vector<Op> out;
        while (depth < 64) {
            const auto token = lexer.next(false);
            if (!token) break;
            const auto& t = **token;
            if (t.kind == Kind::keyword && t.text == "}") break;
            if (t.kind == Kind::keyword && t.text == "{") {
                Op procedure;
                procedure.name = "{";
                procedure.body = parse(lexer, depth + 1);
                out.push_back(std::move(procedure));
                continue;
            }
            if (t.is_number()) { Op number; number.value = t.number_value(); out.push_back(std::move(number)); continue; }
            if (t.kind == Kind::boolean) { Op number; number.value = t.boolean ? 1 : 0; out.push_back(std::move(number)); continue; }
            if (t.kind != Kind::keyword) continue;
            Op op;
            op.name = t.text;
            if ((op.name == "if" || op.name == "ifelse") && !out.empty() && out.back().name == "{") {
                if (op.name == "ifelse" && out.size() >= 2 && out[out.size() - 2].name == "{") {
                    op.alternative = std::move(out.back().body);
                    out.pop_back();
                    op.body = std::move(out.back().body);
                    out.pop_back();
                } else {
                    op.body = std::move(out.back().body);
                    out.pop_back();
                }
            }
            out.push_back(std::move(op));
        }
        return out;
    }
};

FunctionPtr make_function(pdf::Document& doc, const ObjectPtr& source, int depth = 0) {
    const auto object = doc.resolve(source);
    if (!object || depth > 8) return nullptr;
    if (object->kind == Kind::array) {
        // An array of 1-output functions: one function with their outputs.
        struct Combined final : Function {
            std::vector<FunctionPtr> parts;
            std::vector<double> eval(std::vector<double> in) const override {
                std::vector<double> out;
                for (const auto& part : parts) {
                    const auto value = part->eval(in);
                    out.push_back(value.empty() ? 0 : value[0]);
                }
                return out;
            }
        };
        auto combined = std::make_shared<Combined>();
        for (const auto& item : object->array)
            if (auto part = make_function(doc, item, depth + 1)) combined->parts.push_back(std::move(part));
        return combined;
    }
    const auto type = doc.get(object, "FunctionType");
    if (!type || type->kind != Kind::integer) return nullptr;
    std::shared_ptr<Function> made;
    switch (type->integer) {
    case 0: {
        auto f = std::make_shared<SampledFunction>();
        for (const auto value : numbers_of(doc, object->dictionary.find("Size"))) f->size.push_back(std::max(1, static_cast<int>(value)));
        if (const auto bits = doc.get(object, "BitsPerSample"); bits && bits->kind == Kind::integer) f->bits = static_cast<int>(bits->integer);
        f->encode = numbers_of(doc, object->dictionary.find("Encode"));
        f->decode = numbers_of(doc, object->dictionary.find("Decode"));
        f->range = numbers_of(doc, object->dictionary.find("Range"));
        f->outputs = f->range.size() / 2;
        if (object->kind == Kind::stream && f->bits > 0 && f->bits <= 32) {
            const auto data = doc.stream_data(*object);
            if (data) {
                std::uint64_t buffer = 0;
                int have = 0;
                const double max = std::pow(2.0, f->bits) - 1;
                for (const auto byte : *data) {
                    buffer = (buffer << 8) | byte;
                    have += 8;
                    while (have >= f->bits) {
                        f->samples.push_back(static_cast<double>((buffer >> (have - f->bits)) & ((std::uint64_t{1} << f->bits) - 1)) / max);
                        have -= f->bits;
                    }
                }
            }
        }
        made = f;
        break;
    }
    case 2: {
        auto f = std::make_shared<ExponentialFunction>();
        if (const auto c0 = numbers_of(doc, object->dictionary.find("C0")); !c0.empty()) f->c0 = c0;
        if (const auto c1 = numbers_of(doc, object->dictionary.find("C1")); !c1.empty()) f->c1 = c1;
        if (const auto n = doc.get(object, "N"); n && n->is_number()) f->n = n->number_value();
        made = f;
        break;
    }
    case 3: {
        auto f = std::make_shared<StitchingFunction>();
        if (const auto parts = doc.get(object, "Functions"); parts && parts->kind == Kind::array)
            for (const auto& item : parts->array) if (auto part = make_function(doc, item, depth + 1)) f->functions.push_back(std::move(part));
        f->bounds = numbers_of(doc, object->dictionary.find("Bounds"));
        f->encode = numbers_of(doc, object->dictionary.find("Encode"));
        made = f;
        break;
    }
    case 4: {
        auto f = std::make_shared<PostScriptFunction>();
        if (object->kind == Kind::stream)
            if (const auto data = doc.stream_data(*object)) {
                pdf::Lexer lexer(*data);
                lexer.skip_space();
                if (const auto open = lexer.next(false); open && (*open)->kind == Kind::keyword && (*open)->text == "{")
                    f->program = PostScriptFunction::parse(lexer, 0);
            }
        made = f;
        break;
    }
    default: return nullptr;
    }
    made->domain = numbers_of(doc, object->dictionary.find("Domain"));
    if (made->range.empty()) made->range = numbers_of(doc, object->dictionary.find("Range"));
    return made;
}

// ---- colour spaces -----------------------------------------------------------------------------------------
struct ColorSpace {
    enum class Type { gray, rgb, cmyk, lab, indexed, tinted, pattern } type{Type::gray};
    std::size_t components{1};
    std::shared_ptr<const ColorSpace> base;   // indexed, tinted (alternate), pattern (underlying, uncoloured patterns)
    std::vector<std::uint8_t> lookup;
    int hival{};
    FunctionPtr tint;
    std::array<double, 3> white{0.9505, 1, 1.089};
    std::vector<double> lab_range{-100, 100, -100, 100};

    [[nodiscard]] Rgba to_rgb(std::span<const double> c) const {
        const auto at = [&](std::size_t i) { return i < c.size() ? c[i] : 0.0; };
        switch (type) {
        case Type::gray: return {at(0), at(0), at(0), 1};
        case Type::rgb: return {at(0), at(1), at(2), 1};
        case Type::cmyk: {
            const auto k = at(3);
            return {(1 - at(0)) * (1 - k), (1 - at(1)) * (1 - k), (1 - at(2)) * (1 - k), 1};
        }
        case Type::lab: {
            // CIE L*a*b* to XYZ (the space's white point) to linear sRGB, then the sRGB curve.
            const auto L = at(0), a = std::clamp(at(1), lab_range[0], lab_range[1]), b = std::clamp(at(2), lab_range[2], lab_range[3]);
            const auto fy = (L + 16) / 116, fx = fy + a / 500, fz = fy - b / 200;
            const auto g = [](double t) { return t > 6.0 / 29 ? t * t * t : 3 * (6.0 / 29) * (6.0 / 29) * (t - 4.0 / 29); };
            const auto X = white[0] * g(fx), Y = white[1] * g(fy), Z = white[2] * g(fz);
            const auto curve = [](double v) {
                v = std::clamp(v, 0.0, 1.0);
                return v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1 / 2.4) - 0.055;
            };
            return {curve(3.2406 * X - 1.5372 * Y - 0.4986 * Z), curve(-0.9689 * X + 1.8758 * Y + 0.0415 * Z),
                    curve(0.0557 * X - 0.2040 * Y + 1.0570 * Z), 1};
        }
        case Type::indexed: {
            if (!base) return {0, 0, 0, 1};
            const auto index = static_cast<std::size_t>(std::clamp(static_cast<int>(std::lround(at(0))), 0, hival));
            std::vector<double> values(base->components);
            for (std::size_t i = 0; i < values.size(); ++i) {
                const auto offset = index * base->components + i;
                values[i] = offset < lookup.size() ? lookup[offset] / 255.0 : 0;
            }
            return base->to_rgb(values);
        }
        case Type::tinted: {
            if (!base || !tint) {
                const auto t = c.empty() ? 1.0 : at(0);
                return {1 - t, 1 - t, 1 - t, 1};
            }
            return base->to_rgb(tint->eval(std::vector<double>(c.begin(), c.end())));
        }
        case Type::pattern: return base ? base->to_rgb(c) : Rgba{0, 0, 0, 1};
        }
        return {0, 0, 0, 1};
    }
    [[nodiscard]] std::vector<double> initial() const {
        if (type == Type::cmyk) return {0, 0, 0, 1};
        if (type == Type::tinted) return std::vector<double>(components, 1);
        if (type == Type::lab) return {0, 0, 0};
        return std::vector<double>(components, 0);
    }
};
using ColorSpacePtr = std::shared_ptr<const ColorSpace>;

ColorSpacePtr device(ColorSpace::Type type, std::size_t components) {
    auto made = std::make_shared<ColorSpace>();
    made->type = type;
    made->components = components;
    return made;
}

// ---- fonts -------------------------------------------------------------------------------------------------
struct Font {
    enum class Kind { simple, composite, type3 } kind{Kind::simple};
    std::shared_ptr<FontFace> face;                 // nothing for Type 3 (or when no face resolves)
    bool embedded{};                                // the face is the PDF's own program
    std::array<std::uint16_t, 256> glyphs{};        // simple: code -> glyph
    std::array<double, 256> widths{};               // simple: 1/1000 em (Type 3: glyph space)
    std::array<bool, 256> has_width{};
    // composite
    std::vector<std::uint16_t> cid_to_gid;
    bool identity_gid{true};
    std::map<std::uint32_t, double> cid_widths;
    double default_width{1000};
    std::map<std::uint32_t, std::u16string> to_unicode;
    // Type 3
    Affine font_matrix{0.001, 0, 0, 0.001, 0, 0};
    ObjectPtr char_procs, resources;
    std::array<std::string, 256> names;
};
using FontPtr = std::shared_ptr<Font>;

// ToUnicode CMaps: bfchar and bfrange entries.
std::map<std::uint32_t, std::u16string> parse_to_unicode(std::span<const std::uint8_t> data) {
    std::map<std::uint32_t, std::u16string> out;
    pdf::Lexer lexer(data);
    const auto code_of = [](const pdf::Object& o) {
        std::uint32_t value = 0;
        for (const auto c : o.text) value = (value << 8) | static_cast<unsigned char>(c);
        return value;
    };
    const auto units_of = [](const pdf::Object& o) {
        std::u16string units;
        for (std::size_t i = 0; i + 1 < o.text.size(); i += 2)
            units.push_back(static_cast<char16_t>((static_cast<unsigned char>(o.text[i]) << 8) | static_cast<unsigned char>(o.text[i + 1])));
        if (o.text.size() == 1) units.push_back(static_cast<unsigned char>(o.text[0]));
        return units;
    };
    std::vector<ObjectPtr> operands;
    while (auto token = lexer.next(false)) {
        const auto& t = **token;
        if (t.kind != Kind::keyword) { operands.push_back(*token); continue; }
        if (t.text == "endbfchar") {
            for (std::size_t i = 0; i + 1 < operands.size(); i += 2)
                if (operands[i]->kind == Kind::string && operands[i + 1]->kind == Kind::string) out[code_of(*operands[i])] = units_of(*operands[i + 1]);
        } else if (t.text == "endbfrange") {
            for (std::size_t i = 0; i + 2 < operands.size(); i += 3) {
                const auto& low = *operands[i];
                const auto& high = *operands[i + 1];
                const auto& target = *operands[i + 2];
                if (low.kind != Kind::string || high.kind != Kind::string) continue;
                const auto first = code_of(low), last = std::min(code_of(high), first + 0xffff);
                if (target.kind == Kind::string) {
                    auto units = units_of(target);
                    for (auto code = first; code <= last && !units.empty(); ++code) {
                        out[code] = units;
                        ++units.back();
                    }
                } else if (target.kind == Kind::array) {
                    for (std::size_t k = 0; k < target.array.size() && first + k <= last; ++k)
                        if (target.array[k]->kind == Kind::string) out[first + static_cast<std::uint32_t>(k)] = units_of(*target.array[k]);
                }
            }
        }
        operands.clear();
    }
    return out;
}

std::shared_ptr<FontFace> embedded_face(pdf::Document& doc, const ObjectPtr& descriptor) {
    if (!descriptor) return nullptr;
    ObjectPtr program = doc.get(descriptor, "FontFile2");
    if (!program) {
        const auto file3 = doc.get(descriptor, "FontFile3");
        const auto subtype = doc.get(file3, "Subtype");
        if (file3 && subtype && subtype->kind == Kind::name && subtype->text == "OpenType") program = file3;
    }
    if (!program || program->kind != Kind::stream) return nullptr;
    const auto data = doc.stream_data(*program);
    if (!data || data->empty()) return nullptr;
    const auto font = host::gpu::font_from_data(*data);
    if (!font) return nullptr;
    return face_from(*font);
}

// The system face a font name maps to (subset prefix dropped, PostScript style suffixes kept).
std::shared_ptr<FontFace> system_face(std::string name) {
    if (name.size() > 7 && name[6] == '+') name = name.substr(7);
    for (auto& c : name) if (c == ',') c = '-';
    if (const auto resolved = resolve_font(name)) return resolved->face;
    std::string lower;
    for (const auto c : name) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    const bool bold = lower.find("bold") != std::string::npos || lower.find("black") != std::string::npos || lower.find("heavy") != std::string::npos;
    const bool italic = lower.find("italic") != std::string::npos || lower.find("oblique") != std::string::npos;
    std::string family = lower.find("times") != std::string::npos || lower.find("serif") != std::string::npos ? "Times"
                         : lower.find("courier") != std::string::npos || lower.find("mono") != std::string::npos ? "Courier"
                                                                                                                 : "Helvetica";
    std::string fallback = family;
    if (family == "Times") fallback = bold && italic ? "Times-BoldItalic" : bold ? "Times-Bold" : italic ? "Times-Italic" : "Times-Roman";
    else fallback = family + (bold && italic ? "-BoldOblique" : bold ? "-Bold" : italic ? "-Oblique" : "");
    if (const auto resolved = resolve_font(fallback)) return resolved->face;
    if (const auto resolved = resolve_font("Helvetica")) return resolved->face;
    return nullptr;
}

FontPtr load_font(pdf::Document& doc, const ObjectPtr& dictionary) {
    auto font = std::make_shared<Font>();
    const auto subtype = doc.get(dictionary, "Subtype");
    const std::string kind = subtype && subtype->kind == Kind::name ? subtype->text : "Type1";
    const auto base_name = doc.get(dictionary, "BaseFont");
    const std::string base = base_name && base_name->kind == Kind::name ? base_name->text : "";
    if (const auto map = doc.get(dictionary, "ToUnicode"); map && map->kind == Kind::stream)
        if (const auto data = doc.stream_data(*map)) font->to_unicode = parse_to_unicode(*data);

    if (kind == "Type0") {
        font->kind = Font::Kind::composite;
        ObjectPtr descendant;
        if (const auto descendants = doc.get(dictionary, "DescendantFonts"); descendants && descendants->kind == Kind::array && !descendants->array.empty())
            descendant = doc.resolve(descendants->array[0]);
        const auto descriptor = doc.get(descendant, "FontDescriptor");
        font->face = embedded_face(doc, descriptor);
        font->embedded = font->face != nullptr;
        if (!font->face) font->face = system_face(base);
        if (const auto map = doc.get(descendant, "CIDToGIDMap"); map && map->kind == Kind::stream) {
            if (const auto data = doc.stream_data(*map)) {
                font->identity_gid = false;
                for (std::size_t i = 0; i + 1 < data->size(); i += 2) font->cid_to_gid.push_back(static_cast<std::uint16_t>(((*data)[i] << 8) | (*data)[i + 1]));
            }
        }
        if (const auto dw = doc.get(descendant, "DW"); dw && dw->is_number()) font->default_width = dw->number_value();
        if (const auto w = doc.get(descendant, "W"); w && w->kind == Kind::array) {
            for (std::size_t i = 0; i < w->array.size();) {
                const auto first = doc.resolve(w->array[i]);
                if (!first || !first->is_number() || i + 1 >= w->array.size()) break;
                const auto second = doc.resolve(w->array[i + 1]);
                const auto start = static_cast<std::uint32_t>(first->number_value());
                if (second && second->kind == Kind::array) {
                    for (std::size_t k = 0; k < second->array.size(); ++k) {
                        const auto width = doc.resolve(second->array[k]);
                        if (width && width->is_number()) font->cid_widths[start + static_cast<std::uint32_t>(k)] = width->number_value();
                    }
                    i += 2;
                } else if (second && second->is_number() && i + 2 < w->array.size()) {
                    const auto width = doc.resolve(w->array[i + 2]);
                    const auto last = static_cast<std::uint32_t>(second->number_value());
                    for (auto cid = start; cid <= last && cid - start < 65536; ++cid) font->cid_widths[cid] = width && width->is_number() ? width->number_value() : font->default_width;
                    i += 3;
                } else break;
            }
        }
        return font;
    }

    // Widths (simple and Type 3 fonts).
    const auto first_char = doc.get(dictionary, "FirstChar");
    const auto first = first_char && first_char->is_number() ? static_cast<int>(first_char->number_value()) : 0;
    const auto widths = numbers_of(doc, dictionary->dictionary.find("Widths"));
    for (std::size_t i = 0; i < widths.size(); ++i) {
        const auto code = first + static_cast<int>(i);
        if (code >= 0 && code < 256) { font->widths[static_cast<std::size_t>(code)] = widths[i]; font->has_width[static_cast<std::size_t>(code)] = true; }
    }
    // The encoding: a base encoding and /Differences (glyph names).
    std::array<std::optional<std::string>, 256> differences;
    std::function<char16_t(std::uint8_t)> base_unicode = standard;
    const auto encoding = doc.get(dictionary, "Encoding");
    const auto apply_base = [&](const std::string& name) {
        if (name == "WinAnsiEncoding") base_unicode = win_ansi;
        else if (name == "MacRomanEncoding") base_unicode = mac_roman;
        else if (name == "StandardEncoding") base_unicode = standard;
    };
    bool has_encoding = false;
    if (encoding && encoding->kind == Kind::name) { apply_base(encoding->text); has_encoding = true; }
    else if (encoding && encoding->kind == Kind::dictionary) {
        has_encoding = true;
        if (const auto base_encoding = doc.get(encoding, "BaseEncoding"); base_encoding && base_encoding->kind == Kind::name) apply_base(base_encoding->text);
        if (const auto diff = doc.get(encoding, "Differences"); diff && diff->kind == Kind::array) {
            int code = 0;
            for (const auto& item : diff->array) {
                const auto value = doc.resolve(item);
                if (value && value->is_number()) code = static_cast<int>(value->number_value());
                else if (value && value->kind == Kind::name && code >= 0 && code < 256) differences[static_cast<std::size_t>(code++)] = value->text;
            }
        }
    }
    if (kind == "Type3") {
        font->kind = Font::Kind::type3;
        const auto matrix = numbers_of(doc, dictionary->dictionary.find("FontMatrix"));
        if (matrix.size() == 6) font->font_matrix = {matrix[0], matrix[1], matrix[2], matrix[3], matrix[4], matrix[5]};
        font->char_procs = doc.get(dictionary, "CharProcs");
        font->resources = doc.get(dictionary, "Resources");
        for (std::size_t code = 0; code < 256; ++code)
            if (differences[code]) font->names[code] = *differences[code];
        return font;
    }
    const auto descriptor = doc.get(dictionary, "FontDescriptor");
    font->face = embedded_face(doc, descriptor);
    font->embedded = font->face != nullptr;
    if (!font->face) font->face = system_face(base);
    if (!font->face) return font;
    const auto flags_object = doc.get(descriptor, "Flags");
    const auto flags = flags_object && flags_object->is_number() ? static_cast<std::uint32_t>(flags_object->number_value()) : 0;
    const bool symbolic = (flags & 4) != 0 && (flags & 32) == 0;
    for (std::size_t code = 0; code < 256; ++code) {
        std::uint16_t glyph = 0;
        if (differences[code] && font->embedded) glyph = glyph_with_name(*font->face, *differences[code]);
        std::optional<char16_t> unicode;
        if (differences[code]) unicode = unicode_for_name(*differences[code]);
        else if (const auto mapped = font->to_unicode.find(static_cast<std::uint32_t>(code)); !font->embedded && mapped != font->to_unicode.end() && mapped->second.size() == 1) unicode = mapped->second[0];
        else if (!(font->embedded && symbolic && !has_encoding)) unicode = base_unicode(static_cast<std::uint8_t>(code));
        if (!glyph && unicode && *unicode) {
            const std::array<std::uint16_t, 1> units{*unicode};
            glyph = glyphs_for(*font->face, units)[0];
        }
        if (!glyph && font->embedded) {
            // Symbolic TrueType fonts map codes through (3,0): U+F000 + code.
            for (const std::uint16_t candidate : {static_cast<std::uint16_t>(0xf000 + code), static_cast<std::uint16_t>(code)}) {
                const std::array<std::uint16_t, 1> units{candidate};
                glyph = glyphs_for(*font->face, units)[0];
                if (glyph) break;
            }
        }
        font->glyphs[code] = glyph;
    }
    return font;
}

// ---- the renderer ------------------------------------------------------------------------------------------
std::int32_t blend_mode_named(const std::string& name) {
    static const std::map<std::string, std::int32_t> modes{
        {"Multiply", blend::multiply}, {"Screen", blend::screen}, {"Overlay", blend::overlay}, {"Darken", blend::darken},
        {"Lighten", blend::lighten}, {"ColorDodge", blend::color_dodge}, {"ColorBurn", blend::color_burn}, {"HardLight", blend::hard_light},
        {"SoftLight", blend::soft_light}, {"Difference", blend::difference}, {"Exclusion", blend::exclusion}, {"Hue", blend::hue},
        {"Saturation", blend::saturation}, {"Color", blend::color}, {"Luminosity", blend::luminosity}};
    const auto found = modes.find(name);
    return found == modes.end() ? blend::normal : found->second;
}

struct TextState {
    FontPtr font;
    double size{0}, char_spacing{}, word_spacing{}, scale{1}, leading{}, rise{};
    std::int32_t render{0};
};
struct PdfState {
    ColorSpacePtr fill_space, stroke_space;
    std::vector<double> fill{0}, stroke{0};
    ObjectPtr fill_pattern, stroke_pattern;
    double fill_alpha{1}, stroke_alpha{1};
    TextState text;
};

struct Renderer {
    Quartz& q;
    ContextState& c;
    pdf::Document& doc;
    Affine page_ctm;                       // the initial CTM (pattern space)
    double base_alpha{1};
    PdfState state;
    std::vector<PdfState> saved;
    std::vector<std::size_t> marks;        // saved.size() at each form / glyph entry (unbalanced Q stops there)
    PathData path;                         // user space
    Point current{};
    std::optional<bool> pending_clip;      // even-odd
    Affine tm, tlm;
    std::map<const pdf::Object*, FontPtr> fonts;
    std::set<std::string> reported;
    int depth{};

    void note(const std::string& what) {
        if (reported.insert(what).second) diagnostic("CGContextDrawPDFPage: " + what);
    }

    // q / Q map to the context's save / restore (CTM, clips, line state) plus the PDF state.
    void push() {
        save_state(c);
        saved.push_back(state);
    }
    void pop() {
        if (saved.empty() || (!marks.empty() && saved.size() <= marks.back())) return;
        state = std::move(saved.back());
        saved.pop_back();
        restore_state(q, c);
    }

    ColorSpacePtr color_space(const ObjectPtr& resources, const ObjectPtr& source, int level = 0) {
        auto object = doc.resolve(source);
        if (!object || level > 8) return device(ColorSpace::Type::gray, 1);
        if (object->kind == Kind::name) {
            const auto& n = object->text;
            if (n == "DeviceGray" || n == "G" || n == "CalGray") return device(ColorSpace::Type::gray, 1);
            if (n == "DeviceRGB" || n == "RGB" || n == "CalRGB") return device(ColorSpace::Type::rgb, 3);
            if (n == "DeviceCMYK" || n == "CMYK") return device(ColorSpace::Type::cmyk, 4);
            if (n == "Pattern") return device(ColorSpace::Type::pattern, 0);
            // A named resource.
            if (const auto named = doc.get(doc.get(resources, "ColorSpace"), n)) return color_space(resources, named, level + 1);
            return device(ColorSpace::Type::gray, 1);
        }
        if (object->kind != Kind::array || object->array.empty()) return device(ColorSpace::Type::gray, 1);
        const auto family = doc.resolve(object->array[0]);
        const std::string name = family && family->kind == Kind::name ? family->text : "";
        const auto arg = [&](std::size_t i) { return i < object->array.size() ? doc.resolve(object->array[i]) : nullptr; };
        if (name == "CalGray") return device(ColorSpace::Type::gray, 1);
        if (name == "CalRGB") return device(ColorSpace::Type::rgb, 3);
        if (name == "ICCBased") {
            const auto stream = arg(1);
            if (const auto alternate = doc.get(stream, "Alternate")) return color_space(resources, alternate, level + 1);
            const auto n = doc.get(stream, "N");
            const auto count = n && n->is_number() ? static_cast<int>(n->number_value()) : 3;
            return count == 1 ? device(ColorSpace::Type::gray, 1) : count == 4 ? device(ColorSpace::Type::cmyk, 4) : device(ColorSpace::Type::rgb, 3);
        }
        if (name == "Lab") {
            auto made = std::make_shared<ColorSpace>();
            made->type = ColorSpace::Type::lab;
            made->components = 3;
            const auto white = numbers_of(doc, arg(1) ? arg(1)->dictionary.find("WhitePoint") : nullptr);
            if (white.size() == 3) made->white = {white[0], white[1], white[2]};
            const auto range = numbers_of(doc, arg(1) ? arg(1)->dictionary.find("Range") : nullptr);
            if (range.size() == 4) made->lab_range = range;
            return made;
        }
        if (name == "Indexed" || name == "I") {
            auto made = std::make_shared<ColorSpace>();
            made->type = ColorSpace::Type::indexed;
            made->components = 1;
            made->base = color_space(resources, object->array.size() > 1 ? object->array[1] : nullptr, level + 1);
            const auto hival = arg(2);
            made->hival = hival && hival->is_number() ? static_cast<int>(hival->number_value()) : 0;
            if (const auto table = arg(3)) {
                if (table->kind == Kind::string) made->lookup.assign(table->text.begin(), table->text.end());
                else if (table->kind == Kind::stream)
                    if (const auto data = doc.stream_data(*table)) made->lookup = *data;
            }
            return made;
        }
        if (name == "Separation" || name == "DeviceN") {
            auto made = std::make_shared<ColorSpace>();
            made->type = ColorSpace::Type::tinted;
            const auto names = arg(1);
            made->components = name == "Separation" ? 1 : names && names->kind == Kind::array ? names->array.size() : 1;
            made->base = color_space(resources, object->array.size() > 2 ? object->array[2] : nullptr, level + 1);
            made->tint = make_function(doc, object->array.size() > 3 ? object->array[3] : nullptr);
            return made;
        }
        if (name == "Pattern") {
            auto made = std::make_shared<ColorSpace>();
            made->type = ColorSpace::Type::pattern;
            if (object->array.size() > 1) made->base = color_space(resources, object->array[1], level + 1);
            made->components = made->base ? made->base->components : 0;
            return made;
        }
        return device(ColorSpace::Type::rgb, 3);
    }

    // ---- painting ----
    PathData base_path() const { return path.transformed(c.g.ctm); }

    void end_path() {
        if (pending_clip) clip_to_path(q, c, base_path(), *pending_clip);
        pending_clip.reset();
        path = {};
    }

    bool paint_pattern(const ObjectPtr& resources, const ObjectPtr& pattern_name, bool even_odd, bool stroke) {
        // The pattern (a shading pattern) fills the clip of the path.
        const auto pattern = doc.get(doc.get(resources, "Pattern"), pattern_name ? pattern_name->text : "");
        const auto type = doc.get(pattern, "PatternType");
        if (!type || !type->is_number()) return false;
        if (type->number_value() != 2) { note("tiling patterns are not drawn"); return true; }
        if (stroke) { note("shading patterns in strokes are not drawn"); return true; }
        save_state(c);
        clip_to_path(q, c, base_path(), even_odd);
        const auto matrix = numbers_of(doc, pattern->dictionary.find("Matrix"));
        c.g.ctm = (matrix.size() == 6 ? Affine{matrix[0], matrix[1], matrix[2], matrix[3], matrix[4], matrix[5]} : Affine{}).then(page_ctm);
        draw_shading(resources, doc.get(pattern, "Shading"));
        restore_state(q, c);
        return true;
    }

    void fill(const ObjectPtr& resources, bool even_odd) {
        if (state.fill_space && state.fill_space->type == ColorSpace::Type::pattern) {
            static_cast<void>(paint_pattern(resources, state.fill_pattern, even_odd, false));
            return;
        }
        c.g.fill.pattern.reset();
        c.g.fill.color = state.fill_space ? state.fill_space->to_rgb(state.fill) : Rgba{0, 0, 0, 1};
        c.g.alpha = base_alpha * state.fill_alpha;
        fill_path(q, c, base_path(), even_odd);
    }
    void stroke(const ObjectPtr& resources) {
        if (state.stroke_space && state.stroke_space->type == ColorSpace::Type::pattern) {
            static_cast<void>(paint_pattern(resources, state.stroke_pattern, false, true));
            return;
        }
        c.g.stroke.pattern.reset();
        c.g.stroke.color = state.stroke_space ? state.stroke_space->to_rgb(state.stroke) : Rgba{0, 0, 0, 1};
        c.g.alpha = base_alpha * state.stroke_alpha;
        stroke_path(q, c, base_path());
    }

    void draw_shading(const ObjectPtr& resources, const ObjectPtr& source) {
        const auto shading = doc.resolve(source);
        const auto type = doc.get(shading, "ShadingType");
        if (!type || !type->is_number()) return;
        const auto kind = static_cast<int>(type->number_value());
        if (kind != 2 && kind != 3) { note("shading type " + std::to_string(kind) + " is not drawn"); return; }
        const auto space = color_space(resources, doc.get(shading, "ColorSpace"));
        const auto coords = numbers_of(doc, shading->dictionary.find("Coords"));
        const auto domain = numbers_of(doc, shading->dictionary.find("Domain"));
        const auto extend = doc.get(shading, "Extend");
        const auto function = make_function(doc, shading->dictionary.find("Function"));
        if (!function || coords.size() < (kind == 2 ? 4u : 6u)) return;
        const double t0 = domain.size() == 2 ? domain[0] : 0, t1 = domain.size() == 2 ? domain[1] : 1;
        GradientInfo gradient;
        constexpr int samples = 64;
        for (int i = 0; i <= samples; ++i) {
            const double s = static_cast<double>(i) / samples;
            gradient.stops.push_back({s, space->to_rgb(function->eval({t0 + s * (t1 - t0)}))});
        }
        bool extend_start = false, extend_end = false;
        if (extend && extend->kind == Kind::array && extend->array.size() == 2) {
            const auto a = doc.resolve(extend->array[0]), b = doc.resolve(extend->array[1]);
            extend_start = a && a->kind == Kind::boolean && a->boolean;
            extend_end = b && b->kind == Kind::boolean && b->boolean;
        }
        c.g.alpha = base_alpha * state.fill_alpha;
        if (kind == 2) draw_gradient(q, c, gradient, false, {coords[0], coords[1]}, 0, {coords[2], coords[3]}, 0, extend_start, extend_end);
        else draw_gradient(q, c, gradient, true, {coords[0], coords[1]}, coords[2], {coords[3], coords[4]}, coords[5], extend_start, extend_end);
    }

    // ---- images ----
    // Straight RGBA of an image XObject or inline image (`dict` with full or abbreviated keys).
    std::optional<std::pair<std::pair<std::uint32_t, std::uint32_t>, std::vector<std::uint8_t>>> decode_image(const ObjectPtr& resources, const ObjectPtr& image, bool inline_image, int level = 0) {
        const auto key = [&](std::string_view full, std::string_view abbreviation) {
            auto value = doc.get(image, full);
            if (!value && inline_image) value = doc.get(image, abbreviation);
            return value;
        };
        const auto number = [&](std::string_view full, std::string_view abbreviation, double fallback) {
            const auto value = key(full, abbreviation);
            return value && value->is_number() ? value->number_value() : fallback;
        };
        const auto width = static_cast<std::uint32_t>(std::max(0.0, number("Width", "W", 0)));
        const auto height = static_cast<std::uint32_t>(std::max(0.0, number("Height", "H", 0)));
        if (width == 0 || height == 0 || static_cast<std::uint64_t>(width) * height > (std::uint64_t{1} << 28)) return std::nullopt;
        pdf::DataFormat format{};
        const auto data = doc.stream_data(*image, &format);
        if (!data) { note("an image with an unsupported filter is not drawn"); return std::nullopt; }
        std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4, 255);
        const auto mask_object = key("ImageMask", "IM");
        const bool stencil = mask_object && mask_object->kind == Kind::boolean && mask_object->boolean;
        if (format == pdf::DataFormat::jpeg2000) { note("JPEG 2000 images are not drawn"); return std::nullopt; }
        if (format == pdf::DataFormat::jpeg && !stencil) {
            auto frame = codec::decode(*data);
            if (!frame || frame->width == 0) { note("an undecodable JPEG image is not drawn"); return std::nullopt; }
            if (frame->width != width || frame->height != height) {
                // Trust the JPEG's own size.
                rgba = std::move(frame->rgba);
                return std::pair{std::pair{frame->width, frame->height}, std::move(rgba)};
            }
            rgba = std::move(frame->rgba);
        } else {
            const auto bits = stencil ? 1 : static_cast<int>(number("BitsPerComponent", "BPC", 8));
            if (bits <= 0 || bits > 16) return std::nullopt;
            const auto space = stencil ? device(ColorSpace::Type::gray, 1) : color_space(resources, key("ColorSpace", "CS"));
            const auto components = std::max<std::size_t>(1, space->components);
            const auto decode = numbers_of(doc, key("Decode", "D"));
            const auto max = static_cast<double>((1u << bits) - 1);
            const auto row_bytes = (static_cast<std::size_t>(width) * components * static_cast<std::size_t>(bits) + 7) / 8;
            const auto fill_rgb = state.fill_space ? state.fill_space->to_rgb(state.fill) : Rgba{0, 0, 0, 1};
            std::vector<double> values(components);
            for (std::uint32_t y = 0; y < height; ++y) {
                const auto row = static_cast<std::size_t>(y) * row_bytes;
                std::size_t bit = 0;
                for (std::uint32_t x = 0; x < width; ++x) {
                    for (std::size_t k = 0; k < components; ++k, bit += static_cast<std::size_t>(bits)) {
                        std::uint32_t sample = 0;
                        for (int b = 0; b < bits; ++b) {
                            const auto at = row + (bit + static_cast<std::size_t>(b)) / 8;
                            const auto value = at < data->size() ? (*data)[at] : 0;
                            sample = (sample << 1) | ((value >> (7 - (bit + static_cast<std::size_t>(b)) % 8)) & 1);
                        }
                        const double d0 = decode.size() > 2 * k ? decode[2 * k] : 0;
                        const double d1 = decode.size() > 2 * k + 1 ? decode[2 * k + 1] : space->type == ColorSpace::Type::indexed ? max : 1;
                        values[k] = d0 + sample * (d1 - d0) / max;
                    }
                    auto* out = rgba.data() + (static_cast<std::size_t>(y) * width + x) * 4;
                    if (stencil) {
                        // Sample 0 paints (with Decode [0 1]).
                        const bool paints = values[0] < 0.5;
                        out[0] = static_cast<std::uint8_t>(std::lround(std::clamp(fill_rgb.r, 0.0, 1.0) * 255));
                        out[1] = static_cast<std::uint8_t>(std::lround(std::clamp(fill_rgb.g, 0.0, 1.0) * 255));
                        out[2] = static_cast<std::uint8_t>(std::lround(std::clamp(fill_rgb.b, 0.0, 1.0) * 255));
                        out[3] = paints ? 255 : 0;
                        continue;
                    }
                    const auto color = space->to_rgb(values);
                    out[0] = static_cast<std::uint8_t>(std::lround(std::clamp(color.r, 0.0, 1.0) * 255));
                    out[1] = static_cast<std::uint8_t>(std::lround(std::clamp(color.g, 0.0, 1.0) * 255));
                    out[2] = static_cast<std::uint8_t>(std::lround(std::clamp(color.b, 0.0, 1.0) * 255));
                    out[3] = 255;
                }
            }
            // Colour-key masking.
            if (const auto mask = key("Mask", "Mask"); mask && mask->kind == Kind::array && !stencil) {
                const auto ranges = numbers_of(doc, mask);
                if (ranges.size() >= 2 * components) {
                    for (std::uint32_t y = 0; y < height; ++y) {
                        const auto row = static_cast<std::size_t>(y) * row_bytes;
                        std::size_t bit = 0;
                        for (std::uint32_t x = 0; x < width; ++x) {
                            bool inside = true;
                            for (std::size_t k = 0; k < components; ++k, bit += static_cast<std::size_t>(bits)) {
                                std::uint32_t sample = 0;
                                for (int b = 0; b < bits; ++b) {
                                    const auto at = row + (bit + static_cast<std::size_t>(b)) / 8;
                                    sample = (sample << 1) | (((at < data->size() ? (*data)[at] : 0) >> (7 - (bit + static_cast<std::size_t>(b)) % 8)) & 1);
                                }
                                inside = inside && sample >= ranges[2 * k] && sample <= ranges[2 * k + 1];
                            }
                            if (inside) rgba[(static_cast<std::size_t>(y) * width + x) * 4 + 3] = 0;
                        }
                    }
                }
            }
        }
        // Soft masks and stencil masks from other images (resampled to this size).
        const auto apply_alpha = [&](const ObjectPtr& mask_stream, bool stencil_mask) {
            if (!mask_stream || mask_stream->kind != Kind::stream || level > 2) return;
            const auto decoded = decode_image(resources, mask_stream, false, level + 1);
            if (!decoded) return;
            const auto [mw, mh] = decoded->first;
            const auto& pixels = decoded->second;
            for (std::uint32_t y = 0; y < height; ++y)
                for (std::uint32_t x = 0; x < width; ++x) {
                    const auto mx = static_cast<std::size_t>(x) * mw / width, my = static_cast<std::size_t>(y) * mh / height;
                    const auto at = (my * mw + mx) * 4;
                    // Soft masks: the mask's gray; stencil masks: where they would paint.
                    const auto value = stencil_mask ? pixels[at + 3] : pixels[at];
                    auto& alpha = rgba[(static_cast<std::size_t>(y) * width + x) * 4 + 3];
                    alpha = static_cast<std::uint8_t>(alpha * value / 255);
                }
        };
        if (!inline_image) {
            apply_alpha(doc.get(image, "SMask"), false);
            const auto mask = doc.get(image, "Mask");
            if (mask && mask->kind == Kind::stream) apply_alpha(mask, true);
        }
        return std::pair{std::pair{width, height}, std::move(rgba)};
    }

    void draw_image_object(const ObjectPtr& resources, const ObjectPtr& image, bool inline_image) {
        const auto decoded = decode_image(resources, image, inline_image);
        if (!decoded) return;
        PixelLayout layout;
        layout.width = decoded->first.first;
        layout.height = decoded->first.second;
        layout.bytes_per_row = static_cast<std::uint64_t>(layout.width) * 4;
        layout.bitmap_info = alpha_info::last;
        layout.straight_alpha = true;
        const auto made = q.make_image(layout, q.srgb(), std::make_shared<const std::vector<std::uint8_t>>(decoded->second));
        c.g.alpha = base_alpha * state.fill_alpha;
        try { draw_image(q, c, made, {{0, 0}, {1, 1}}); } catch (...) { q.release(made); throw; }
        q.release(made);
    }

    // ---- forms ----
    void draw_form(const ObjectPtr& resources, const ObjectPtr& form) {
        if (depth > 24 || form->kind != Kind::stream) return;
        const auto data = doc.stream_data(*form);
        if (!data) return;
        push();
        marks.push_back(saved.size());
        const auto matrix = numbers_of(doc, form->dictionary.find("Matrix"));
        if (matrix.size() == 6) c.g.ctm = Affine{matrix[0], matrix[1], matrix[2], matrix[3], matrix[4], matrix[5]}.then(c.g.ctm);
        const auto box = numbers_of(doc, form->dictionary.find("BBox"));
        if (box.size() == 4) {
            PathData clip;
            clip.add_rect(c.g.ctm, standardize({{box[0], box[1]}, {box[2] - box[0], box[3] - box[1]}}));
            clip_to_path(q, c, clip, false);
        }
        auto own = doc.get(form, "Resources");
        ++depth;
        try { run(*data, own ? own : resources); } catch (...) { --depth; marks.pop_back(); throw; }
        --depth;
        while (saved.size() > marks.back()) pop();
        marks.pop_back();
        pop();
    }

    // ---- text ----
    FontPtr font_named(const ObjectPtr& resources, const std::string& name) {
        const auto dictionary = doc.get(doc.get(resources, "Font"), name);
        if (!dictionary || dictionary->kind != Kind::dictionary) return nullptr;
        auto& cached = fonts[dictionary.get()];
        if (!cached) cached = load_font(doc, dictionary);
        return cached;
    }

    void show(const ObjectPtr& resources, const std::string& bytes) {
        const auto& ts = state.text;
        const auto font = ts.font;
        if (!font) return;
        const bool two_byte = font->kind == Font::Kind::composite;
        GlyphRunDraw run;
        run.face = font->face;
        run.size = ts.size;
        double x = 0;
        for (std::size_t i = 0; i < bytes.size();) {
            std::uint32_t code = static_cast<unsigned char>(bytes[i]);
            std::size_t length = 1;
            if (two_byte && i + 1 < bytes.size()) { code = (code << 8) | static_cast<unsigned char>(bytes[i + 1]); length = 2; }
            i += length;
            double width = 0;
            std::uint16_t glyph = 0;
            if (font->kind == Font::Kind::type3) {
                width = font->widths[code & 0xff] * font->font_matrix.a;
                draw_type3_glyph(code & 0xff, x);
            } else if (font->kind == Font::Kind::composite) {
                const auto found = font->cid_widths.find(code);
                width = (found != font->cid_widths.end() ? found->second : font->default_width) / 1000;
                if (font->embedded) glyph = font->identity_gid ? static_cast<std::uint16_t>(code) : code < font->cid_to_gid.size() ? font->cid_to_gid[code] : 0;
                else if (font->face) {
                    const auto unicode = font->to_unicode.find(code);
                    if (unicode != font->to_unicode.end() && !unicode->second.empty()) {
                        std::vector<std::uint16_t> units(unicode->second.begin(), unicode->second.end());
                        glyph = glyphs_for(*font->face, units)[0];
                    }
                }
            } else {
                glyph = font->glyphs[code & 0xff];
                if (font->has_width[code & 0xff]) width = font->widths[code & 0xff] / 1000;
                else if (font->face && glyph) {
                    const std::array<std::uint16_t, 1> one{glyph};
                    width = advances_for(*font->face, 1000, one)[0] / 1000;
                }
            }
            if (font->kind != Font::Kind::type3 && glyph) {
                run.glyphs.push_back(glyph);
                run.positions.push_back({x, 0});
            }
            x += width * ts.size + ts.char_spacing + (length == 1 && code == 32 ? ts.word_spacing : 0);
        }
        if (!run.glyphs.empty() && run.face && ts.render != text_mode::invisible) {
            c.text_matrix = Affine{ts.scale, 0, 0, 1, 0, ts.rise}.then(tm);
            c.g.text_mode = ts.render;
            c.g.fill.pattern.reset();
            c.g.stroke.pattern.reset();
            c.g.fill.color = state.fill_space && state.fill_space->type != ColorSpace::Type::pattern ? state.fill_space->to_rgb(state.fill) : Rgba{0, 0, 0, 1};
            c.g.stroke.color = state.stroke_space && state.stroke_space->type != ColorSpace::Type::pattern ? state.stroke_space->to_rgb(state.stroke) : Rgba{0, 0, 0, 1};
            c.g.alpha = base_alpha * state.fill_alpha;
            draw_glyphs(q, c, run, {0, 0}, std::nullopt);
        }
        static_cast<void>(resources);
        tm = Affine::translation(x * ts.scale, 0).then(tm);
    }

    void draw_type3_glyph(std::uint32_t code, double x) {
        const auto& ts = state.text;
        const auto& font = *ts.font;
        const auto proc = doc.get(font.char_procs, font.names[code]);
        if (!proc || proc->kind != Kind::stream || depth > 24) return;
        const auto data = doc.stream_data(*proc);
        if (!data) return;
        push();
        marks.push_back(saved.size());
        const auto text_to_user = Affine::translation(x, 0).then(Affine{ts.size * ts.scale, 0, 0, ts.size, 0, ts.rise}).then(tm);
        c.g.ctm = font.font_matrix.then(text_to_user).then(c.g.ctm);
        const auto saved_tm = tm, saved_tlm = tlm;
        ++depth;
        try { run(*data, font.resources ? font.resources : nullptr); } catch (...) { --depth; marks.pop_back(); throw; }
        --depth;
        tm = saved_tm;
        tlm = saved_tlm;
        while (saved.size() > marks.back()) pop();
        marks.pop_back();
        pop();
    }

    // ---- the interpreter ----
    void run(std::span<const std::uint8_t> content, const ObjectPtr& resources) {
        pdf::Lexer lexer(content);
        std::vector<ObjectPtr> operands;
        const auto num = [&](std::size_t i) {
            // The i-th of the operands (from the first), 0 when missing.
            return i < operands.size() && operands[i]->is_number() ? operands[i]->number_value() : 0.0;
        };
        const auto numbers = [&] {
            std::vector<double> out;
            for (const auto& o : operands) if (o->is_number()) out.push_back(o->number_value());
            return out;
        };
        while (true) {
            if (lexer.at_end()) break;
            const auto token = lexer.next(false);
            if (!token) break;
            if ((*token)->kind != Kind::keyword) { operands.push_back(*token); continue; }
            const auto& op = (*token)->text;
            const auto count = operands.size();
            // Graphics state.
            if (op == "q") push();
            else if (op == "Q") pop();
            else if (op == "cm" && count >= 6) c.g.ctm = Affine{num(0), num(1), num(2), num(3), num(4), num(5)}.then(c.g.ctm);
            else if (op == "w" && count >= 1) c.g.line_width = num(0);
            else if (op == "J" && count >= 1) c.g.cap = static_cast<std::uint32_t>(std::clamp(num(0), 0.0, 2.0));
            else if (op == "j" && count >= 1) c.g.join = static_cast<std::uint32_t>(std::clamp(num(0), 0.0, 2.0));
            else if (op == "M" && count >= 1) c.g.miter_limit = num(0);
            else if (op == "d" && count >= 2) {
                c.g.dashes = numbers_of(doc, operands[0]);
                c.g.dash_phase = num(1);
            }
            else if (op == "ri" || op == "i") {}
            else if (op == "gs" && count >= 1 && operands[0]->kind == Kind::name) apply_ext_state(resources, operands[0]->text);
            // Paths.
            else if (op == "m" && count >= 2) { current = {num(0), num(1)}; path.move_to(current); }
            else if (op == "l" && count >= 2) { current = {num(0), num(1)}; path.line_to(current); }
            else if (op == "c" && count >= 6) { path.cubic_to({num(0), num(1)}, {num(2), num(3)}, {num(4), num(5)}); current = {num(4), num(5)}; }
            else if (op == "v" && count >= 4) { path.cubic_to(current, {num(0), num(1)}, {num(2), num(3)}); current = {num(2), num(3)}; }
            else if (op == "y" && count >= 4) { path.cubic_to({num(0), num(1)}, {num(2), num(3)}, {num(2), num(3)}); current = {num(2), num(3)}; }
            else if (op == "h") path.close();
            else if (op == "re" && count >= 4) {
                path.add_rect({}, {{num(0), num(1)}, {num(2), num(3)}});
                current = {num(0), num(1)};
            }
            else if (op == "S") { stroke(resources); end_path(); }
            else if (op == "s") { path.close(); stroke(resources); end_path(); }
            else if (op == "f" || op == "F") { fill(resources, false); end_path(); }
            else if (op == "f*") { fill(resources, true); end_path(); }
            else if (op == "B") { fill(resources, false); stroke(resources); end_path(); }
            else if (op == "B*") { fill(resources, true); stroke(resources); end_path(); }
            else if (op == "b") { path.close(); fill(resources, false); stroke(resources); end_path(); }
            else if (op == "b*") { path.close(); fill(resources, true); stroke(resources); end_path(); }
            else if (op == "n") end_path();
            else if (op == "W") pending_clip = false;
            else if (op == "W*") pending_clip = true;
            // Colour.
            else if (op == "CS" && count >= 1) { state.stroke_space = color_space(resources, operands[0]); state.stroke = state.stroke_space->initial(); }
            else if (op == "cs" && count >= 1) { state.fill_space = color_space(resources, operands[0]); state.fill = state.fill_space->initial(); }
            else if (op == "SC" || op == "SCN") {
                state.stroke = numbers();
                state.stroke_pattern = count && operands.back()->kind == Kind::name ? operands.back() : nullptr;
            }
            else if (op == "sc" || op == "scn") {
                state.fill = numbers();
                state.fill_pattern = count && operands.back()->kind == Kind::name ? operands.back() : nullptr;
            }
            else if (op == "G" && count >= 1) { state.stroke_space = device(ColorSpace::Type::gray, 1); state.stroke = {num(0)}; }
            else if (op == "g" && count >= 1) { state.fill_space = device(ColorSpace::Type::gray, 1); state.fill = {num(0)}; }
            else if (op == "RG" && count >= 3) { state.stroke_space = device(ColorSpace::Type::rgb, 3); state.stroke = {num(0), num(1), num(2)}; }
            else if (op == "rg" && count >= 3) { state.fill_space = device(ColorSpace::Type::rgb, 3); state.fill = {num(0), num(1), num(2)}; }
            else if (op == "K" && count >= 4) { state.stroke_space = device(ColorSpace::Type::cmyk, 4); state.stroke = {num(0), num(1), num(2), num(3)}; }
            else if (op == "k" && count >= 4) { state.fill_space = device(ColorSpace::Type::cmyk, 4); state.fill = {num(0), num(1), num(2), num(3)}; }
            // Shadings, images, forms.
            else if (op == "sh" && count >= 1 && operands[0]->kind == Kind::name) {
                draw_shading(resources, doc.get(doc.get(resources, "Shading"), operands[0]->text));
            }
            else if (op == "Do" && count >= 1 && operands[0]->kind == Kind::name) {
                const auto object = doc.get(doc.get(resources, "XObject"), operands[0]->text);
                const auto subtype = doc.get(object, "Subtype");
                if (object && subtype && subtype->kind == Kind::name) {
                    if (subtype->text == "Image") draw_image_object(resources, object, false);
                    else if (subtype->text == "Form") draw_form(resources, object);
                }
            }
            else if (op == "BI") {
                // An inline image: key / value pairs up to ID, its data up to EI.
                auto image = std::make_shared<pdf::Object>();
                image->kind = Kind::stream;
                bool ok = true;
                for (;;) {
                    const auto entry_key = lexer.next(false);
                    if (!entry_key) { ok = false; break; }
                    if ((*entry_key)->kind == Kind::keyword && (*entry_key)->text == "ID") break;
                    const auto entry = lexer.next(false);
                    if (!entry) { ok = false; break; }
                    if ((*entry_key)->kind == Kind::name) image->dictionary.entries.emplace_back((*entry_key)->text, *entry);
                }
                if (!ok) break;
                const auto bytes = lexer.bytes();
                auto start = lexer.position();
                if (start < bytes.size() && (bytes[start] == ' ' || bytes[start] == '\n' || bytes[start] == '\r' || bytes[start] == '\t')) ++start;
                const auto space = [](std::uint8_t b) { return b == ' ' || b == '\n' || b == '\r' || b == '\t' || b == 0; };
                std::size_t end = start;
                for (; end + 1 < bytes.size(); ++end)
                    if (bytes[end] == 'E' && bytes[end + 1] == 'I' && (end == start || space(bytes[end - 1])) && (end + 2 >= bytes.size() || space(bytes[end + 2]))) break;
                if (end + 1 >= bytes.size()) break;
                auto data_end = end;
                while (data_end > start && space(bytes[data_end - 1])) --data_end;
                image->own_data = std::make_shared<const std::vector<std::uint8_t>>(bytes.begin() + static_cast<std::ptrdiff_t>(start),
                                                                                  bytes.begin() + static_cast<std::ptrdiff_t>(data_end));
                lexer.seek(end + 2);
                draw_image_object(resources, image, true);
            }
            // Text.
            else if (op == "BT") { tm = {}; tlm = {}; }
            else if (op == "ET") {}
            else if (op == "Tc" && count >= 1) state.text.char_spacing = num(0);
            else if (op == "Tw" && count >= 1) state.text.word_spacing = num(0);
            else if (op == "Tz" && count >= 1) state.text.scale = num(0) / 100;
            else if (op == "TL" && count >= 1) state.text.leading = num(0);
            else if (op == "Ts" && count >= 1) state.text.rise = num(0);
            else if (op == "Tr" && count >= 1) state.text.render = static_cast<std::int32_t>(std::clamp(num(0), 0.0, 7.0));
            else if (op == "Tf" && count >= 2 && operands[0]->kind == Kind::name) {
                state.text.font = font_named(resources, operands[0]->text);
                state.text.size = num(1);
            }
            else if (op == "Td" && count >= 2) { tlm = Affine::translation(num(0), num(1)).then(tlm); tm = tlm; }
            else if (op == "TD" && count >= 2) { state.text.leading = -num(1); tlm = Affine::translation(num(0), num(1)).then(tlm); tm = tlm; }
            else if (op == "Tm" && count >= 6) { tlm = Affine{num(0), num(1), num(2), num(3), num(4), num(5)}; tm = tlm; }
            else if (op == "T*") { tlm = Affine::translation(0, -state.text.leading).then(tlm); tm = tlm; }
            else if (op == "Tj" && count >= 1 && operands[0]->kind == Kind::string) show(resources, operands[0]->text);
            else if (op == "'" && count >= 1 && operands.back()->kind == Kind::string) {
                tlm = Affine::translation(0, -state.text.leading).then(tlm);
                tm = tlm;
                show(resources, operands.back()->text);
            }
            else if (op == "\"" && count >= 3 && operands[2]->kind == Kind::string) {
                state.text.word_spacing = num(0);
                state.text.char_spacing = num(1);
                tlm = Affine::translation(0, -state.text.leading).then(tlm);
                tm = tlm;
                show(resources, operands[2]->text);
            }
            else if (op == "TJ" && count >= 1 && operands[0]->kind == Kind::array) {
                for (const auto& item : operands[0]->array) {
                    if (item->kind == Kind::string) show(resources, item->text);
                    else if (item->is_number()) tm = Affine::translation(-item->number_value() / 1000 * state.text.size * state.text.scale, 0).then(tm);
                }
            }
            else if (op == "d0" || op == "d1") {}   // Type 3 glyph metrics
            // Marked content and compatibility sections: nothing to draw.
            operands.clear();
        }
    }

    void apply_ext_state(const ObjectPtr& resources, const std::string& name) {
        const auto ext = doc.get(doc.get(resources, "ExtGState"), name);
        if (!ext || ext->kind != Kind::dictionary) return;
        for (const auto& [key, raw] : ext->dictionary.entries) {
            const auto value = doc.resolve(raw);
            if (!value) continue;
            if (key == "LW" && value->is_number()) c.g.line_width = value->number_value();
            else if (key == "LC" && value->is_number()) c.g.cap = static_cast<std::uint32_t>(std::clamp(value->number_value(), 0.0, 2.0));
            else if (key == "LJ" && value->is_number()) c.g.join = static_cast<std::uint32_t>(std::clamp(value->number_value(), 0.0, 2.0));
            else if (key == "ML" && value->is_number()) c.g.miter_limit = value->number_value();
            else if (key == "D" && value->kind == Kind::array && value->array.size() == 2) {
                c.g.dashes = numbers_of(doc, value->array[0]);
                const auto phase = doc.resolve(value->array[1]);
                c.g.dash_phase = phase && phase->is_number() ? phase->number_value() : 0;
            }
            else if (key == "CA" && value->is_number()) state.stroke_alpha = std::clamp(value->number_value(), 0.0, 1.0);
            else if (key == "ca" && value->is_number()) state.fill_alpha = std::clamp(value->number_value(), 0.0, 1.0);
            else if (key == "BM") {
                const auto mode = value->kind == Kind::array && !value->array.empty() ? doc.resolve(value->array[0]) : value;
                if (mode && mode->kind == Kind::name) c.g.blend_mode = blend_mode_named(mode->text);
            }
            else if (key == "Font" && value->kind == Kind::array && value->array.size() == 2) {
                const auto font = doc.resolve(value->array[0]);
                if (font && font->kind == Kind::dictionary) {
                    auto& cached = fonts[font.get()];
                    if (!cached) cached = load_font(doc, font);
                    state.text.font = cached;
                }
                const auto size = doc.resolve(value->array[1]);
                if (size && size->is_number()) state.text.size = size->number_value();
            }
            else if (key == "SMask" && !(value->kind == Kind::name && value->text == "None")) note("soft masks are not applied");
        }
    }
};

} // namespace

void render_pdf_page(Quartz& q, ContextState& c, pdf::Document& document, const pdf::ObjectPtr& page) {
    if (!page) return;
    Renderer renderer{q, c, document};
    save_state(c);
    try {
        // The crop box (within the media box) clips the page.
        const auto box_of = [&](std::string_view key) -> std::optional<Rect> {
            const auto values = numbers_of(document, document.inherited(page, key));
            if (values.size() != 4) return std::nullopt;
            return standardize({{values[0], values[1]}, {values[2] - values[0], values[3] - values[1]}});
        };
        const auto media = box_of("MediaBox").value_or(Rect{{0, 0}, {612, 792}});
        const auto crop = rect_intersection(box_of("CropBox").value_or(media), media);
        PathData clip;
        clip.add_rect(c.g.ctm, crop);
        clip_to_path(q, c, clip, false);
        renderer.page_ctm = c.g.ctm;
        renderer.base_alpha = c.g.alpha;
        renderer.state.fill_space = device(ColorSpace::Type::gray, 1);
        renderer.state.stroke_space = device(ColorSpace::Type::gray, 1);
        c.g.line_width = 1;
        c.g.cap = c.g.join = 0;
        c.g.miter_limit = 10;
        c.g.dashes.clear();
        c.g.blend_mode = blend::normal;
        c.text_matrix = {};
        const auto resources = document.inherited(page, "Resources");
        const auto contents = document.get(page, "Contents");
        std::vector<std::uint8_t> data;
        const auto append = [&](const pdf::ObjectPtr& stream) {
            if (!stream || stream->kind != Kind::stream) return;
            if (const auto bytes = document.stream_data(*stream)) {
                data.insert(data.end(), bytes->begin(), bytes->end());
                data.push_back('\n');
            }
        };
        if (contents && contents->kind == Kind::array) for (const auto& item : contents->array) append(document.resolve(item));
        else append(contents);
        renderer.run(data, resources);
        while (!renderer.saved.empty()) renderer.pop();
    } catch (...) {
        while (!renderer.saved.empty()) {
            renderer.saved.pop_back();
            restore_state(q, c);
        }
        restore_state(q, c);
        throw;
    }
    restore_state(q, c);
}

} // namespace alchemy::gui::quartz2d
