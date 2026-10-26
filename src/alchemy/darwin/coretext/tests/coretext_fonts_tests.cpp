// CoreText fonts (src/alchemy/darwin/coretext/): glyph names from the 'post'
// table, the font manager's registrations and errors, font collections, run
// base advances and origins, and cluster breaks with an offset.
#include "alchemy/gui/tests/framework_case.hpp"
#include "alchemy/path.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>

namespace {
using namespace fill_test;

constexpr std::string_view ct = "/System/Library/Frameworks/CoreText.framework/Versions/A/CoreText";
constexpr std::string_view cg = "/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics";
constexpr std::string_view core_foundation = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";

// A block literal whose invoke function is `handler`.
Address block(Fixture& f, transmute::GuestCallHandler handler) {
    static int serial = 0;
    const auto invoke = f.process.install_function("_coretext_fonts_block_" + std::to_string(serial++), std::move(handler),
                                                   "/usr/lib/libcoretextfontstest.dylib");
    const auto descriptor = f.block(16);
    f.engine.write64(descriptor + 8, 32);
    const auto literal = f.block(32);
    f.engine.write64(literal + 8, 1ULL << 28);   // BLOCK_IS_GLOBAL
    f.engine.write64(literal + 16, invoke);
    f.engine.write64(literal + 24, descriptor);
    return literal;
}

std::uint16_t unit(Fixture& f, Address at) { return static_cast<std::uint16_t>(f.engine.read8(at) | (f.engine.read8(at + 1) << 8)); }
double real(Fixture& f, Address at) { return std::bit_cast<double>(f.engine.read64(at)); }
std::string text(Fixture& f, Address string) { return string ? f.cf.utf8(string) : std::string("(null)"); }

Address font_named(Fixture& f, std::string_view name, double size) {
    alchemy::CallRegs registers{};
    registers.x[0] = f.cf.string(name);
    registers.set_d(0, size);
    return f.regs(ct, "_CTFontCreateWithName", registers).x0;
}
Address descriptor_named(Fixture& f, std::string_view name) {
    alchemy::CallRegs registers{};
    registers.x[0] = f.cf.string(name);
    registers.set_d(0, 12);
    return f.regs(ct, "_CTFontDescriptorCreateWithNameAndSize", registers).x0;
}
Address array(Fixture& f, std::vector<Address> values) { return f.cf.array(values); }

const Case coretext_fonts_case("coretext_fonts", [](Fixture& f) {
    // ---- glyph names: Helvetica's stand-in is a TrueType face with a format 2.0 'post' table ----
    const auto font = font_named(f, "Helvetica", 12);
    const auto characters = f.block(8), glyphs = f.block(8);
    f.engine.write8(characters, 'A');
    f.engine.write8(characters + 2, ' ');
    require((f.call(ct, "_CTFontGetGlyphsForCharacters", {font, characters, glyphs, 2}) & 0xff) == 1, "glyphs for A and space");
    const auto a = unit(f, glyphs), space = unit(f, glyphs + 2);
    require(text(f, f.call(ct, "_CTFontCopyNameForGlyph", {font, a})) == "A" && text(f, f.call(ct, "_CTFontCopyNameForGlyph", {font, space})) == "space",
            "glyph names from the post table");
    require(f.call(ct, "_CTFontGetGlyphWithName", {font, f.cf.string("A")}) == a &&
                f.call(ct, "_CTFontGetGlyphWithName", {font, f.cf.string("no.such.glyph")}) == 0, "glyphs by name");
    require((f.send(font, "glyphWithName:", {f.cf.string("space")}) & 0xffff) == space, "-[NSFont glyphWithName:]");
    const auto graphics = f.call(ct, "_CTFontCopyGraphicsFont", {font, 0});
    require(f.call(cg, "_CGFontGetGlyphWithGlyphName", {graphics, f.cf.string("A")}) == a &&
                text(f, f.call(cg, "_CGFontCopyGlyphNameForGlyph", {graphics, a})) == "A", "CGFont glyph names");
    require((f.call(ct, "_CTFontHasTable", {font, 0x706f7374}) & 0xff) == 1 && (f.call(ct, "_CTFontHasTable", {font, 0x7a7a7a7a}) & 0xff) == 0,
            "CTFontHasTable");

    // ---- the font manager: a copy of a system font file ----
    const std::filesystem::path system_font = "C:/Windows/Fonts/arial.ttf";
    require(std::filesystem::exists(system_font), "the test needs C:/Windows/Fonts/arial.ttf");
    const auto directory = std::filesystem::temp_directory_path() / "alchemy_coretext_fonts";
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    std::filesystem::create_directories(directory);
    std::filesystem::copy_file(system_font, directory / "copy.ttf");
    std::ofstream(directory / "not-a-font.txt") << "plain text";
    const auto url = f.cf.file_url(alchemy::guest_path_string(directory / "copy.ttf"), false);
    const auto text_url = f.cf.file_url(alchemy::guest_path_string(directory / "not-a-font.txt"), false);
    require((f.call(ct, "_CTFontManagerIsSupportedFont", {url}) & 0xff) == 1 && (f.call(ct, "_CTFontManagerIsSupportedFont", {text_url}) & 0xff) == 0,
            "supported font files");
    require(f.call(ct, "_CTFontManagerGetScopeForURL", {url}) == 0, "an unregistered file has no scope");
    const auto descriptors = f.call(ct, "_CTFontManagerCreateFontDescriptorsFromURL", {url});
    require(descriptors && f.cf.array_values(descriptors).size() == 1, "a descriptor of the file's face");
    const auto file_descriptor = f.cf.array_values(descriptors)[0];
    require(text(f, f.call(ct, "_CTFontDescriptorCopyAttribute", {file_descriptor, f.cf.string("NSFontNameAttribute")})) == "ArialMT",
            "the descriptor names the face");
    require(f.call(ct, "_CTFontManagerGetScopeForURL", {url}) == 0, "descriptors do not register the file");

    const auto error = f.block(8);
    const auto code = [&] { return static_cast<std::int64_t>(f.call(core_foundation, "_CFErrorGetCode", {f.engine.read64(error)})); };
    require((f.call(ct, "_CTFontManagerRegisterFontsForURL", {url, 1, error}) & 0xff) == 1 && f.engine.read64(error) == 0, "the file registers");
    require(f.call(ct, "_CTFontManagerGetScopeForURL", {url}) == 1, "a registered file has process scope");
    require((f.call(ct, "_CTFontManagerRegisterFontsForURL", {url, 1, error}) & 0xff) == 0 && code() == 105, "kCTFontManagerErrorAlreadyRegistered");
    require(text(f, f.call(core_foundation, "_CFErrorGetDomain", {f.engine.read64(error)})) == "com.apple.CoreText.CTFontManagerErrorDomain",
            "the font manager's error domain");
    require((f.call(ct, "_CTFontManagerUnregisterFontsForURL", {url, 1, error}) & 0xff) == 0 && code() == 202,
            "registered fonts stay registered (kCTFontManagerErrorInUse)");
    require((f.call(ct, "_CTFontManagerUnregisterFontsForURL", {text_url, 1, error}) & 0xff) == 0 && code() == 201, "kCTFontManagerErrorNotRegistered");
    require((f.call(ct, "_CTFontManagerRegisterFontsForURL", {text_url, 1, error}) & 0xff) == 0 && code() == 104, "kCTFontManagerErrorInvalidFontData");
    const auto names = f.call(ct, "_CTFontManagerCopyAvailablePostScriptNames");
    const auto listed = f.cf.array_values(names);
    require(std::any_of(listed.begin(), listed.end(), [&](Address name) { return text(f, name) == "ArialMT"; }), "the available PostScript names");

    // Requested fonts: the unresolved descriptors.
    std::size_t unresolved = 99;
    const auto handler = block(f, [&](transmute::CpuState& s, transmute::GuestMemory&) { unresolved = f.cf.array_values(s.x[1]).size(); });
    static_cast<void>(f.call(ct, "_CTFontManagerRequestFonts",
                             {array(f, {descriptor_named(f, "Helvetica"), descriptor_named(f, "No Such Font Anywhere")}), handler}));
    require(unresolved == 1, "one requested font is unresolved");
    // Asset names: none exist.
    std::size_t asset_errors = 0;
    const auto registration = block(f, [&](transmute::CpuState& s, transmute::GuestMemory&) {
        asset_errors = f.cf.array_values(s.x[1]).size();
        s.x[0] = 1;
    });
    static_cast<void>(f.call(ct, "_CTFontManagerRegisterFontsWithAssetNames", {array(f, {f.cf.string("SomeAsset")}), 0, 1, 1, registration}));
    require(asset_errors == 1, "an error for each asset name");
    static_cast<void>(f.call(ct, "_CTFontManagerSetAutoActivationSetting", {f.cf.string("com.example.app"), 2}));
    require(f.call(ct, "_CTFontManagerGetAutoActivationSetting", {f.cf.string("com.example.app")}) == 2 &&
                f.call(ct, "_CTFontManagerGetAutoActivationSetting", {f.cf.string("com.example.other")}) == 0, "auto-activation settings");

    // ---- font collections ----
    const auto helvetica = descriptor_named(f, "Helvetica"), menlo = descriptor_named(f, "Menlo-Regular");
    const auto collection = f.call(ct, "_CTFontCollectionCreateWithFontDescriptors", {array(f, {helvetica}), 0});
    require(f.cf.array_values(f.call(ct, "_CTFontCollectionCreateMatchingFontDescriptors", {collection})).size() == 1, "a collection of descriptors");
    const auto both = f.call(ct, "_CTFontCollectionCreateCopyWithFontDescriptors", {collection, array(f, {menlo}), 0});
    require(f.cf.array_values(f.call(ct, "_CTFontCollectionCreateMatchingFontDescriptors", {both})).size() == 2, "a copy with more descriptors");
    require(f.call(ct, "_CTFontCollectionGetTypeID") == f.call(core_foundation, "_CFGetTypeID", {collection}), "the collection type ID");
    const auto descending = f.process.install_function("_coretext_fonts_descending", [](transmute::CpuState& s, transmute::GuestMemory&) {
        s.x[0] = s.x[0] > s.x[1] ? static_cast<Address>(-1) : (s.x[0] < s.x[1] ? 1 : 0);
    }, "/usr/lib/libcoretextfontstest.dylib");
    const auto sorted = f.cf.array_values(f.call(ct, "_CTFontCollectionCreateMatchingFontDescriptorsSortedWithCallback", {both, descending, 0}));
    require(sorted.size() == 2 && sorted[0] > sorted[1], "descriptors sorted by the callback");

    // ---- run base advances and origins ----
    const std::vector<std::pair<Address, Address>> pairs{{f.cf.string("NSFont"), font}};
    const auto attributes = f.cf.dictionary(pairs);
    const auto string = f.call(core_foundation, "_CFAttributedStringCreate", {0, f.cf.string("Hello"), attributes});
    const auto line = f.call(ct, "_CTLineCreateWithAttributedString", {string});
    const auto runs = f.cf.array_values(f.call(ct, "_CTLineGetGlyphRuns", {line}));
    require(runs.size() == 1, "one run");
    const auto advances = f.block(5 * 16), origins = f.block(5 * 16), positions = f.block(5 * 16);
    static_cast<void>(f.call(ct, "_CTRunGetBaseAdvancesAndOrigins", {runs[0], 0, 0, advances, origins}));
    static_cast<void>(f.call(ct, "_CTRunGetPositions", {runs[0], 0, 0, positions}));
    double pen = real(f, positions);
    for (Address index = 0; index < 5; ++index) {
        require(std::abs(real(f, positions + index * 16) - (pen + real(f, origins + index * 16))) < 1e-9, "positions from base advances and origins");
        pen += real(f, advances + index * 16);
    }
    require(real(f, advances) > 0, "base advances");

    // ---- cluster breaks with an offset ----
    const auto typesetter = f.call(ct, "_CTTypesetterCreateWithAttributedString", {string});
    alchemy::CallRegs registers{};
    registers.x[0] = typesetter;
    registers.set_d(0, 1000);
    registers.set_d(1, 20);
    require(f.regs(ct, "_CTTypesetterSuggestClusterBreakWithOffset", registers).x0 == 5, "a cluster break with an offset");
    std::filesystem::remove_all(directory, ignored);
});

} // namespace
