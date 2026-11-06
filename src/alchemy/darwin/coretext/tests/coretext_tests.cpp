// CoreText unit tests (owner: Q). Offscreen only, on the Quartz 2D fixture
// (coregraphics/tests/quartz2d_fixture.hpp): fonts, glyphs, lines, runs and
// frames through their exported addresses, and a line drawn into a bitmap
// context whose pixels are read back.
#include "alchemy/darwin/coregraphics/tests/quartz2d_fixture.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <iostream>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

void text(Fixture& f) {
    const auto font = f.id(ct, "_CTFontCreateWithName", {f.str(".AppleSystemUIFont"), 0}, {24});
    require(font != 0, "CTFontCreateWithName");
    const auto ascent = f.call(ct, "_CTFontGetAscent", {font}).d(0);
    const auto descent = f.call(ct, "_CTFontGetDescent", {font}).d(0);
    const auto units = f.id(ct, "_CTFontGetUnitsPerEm", {font});
    require(ascent > 15 && ascent < 35 && descent > 3 && descent < 12 && units >= 1000, "font metrics " + std::to_string(ascent) + " " + std::to_string(descent));
    const auto name = f.id(ct, "_CTFontCopyPostScriptName", {font});
    require(f.cf.utf8(name) == ".SFNS-Regular", "mapped font keeps its Apple name: " + f.cf.utf8(name));
    // Name round trip and Helvetica fallback for unknown names.
    const auto again = f.id(ct, "_CTFontCreateWithName", {name, 0}, {12});
    require(f.call(ct, "_CTFontGetSize", {again}).d(0) == 12, "CTFontGetSize");
    const auto unknown = f.id(ct, "_CTFontCreateWithName", {f.str("No Such Font Anywhere"), 0}, {12});
    require(f.cf.utf8(f.id(ct, "_CTFontCopyFamilyName", {unknown})) == "Helvetica", "unknown font falls back to Helvetica");
    // Glyphs and advances.
    const auto characters = f.allocate(10), glyphs = f.allocate(10), advances = f.allocate(80);
    const std::uint16_t hello[] = {'H', 'e', 'l', 'l', 'o'};
    f.engine.write_bytes(characters, std::span(reinterpret_cast<const std::uint8_t*>(hello), 10));
    require((f.id(ct, "_CTFontGetGlyphsForCharacters", {font, characters, glyphs, 5}) & 0xff) == 1, "glyphs for characters");
    const auto total = f.call(ct, "_CTFontGetAdvancesForGlyphs", {font, 0, glyphs, advances, 5}).d(0);
    require(total > 40 && total < 80, "advance total " + std::to_string(total));
    // CTLine from an attributed string with a font and a colour.
    const auto red = f.id(cg, "_CGColorCreateGenericRGB", {}, {1, 0, 0, 1});
    std::vector<std::pair<Address, Address>> pairs{{f.str("NSFont"), font}, {f.str("CTForegroundColor"), red}};
    const auto attributes = f.cf.dictionary(pairs);
    const auto string = f.id(cf_library, "_CFAttributedStringCreate", {0, f.str("Hello"), attributes});
    const auto line = f.id(ct, "_CTLineCreateWithAttributedString", {string});
    require(f.id(ct, "_CTLineGetGlyphCount", {line}) == 5, "line glyph count");
    const auto metrics = f.allocate(24);
    const auto width = f.call(ct, "_CTLineGetTypographicBounds", {line, metrics, metrics + 8, metrics + 16}).d(0);
    require(close_to(width, total, 0.5), "line width " + std::to_string(width) + " vs advances " + std::to_string(total));
    require(close_to(f.read_double(metrics), ascent, 0.01) && close_to(f.read_double(metrics + 8), descent, 0.01), "line ascent/descent");
    const auto runs = f.id(ct, "_CTLineGetGlyphRuns", {line});
    require(f.cf.array_values(runs).size() == 1, "one glyph run");
    const auto run = f.cf.array_values(runs).front();
    const auto range = f.call(ct, "_CTRunGetStringRange", {run});
    require(range.x0 == 0 && range.x1 == 5, "run string range");
    const auto positions = f.allocate(80);
    f.call(ct, "_CTRunGetPositions", {run, 0, 0, positions});
    require(f.read_double(positions) == 0 && f.read_double(positions + 16) > 5, "run positions");
    // Draw at (4, 10): red pixels inside [4, 4 + width] x [10 - descent, 10 + ascent].
    Canvas canvas(f, 96, 48);
    f.call(cg, "_CGContextSetTextPosition", {canvas.context}, {4, 10});
    f.call(ct, "_CTLineDraw", {line, canvas.context});
    int count{}, min_x = 999, max_x = -1, min_y = 999, max_y = -1;
    for (std::uint32_t y = 0; y < 48; ++y)
        for (std::uint32_t x = 0; x < 96; ++x) {
            const auto p = canvas.cg_at(x, y);
            if (p[3] < 128) continue;
            require(p[0] >= p[1] && p[0] >= p[2], "text colour");
            ++count;
            min_x = std::min<int>(min_x, x); max_x = std::max<int>(max_x, x);
            min_y = std::min<int>(min_y, y); max_y = std::max<int>(max_y, y);
        }
    require(count > 60, "text drew pixels: " + std::to_string(count));
    require(min_x >= 4 && max_x <= 4 + width + 1 && min_y >= 10 - descent - 1 && max_y <= 10 + ascent + 1,
            "text bounds " + std::to_string(min_x) + "," + std::to_string(min_y) + " - " + std::to_string(max_x) + "," + std::to_string(max_y));
    require(max_y > 20 && min_y <= 11, "capital H reaches cap height");
    const auto image_bounds = f.call(ct, "_CTLineGetImageBounds", {line, 0});
    require(image_bounds.d(2) > 30 && image_bounds.d(3) > 10, "image bounds");
    // Framesetter: wrapping in a 60-point column.
    const auto long_text = f.id(cf_library, "_CFAttributedStringCreate", {0, f.str("one two three four five six"), attributes});
    const auto framesetter = f.id(ct, "_CTFramesetterCreateWithAttributedString", {long_text});
    const auto fit = f.allocate(16);
    const auto suggested = f.call(ct, "_CTFramesetterSuggestFrameSizeWithConstraints", {framesetter, 0, 0, 0, fit}, {80, 1e9});
    require(suggested.d(0) <= 80 && suggested.d(1) > 2 * (ascent + descent), "suggested frame size " + std::to_string(suggested.d(0)) + "x" + std::to_string(suggested.d(1)));
    const auto column = f.id(cg, "_CGPathCreateWithRect", {0}, {0, 0, 80, 400});
    const auto frame = f.id(ct, "_CTFramesetterCreateFrame", {framesetter, 0, 0, column, 0});
    const auto lines = f.cf.array_values(f.id(ct, "_CTFrameGetLines", {frame}));
    require(lines.size() >= 3, "wrapped lines: " + std::to_string(lines.size()));
    const auto origins = f.allocate(16 * lines.size());
    f.call(ct, "_CTFrameGetLineOrigins", {frame, 0, 0, origins});
    require(close_to(f.read_double(origins + 8), 400 - ascent, 0.5) && f.read_double(origins + 24) < f.read_double(origins + 8), "line origins");
    // Truncation.
    const auto token = f.id(ct, "_CTLineCreateWithAttributedString", {f.id(cf_library, "_CFAttributedStringCreate", {0, f.str("\xE2\x80\xA6"), attributes})});
    const auto wide = f.id(ct, "_CTLineCreateWithAttributedString", {long_text});
    const auto truncated = f.id(ct, "_CTLineCreateTruncatedLine", {wide, 1, token}, {100});
    const auto truncated_width = f.call(ct, "_CTLineGetTypographicBounds", {truncated, 0, 0, 0}).d(0);
    require(truncated_width <= 100 && truncated_width > 50, "truncated line width " + std::to_string(truncated_width));
    // Glyph outlines.
    const auto glyph = f.engine.read32(glyphs) & 0xffff;
    const auto outline = f.id(ct, "_CTFontCreatePathForGlyph", {font, glyph, 0});
    const auto outline_box = f.call(cg, "_CGPathGetPathBoundingBox", {outline});
    require(outline_box.d(2) > 5 && outline_box.d(3) > 10, "glyph outline bounds");
    // UI fonts, traits.
    const auto bold = f.id(ct, "_CTFontCreateUIFontForLanguage", {3, 0}, {0});
    require((f.id(ct, "_CTFontGetSymbolicTraits", {bold}) & 2) == 2 && f.call(ct, "_CTFontGetSize", {bold}).d(0) == 13, "emphasized system font");
    const auto menlo = f.id(ct, "_CTFontCreateWithName", {f.str("Menlo-Regular"), 0}, {11});
    require((f.id(ct, "_CTFontGetSymbolicTraits", {menlo}) & (1U << 10)) != 0, "Menlo maps to a monospaced font");
    for (const auto object : {font, again, unknown, line, string, long_text, framesetter, column, frame, token, wide, truncated, outline, bold, menlo, red, attributes}) {
        f.call(cf_library, "_CFRelease", {object});
    }
}

// CFEqual / CFHash (and -isEqual: / -hash) of CTFonts and CTFontDescriptors,
// as CoreText answers them: the same font, size and matrix; descriptors by
// their attributes. Skia's typeface cache finds CTFonts by CFEqual.
void font_equality(Fixture& f) {
    const auto equal = [&](Address a, Address b) { return f.id(cf_library, "_CFEqual", {a, b}) != 0; };
    const auto hash = [&](Address a) { return f.id(cf_library, "_CFHash", {a}); };
    const auto a = f.id(ct, "_CTFontCreateWithName", {f.str("Helvetica"), 0}, {12});
    const auto b = f.id(ct, "_CTFontCreateWithName", {f.str("Helvetica"), 0}, {12});
    const auto bigger = f.id(ct, "_CTFontCreateWithName", {f.str("Helvetica"), 0}, {13});
    const auto other = f.id(ct, "_CTFontCreateWithName", {f.str("Menlo-Regular"), 0}, {12});
    const auto matrix = f.allocate(48);
    for (const auto& [offset, value] : std::initializer_list<std::pair<Address, double>>{{0, 1}, {8, 0}, {16, 0.2}, {24, 1}})
        f.engine.write64(matrix + offset, std::bit_cast<std::uint64_t>(value));
    const auto slanted = f.id(ct, "_CTFontCreateWithName", {f.str("Helvetica"), matrix}, {12});
    const auto copy = f.id(ct, "_CTFontCreateCopyWithAttributes", {a, 0, 0}, {0});
    require(a != b && equal(a, b) && equal(b, a) && hash(a) == hash(b), "two CTFontCreateWithName fonts of one face and size are CFEqual");
    require(equal(a, copy) && hash(a) == hash(copy), "CTFontCreateCopyWithAttributes(font, 0, NULL) is CFEqual to the font");
    require(!equal(a, bigger) && !equal(a, other) && !equal(a, slanted), "fonts of another size, face or matrix are not CFEqual");
    require((f.send(a, "isEqual:", {b}).x0 & 0xff) == 1 && f.send(a, "hash").x0 == hash(a) && (f.send(a, "isEqual:", {slanted}).x0 & 0xff) == 0,
            "-[NSFont isEqual:] / -hash agree with CFEqual / CFHash");
    require(!equal(a, f.str("Helvetica")) && !equal(f.str("Helvetica"), a), "a font is not CFEqual to a string");
    require(equal(f.str("Helvetica"), f.str("Helvetica")), "CFEqual of other CF types is CoreFoundation's");

    const auto d1 = f.id(ct, "_CTFontDescriptorCreateWithNameAndSize", {f.str("Helvetica")}, {12});
    const auto d2 = f.id(ct, "_CTFontDescriptorCreateWithNameAndSize", {f.str("Helvetica")}, {12});
    const auto d3 = f.id(ct, "_CTFontDescriptorCreateWithNameAndSize", {f.str("Helvetica")}, {14});
    require(d1 != d2 && equal(d1, d2) && hash(d1) == hash(d2) && !equal(d1, d3) && !equal(d1, a),
            "CTFontDescriptors are CFEqual by their attributes");
    require((f.send(d1, "isEqual:", {d2}).x0 & 0xff) == 1 && f.send(d1, "hash").x0 == hash(d1), "-[NSFontDescriptor isEqual:] / -hash");
    for (const auto object : {a, b, bigger, other, slanted, copy, d1, d2, d3}) f.call(cf_library, "_CFRelease", {object});
}

// Run delegates, colour space variants, family name order (the frameworks wave, U21).
void delegates_spaces_collections(Fixture& f) {
    const auto font = f.id(ct, "_CTFontCreateWithName", {f.str("Helvetica"), 0}, {12});
    // A run delegate sizes its run: width 50, ascent 20, descent 5; its refCon; dealloc once.
    static int deallocs = 0;
    deallocs = 0;
    const auto host_function = [&f](const char* name, double value) {
        const auto address = f.process.install_function(name, [value](transmute::CpuState& s, transmute::GuestMemory&) { s.v[0] = {std::bit_cast<std::uint64_t>(value), 0}; },
                                                        "/usr/lib/libu21test.dylib");
        return address;
    };
    const auto dealloc = f.process.install_function("_u21_dealloc", [](transmute::CpuState&, transmute::GuestMemory&) { ++deallocs; }, "/usr/lib/libu21test.dylib");
    const auto callbacks = f.allocate(40);
    f.engine.write64(callbacks, 1);   // kCTRunDelegateVersion1
    f.engine.write64(callbacks + 8, dealloc);
    f.engine.write64(callbacks + 16, host_function("_u21_ascent", 20));
    f.engine.write64(callbacks + 24, host_function("_u21_descent", 5));
    f.engine.write64(callbacks + 32, host_function("_u21_width", 50));
    const auto delegate = f.id(ct, "_CTRunDelegateCreate", {callbacks, 0x1234});
    require(delegate && f.id(ct, "_CTRunDelegateGetRefCon", {delegate}) == 0x1234, "CTRunDelegateCreate / GetRefCon");
    std::vector<std::pair<Address, Address>> pairs{{f.str("NSFont"), font}};
    const auto attributes = f.cf.dictionary(pairs);
    const auto plain = f.id(cf_library, "_CFAttributedStringCreate", {0, f.str("AB"), attributes});
    const auto plain_line = f.id(ct, "_CTLineCreateWithAttributedString", {plain});
    const auto plain_width = f.call(ct, "_CTLineGetTypographicBounds", {plain_line, 0, 0, 0}).d(0);
    const auto text = f.id(cf_library, "_CFAttributedStringCreateMutable", {0, 0});
    const std::u16string units = u"A\uFFFCB";
    const auto string = f.cf.string_utf16(std::span(reinterpret_cast<const std::uint16_t*>(units.data()), units.size()));
    f.call(cf_library, "_CFAttributedStringReplaceString", {text, 0, 0, string});
    f.call(cf_library, "_CFAttributedStringSetAttributes", {text, 0, 3, attributes, 1});
    f.call(cf_library, "_CFAttributedStringSetAttribute", {text, 1, 1, f.str("CTRunDelegate"), delegate});
    const auto line = f.id(ct, "_CTLineCreateWithAttributedString", {text});
    const auto metrics = f.allocate(24);
    const auto width = f.call(ct, "_CTLineGetTypographicBounds", {line, metrics, metrics + 8, metrics + 16}).d(0);
    require(std::abs(width - plain_width - 50) < 1, "the delegate's run is 50 wide: " + std::to_string(width) + " vs " + std::to_string(plain_width));
    require(std::abs(f.read_double(metrics) - 20) < 0.01 && f.read_double(metrics + 8) >= 5 - 0.01, "the line takes the delegate's ascent and descent");
    f.call(cf_library, "_CFRelease", {line});
    f.call(cf_library, "_CFRelease", {text});
    f.call(cf_library, "_CFRelease", {delegate});
    require(deallocs == 1, "the delegate's dealloc callback runs once");

    // Colour space variants.
    const auto name_of = [&f](Address space) { return space ? f.cf.utf8(f.id(cg, "_CGColorSpaceCopyName", {space})) : std::string("(null)"); };
    const auto srgb = f.id(cg, "_CGColorSpaceCreateWithName", {f.str("kCGColorSpaceSRGB")});
    const auto extended = f.id(cg, "_CGColorSpaceCreateExtended", {srgb});
    require(name_of(extended) == "kCGColorSpaceExtendedSRGB" && name_of(f.id(cg, "_CGColorSpaceCreateLinearized", {srgb})) == "kCGColorSpaceLinearSRGB" &&
                name_of(f.id(cg, "_CGColorSpaceCreateExtendedLinearized", {srgb})) == "kCGColorSpaceExtendedLinearSRGB" &&
                name_of(f.id(cg, "_CGColorSpaceCreateCopyWithStandardRange", {extended})) == "kCGColorSpaceSRGB",
            "extended, linearized and standard-range variants");
    const auto p3 = f.id(cg, "_CGColorSpaceCreateWithName", {f.str("kCGColorSpaceDisplayP3")});
    require(name_of(f.id(cg, "_CGColorSpaceCreateLinearized", {p3})) == "kCGColorSpaceLinearDisplayP3" &&
                name_of(f.id(cg, "_CGColorSpaceCreateExtendedLinearized", {p3})) == "kCGColorSpaceExtendedLinearDisplayP3" &&
                (f.id(cg, "_CGColorSpaceIsHDR", {p3}) & 0xff) == 0,
            "linear Display P3 variants; not HDR");

    // Family names (font collections are tested in coretext_fonts_tests.cpp).
    require(static_cast<std::int64_t>(f.id(ct, "_CTFontManagerCompareFontFamilyNames", {f.str("arial"), f.str("Times"), 0})) < 0,
            "family names compare case-insensitively");
}

} // namespace

int main() {
#ifdef _WIN32
    _putenv_s("ALCHEMY_GUI_NO_ACTIVATE", "1");
#endif
    try {
        Fixture fixture;
        text(fixture);
        font_equality(fixture);
        delegates_spaces_collections(fixture);
        std::cout << "Alchemy CoreText tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CoreText test failed: " << error.what() << '\n';
        return 1;
    }
}
