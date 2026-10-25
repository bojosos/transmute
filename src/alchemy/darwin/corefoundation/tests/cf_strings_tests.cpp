// CoreFoundation strings (cf_strings.cpp, cf_encodings.cpp) over the full
// provider set (fill_fixture.hpp): mutable strings and editing, case mapping,
// folding and ICU transforms, search, line bounds, splitting and joining,
// number scanning, Pascal strings, and the ICU-converted encodings with their
// names and code pages. Results returned in x1 or d0 (CFRange lengths,
// CFStringGetDoubleValue) are covered by the cf_strings guest fixture.
#include "alchemy/gui/tests/fill_fixture.hpp"

#include <array>
#include <vector>

namespace {
using namespace fill_test;

constexpr std::string_view cf = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";
constexpr Address utf8 = 0x08000100, ascii = 0x600, mac_roman = 0, shift_jis = 0x0a01, iso_latin2 = 0x0202,
    windows_latin1 = 0x0500, windows_cyrillic = 0x0502, dos_latin1 = 0x0410, dos_japanese = 0x0420, invalid_id = 0xffffffff;
constexpr Address not_found = static_cast<Address>(-1);

using Units = std::vector<std::uint16_t>;
Address u16(Fixture& f, std::initializer_list<std::uint16_t> units) { return f.cf.string_utf16(Units(units)); }
Address string(Fixture& f, std::string_view text) { return f.cf.string(text); }
Address bytes(Fixture& f, std::initializer_list<std::uint8_t> values) {
    const auto address = f.block(values.size());
    f.engine.write_bytes(address, std::vector<std::uint8_t>(values));
    return address;
}
Address mutable_string(Fixture& f, std::string_view text) {
    const auto result = f.call(cf, "_CFStringCreateMutable", {0, 0});
    static_cast<void>(f.call(cf, "_CFStringAppendCString", {result, f.text(text), utf8}));
    return result;
}
Address locale(Fixture& f, std::string_view identifier) { return f.call(cf, "_CFLocaleCreate", {0, string(f, identifier)}); }
bool yes(Address value) { return (value & 0xff) != 0; }

void editing(Fixture& f) {
    const auto text = f.call(cf, "_CFStringCreateMutable", {0, 0});
    require(f.call(cf, "_CFStringGetLength", {text}) == 0, "CFStringCreateMutable is empty");
    static_cast<void>(f.call(cf, "_CFStringAppendCString", {text, f.text("Hello"), utf8}));
    const auto space = bytes(f, {' ', 0});
    static_cast<void>(f.call(cf, "_CFStringAppendCharacters", {text, space, 1}));
    static_cast<void>(f.call(cf, "_CFStringAppend", {text, string(f, "World")}));
    require(f.cf.utf8(text) == "Hello World", "append C string, characters and string");
    static_cast<void>(f.call(cf, "_CFStringInsert", {text, 5, string(f, ",")}));
    require(f.cf.utf8(text) == "Hello, World", "CFStringInsert");
    static_cast<void>(f.call(cf, "_CFStringDelete", {text, 5, 1}));
    static_cast<void>(f.call(cf, "_CFStringReplace", {text, 0, 5, string(f, "Howdy")}));
    require(f.cf.utf8(text) == "Howdy World", "CFStringDelete and CFStringReplace");
    static_cast<void>(f.call(cf, "_CFStringReplaceAll", {text, string(f, "a-b-c-d")}));
    require(f.call(cf, "_CFStringFindAndReplace", {text, string(f, "-"), string(f, "+"), 0, 7, 0}) == 3 &&
                f.cf.utf8(text) == "a+b+c+d", "CFStringFindAndReplace");
    require(f.call(cf, "_CFStringFindAndReplace", {text, string(f, "+"), string(f, "--"), 0, 7, 4}) == 3 &&
                f.cf.utf8(text) == "a--b--c--d", "backwards find-and-replace");
    const auto mixed = mutable_string(f, "AbabAB");
    require(f.call(cf, "_CFStringFindAndReplace", {mixed, string(f, "ab"), string(f, "x"), 0, 6, 1}) == 3 &&
                f.cf.utf8(mixed) == "xxx", "case-insensitive find-and-replace");
    // va_list on Darwin arm64: a pointer to 8-byte argument slots.
    const auto arguments = f.block(8);
    f.engine.write64(arguments, 42);
    static_cast<void>(f.call(cf, "_CFStringAppendFormatAndArguments", {mixed, 0, string(f, " n=%d"), arguments}));
    require(f.cf.utf8(mixed) == "xxx n=42", "CFStringAppendFormatAndArguments");
    // Pascal strings.
    const auto pascal = bytes(f, {5, 'H', 'e', 'l', 'l', 'o'});
    const auto created = f.call(cf, "_CFStringCreateWithPascalString", {0, pascal, ascii});
    require(f.cf.utf8(created) == "Hello", "CFStringCreateWithPascalString");
    const auto out = f.block(16, 0xff);
    require(yes(f.call(cf, "_CFStringGetPascalString", {created, out, 16, ascii})) && f.memory().read8(out) == 5 &&
                f.memory().read8(out + 1) == 'H' && f.memory().read8(out + 5) == 'o', "CFStringGetPascalString");
    require(!yes(f.call(cf, "_CFStringGetPascalString", {created, out, 4, ascii})), "a Pascal buffer too small fails");
    require(f.call(cf, "_CFStringGetPascalStringPtr", {created, ascii}) == 0, "no inner Pascal storage");
    const auto appended = mutable_string(f, ">");
    static_cast<void>(f.call(cf, "_CFStringAppendPascalString", {appended, pascal, ascii}));
    require(f.cf.utf8(appended) == ">Hello", "CFStringAppendPascalString");
    // Mutating an immutable string stops explicitly.
    bool refused = false;
    try { static_cast<void>(f.call(cf, "_CFStringAppendCString", {string(f, "fixed"), f.text("!"), utf8})); }
    catch (const std::exception&) { refused = true; }
    require(refused, "appending to an immutable string stops");
}

void padding_and_trimming(Fixture& f) {
    const auto text = mutable_string(f, "abc");
    static_cast<void>(f.call(cf, "_CFStringPad", {text, string(f, "xy"), 7, 1}));
    require(f.cf.utf8(text) == "abcyxyx", "CFStringPad grows from the pad index");
    static_cast<void>(f.call(cf, "_CFStringPad", {text, 0, 2, 0}));
    require(f.cf.utf8(text) == "ab", "CFStringPad truncates");
    const auto dashes = mutable_string(f, "--a-b--");
    static_cast<void>(f.call(cf, "_CFStringTrim", {dashes, string(f, "-")}));
    require(f.cf.utf8(dashes) == "a-b", "CFStringTrim");
    const auto only = mutable_string(f, "----");
    static_cast<void>(f.call(cf, "_CFStringTrim", {only, string(f, "--")}));
    require(f.cf.utf8(only).empty(), "CFStringTrim of nothing but the trim string");
    const auto spaced = f.cf.mutable_copy(u16(f, {0x20, 0x09, 0x3000, 'a', 0x20, 'b', 0x0a, 0x2029}));
    static_cast<void>(f.call(cf, "_CFStringTrimWhitespace", {spaced}));
    require(f.cf.utf8(spaced) == "a b", "CFStringTrimWhitespace (Unicode spaces and newlines)");
}

void case_and_folding(Fixture& f) {
    const auto text = mutable_string(f, "HELLO wORLD");
    static_cast<void>(f.call(cf, "_CFStringLowercase", {text, 0}));
    require(f.cf.utf8(text) == "hello world", "CFStringLowercase");
    static_cast<void>(f.call(cf, "_CFStringCapitalize", {text, 0}));
    require(f.cf.utf8(text) == "Hello World", "CFStringCapitalize");
    const auto strasse = f.cf.mutable_copy(u16(f, {'s', 't', 'r', 'a', 0x00df, 'e'}));
    static_cast<void>(f.call(cf, "_CFStringUppercase", {strasse, 0}));
    require(f.cf.utf8(strasse) == "STRASSE", "CFStringUppercase expands the sharp s");
    const auto dotted = mutable_string(f, "i");
    static_cast<void>(f.call(cf, "_CFStringUppercase", {dotted, locale(f, "tr_TR")}));
    require(f.cf.units(dotted) == Units{0x0130}, "Turkish upper-case dotted i");
    const auto accented = f.cf.mutable_copy(u16(f, {0x00c1, 'b', 'C', 0xff21}));
    static_cast<void>(f.call(cf, "_CFStringFold", {accented, 1 | 128 | 256, 0}));
    require(f.cf.utf8(accented) == "abca", "CFStringFold: case, diacritics and width");
}

void transforms(Fixture& f) {
    require(f.string_constant(cf, "_kCFStringTransformToLatin") == "Any-Latin", "kCFStringTransformToLatin");
    require(f.string_constant(cf, "_kCFStringTransformStripCombiningMarks") == "NFD; [:Mn:] Remove; NFC", "strip combining marks");
    const auto russian = f.cf.mutable_copy(u16(f, {0x041f, 0x0440, 0x0438, 0x0432, 0x0435, 0x0442}));
    require(yes(f.call(cf, "_CFStringTransform", {russian, 0, f.engine.read64(f.symbol(cf, "_kCFStringTransformToLatin")), 0})) &&
                f.cf.utf8(russian) == "Privet", "Cyrillic to Latin");
    const auto cafe = f.cf.mutable_copy(u16(f, {'c', 'a', 'f', 0x00e9}));
    require(yes(f.call(cf, "_CFStringTransform", {cafe, 0, f.engine.read64(f.symbol(cf, "_kCFStringTransformStripDiacritics")), 0})) &&
                f.cf.utf8(cafe) == "cafe", "strip diacritics");
    const auto kana = mutable_string(f, "ka");
    const auto katakana = f.engine.read64(f.symbol(cf, "_kCFStringTransformLatinKatakana"));
    require(yes(f.call(cf, "_CFStringTransform", {kana, 0, katakana, 0})) && f.cf.units(kana) == Units{0x30ab}, "Latin to Katakana");
    require(yes(f.call(cf, "_CFStringTransform", {kana, 0, katakana, 1})) && f.cf.utf8(kana) == "ka", "reverse transform");
    const auto range = f.block(16);
    f.engine.write64(range, 1);
    f.engine.write64(range + 8, 1);
    const auto partial = f.cf.mutable_copy(u16(f, {0x00e9, 0x00e9, 0x00e9}));
    require(yes(f.call(cf, "_CFStringTransform", {partial, range, string(f, "Latin-ASCII"), 0})) &&
                f.cf.units(partial) == Units{0x00e9, 'e', 0x00e9} && f.engine.read64(range + 8) == 1, "transform of a range");
    require(!yes(f.call(cf, "_CFStringTransform", {partial, 0, string(f, "No-Such-Transform"), 0})), "an unknown transform fails");
}

void search(Fixture& f) {
    const auto hello = string(f, "Hello World");
    require(f.call(cf, "_CFStringFind", {hello, string(f, "World"), 0}) == 6, "CFStringFind");
    require(f.call(cf, "_CFStringFind", {hello, string(f, "world"), 1}) == 6, "case-insensitive CFStringFind");
    require(f.call(cf, "_CFStringFind", {hello, string(f, "planet"), 0}) == not_found, "CFStringFind without a match");
    const auto result = f.block(16);
    require(yes(f.call(cf, "_CFStringFindWithOptionsAndLocale", {hello, string(f, "HELLO"), 0, 11, 1, locale(f, "en_US"), result})) &&
                f.engine.read64(result) == 0 && f.engine.read64(result + 8) == 5, "CFStringFindWithOptionsAndLocale");
    require(yes(f.call(cf, "_CFStringFindWithOptions", {hello, string(f, "o"), 0, 11, 4, result})) && f.engine.read64(result) == 7,
            "CFStringFindWithOptions backwards");
    const auto digits = f.call(cf, "_CFCharacterSetGetPredefined", {4});
    const auto mixed = string(f, "abc 123");
    require(yes(f.call(cf, "_CFStringFindCharacterFromSet", {mixed, digits, 0, 7, 0, result})) && f.engine.read64(result) == 4 &&
                f.engine.read64(result + 8) == 1, "CFStringFindCharacterFromSet");
    require(yes(f.call(cf, "_CFStringFindCharacterFromSet", {mixed, digits, 0, 7, 4, result})) && f.engine.read64(result) == 6,
            "CFStringFindCharacterFromSet backwards");
    require(!yes(f.call(cf, "_CFStringFindCharacterFromSet", {mixed, digits, 0, 7, 8, result})), "anchored set search");
    require(static_cast<std::int64_t>(f.call(cf, "_CFStringCompareWithOptionsAndLocale", {string(f, "a"), string(f, "B"), 0, 1, 1, locale(f, "en_US")})) == -1,
            "localized case-insensitive comparison");
    require(static_cast<std::int64_t>(f.call(cf, "_CFStringCompareWithOptionsAndLocale", {string(f, "a"), string(f, "B"), 0, 1, 0, 0})) == 1,
            "literal comparison without a locale");
    require(f.call(cf, "_CFStringGetRangeOfComposedCharactersAtIndex", {u16(f, {'x', 'e', 0x0301, 'y'}), 2}) == 1,
            "a combining mark belongs to its base character");
}

void lines(Fixture& f) {
    const auto text = u16(f, {'a', 'b', 0x0d, 0x0a, 'c', 'd', 0x0a, 'e', 'f'});
    const auto begin = f.block(8), end = f.block(8), contents = f.block(8);
    static_cast<void>(f.call(cf, "_CFStringGetLineBounds", {text, 5, 0, begin, end, contents}));
    require(f.engine.read64(begin) == 4 && f.engine.read64(end) == 7 && f.engine.read64(contents) == 6, "line bounds");
    static_cast<void>(f.call(cf, "_CFStringGetLineBounds", {text, 0, 1, begin, end, contents}));
    require(f.engine.read64(begin) == 0 && f.engine.read64(end) == 4 && f.engine.read64(contents) == 2, "CR LF ends a line");
    static_cast<void>(f.call(cf, "_CFStringGetLineBounds", {text, 8, 1, begin, end, contents}));
    require(f.engine.read64(begin) == 7 && f.engine.read64(end) == 9 && f.engine.read64(contents) == 9, "the last line");
    const auto separated = u16(f, {'a', 'b', 0x2028, 'c', 'd'});
    static_cast<void>(f.call(cf, "_CFStringGetLineBounds", {separated, 4, 0, begin, 0, 0}));
    require(f.engine.read64(begin) == 3, "U+2028 separates lines");
    static_cast<void>(f.call(cf, "_CFStringGetParagraphBounds", {separated, 4, 0, begin, end, 0}));
    require(f.engine.read64(begin) == 0 && f.engine.read64(end) == 5, "but not paragraphs");
}

void splitting_and_scanning(Fixture& f) {
    const auto parts = f.call(cf, "_CFStringCreateArrayBySeparatingStrings", {0, string(f, "a,b,,c"), string(f, ",")});
    const auto values = f.cf.array_values(parts);
    require(values.size() == 4 && f.cf.utf8(values[0]) == "a" && f.cf.utf8(values[2]).empty() && f.cf.utf8(values[3]) == "c",
            "CFStringCreateArrayBySeparatingStrings");
    require(f.cf.array_values(f.call(cf, "_CFStringCreateArrayBySeparatingStrings", {0, string(f, "abc"), string(f, ",")})).size() == 1,
            "no separator: one element");
    require(f.cf.utf8(f.call(cf, "_CFStringCreateByCombiningStrings", {0, parts, string(f, "-")})) == "a-b--c",
            "CFStringCreateByCombiningStrings");
    const auto value = [&](std::string_view text) {
        return static_cast<std::int32_t>(f.call(cf, "_CFStringGetIntValue", {string(f, text)}));
    };
    require(value("  -42abc") == -42 && value("+ 7") == 7 && value("abc") == 0, "CFStringGetIntValue");
    require(value("99999999999") == 2147483647 && value("-99999999999") == -2147483647 - 1, "CFStringGetIntValue clamps");
}

void encodings(Fixture& f) {
    // Mac OS Roman, the system encoding, converts through the host's ICU.
    const auto system = f.call(cf, "_CFStringGetSystemEncoding");
    require(system == mac_roman, "the system encoding is Mac OS Roman");
    const auto roman = f.call(cf, "_CFStringCreateWithBytes", {0, bytes(f, {'c', 'a', 'f', 0x8e}), 4, mac_roman, 0});
    require(roman && f.cf.units(roman) == Units{'c', 'a', 'f', 0x00e9}, "Mac OS Roman to UTF-16");
    // (CFStringGetBytes takes nine arguments, more than this engine passes in registers; its
    // conversions are those of CFStringGetCString and CFStringCreateExternalRepresentation.)
    const auto buffer = f.block(16);
    require(yes(f.call(cf, "_CFStringGetCString", {roman, buffer, 16, mac_roman})) && f.memory().read8(buffer + 3) == 0x8e &&
                f.memory().read8(buffer + 4) == 0, "UTF-16 to Mac OS Roman");
    require(f.call(cf, "_CFStringCreateWithCString", {0, f.text("plain"), system}) != 0, "C strings in the system encoding");
    // Shift JIS, both ways, and invalid input.
    const auto hiragana = f.call(cf, "_CFStringCreateWithBytes", {0, bytes(f, {0x82, 0xa0, 'A'}), 3, shift_jis, 0});
    require(hiragana && f.cf.units(hiragana) == Units{0x3042, 'A'}, "Shift JIS to UTF-16");
    require(yes(f.call(cf, "_CFStringGetCString", {hiragana, buffer, 16, shift_jis})) && f.memory().read8(buffer) == 0x82 &&
                f.memory().read8(buffer + 1) == 0xa0 && f.memory().read8(buffer + 2) == 'A' && f.memory().read8(buffer + 3) == 0,
            "UTF-16 to Shift JIS");
    require(f.call(cf, "_CFStringCreateWithBytes", {0, bytes(f, {0x82}), 1, shift_jis, 0}) == 0, "a truncated Shift JIS character");
    // A character the encoding lacks: the loss byte, or the conversion ends there.
    const auto han = u16(f, {'a', 0x4e00, 'b'});
    const auto lossy = f.call(cf, "_CFStringCreateExternalRepresentation", {0, han, mac_roman, '?'});
    require(lossy && f.call(cf, "_CFDataGetLength", {lossy}) == 3 && f.memory().read8(f.call(cf, "_CFDataGetBytePtr", {lossy}) + 1) == '?',
            "lossy conversion");
    require(!yes(f.call(cf, "_CFStringGetCString", {han, buffer, 16, mac_roman})), "conversion fails at a missing character");
    require(f.call(cf, "_CFStringGetMaximumSizeForEncoding", {10, shift_jis}) >= 20, "maximum size for Shift JIS");
    require(yes(f.call(cf, "_CFStringIsEncodingAvailable", {shift_jis})) && yes(f.call(cf, "_CFStringIsEncodingAvailable", {mac_roman})),
            "ICU encodings are available");
    require(f.cf.utf8(f.call(cf, "_CFStringGetNameOfEncoding", {shift_jis})) == "Japanese (Shift JIS)" &&
                f.cf.utf8(f.call(cf, "_CFStringGetNameOfEncoding", {mac_roman})) == "Western (Mac OS Roman)", "encoding names");
    // NSStringEncoding values.
    require(f.call(cf, "_CFStringConvertEncodingToNSStringEncoding", {utf8}) == 4 &&
                f.call(cf, "_CFStringConvertEncodingToNSStringEncoding", {mac_roman}) == 30 &&
                f.call(cf, "_CFStringConvertEncodingToNSStringEncoding", {shift_jis}) == 0x80000a01, "CF to NSStringEncoding");
    require(f.call(cf, "_CFStringConvertNSStringEncodingToEncoding", {8}) == dos_japanese &&
                f.call(cf, "_CFStringConvertNSStringEncodingToEncoding", {0x80000a01}) == shift_jis &&
                f.call(cf, "_CFStringConvertNSStringEncodingToEncoding", {99}) == invalid_id, "NSStringEncoding to CF");
    // Windows code pages.
    require(f.call(cf, "_CFStringConvertEncodingToWindowsCodepage", {windows_latin1}) == 1252 &&
                f.call(cf, "_CFStringConvertEncodingToWindowsCodepage", {iso_latin2}) == 28592, "CF to Windows code pages");
    require(f.call(cf, "_CFStringConvertWindowsCodepageToEncoding", {932}) == dos_japanese &&
                f.call(cf, "_CFStringConvertWindowsCodepageToEncoding", {28595}) == 0x0205 &&
                f.call(cf, "_CFStringConvertWindowsCodepageToEncoding", {99999}) == invalid_id, "Windows code pages to CF");
    // IANA names.
    const auto iana = [&](Address encoding) { return f.cf.utf8(f.call(cf, "_CFStringConvertEncodingToIANACharSetName", {encoding})); };
    require(iana(utf8) == "utf-8" && iana(1) == "x-mac-japanese" && iana(iso_latin2) == "iso-8859-2" && iana(dos_latin1) == "cp850" &&
                iana(windows_cyrillic) == "windows-1251" && iana(mac_roman) == "macintosh", "IANA names");
    const auto from_iana = [&](std::string_view name) { return f.call(cf, "_CFStringConvertIANACharSetNameToEncoding", {string(f, name)}); };
    require(from_iana("Shift_JIS") == shift_jis && from_iana("ISO-8859-1") == 0x0201 && from_iana("x-mac-japanese") == 1 &&
                from_iana("macintosh") == mac_roman && from_iana("UTF8") == utf8 && from_iana("not-a-charset") == invalid_id,
            "IANA names to CF (CF's own, then ICU's aliases)");
    require(f.call(cf, "_CFStringGetMostCompatibleMacStringEncoding", {shift_jis}) == 1 &&
                f.call(cf, "_CFStringGetMostCompatibleMacStringEncoding", {iso_latin2}) == 29 &&
                f.call(cf, "_CFStringGetMostCompatibleMacStringEncoding", {windows_cyrillic}) == 7 &&
                f.call(cf, "_CFStringGetMostCompatibleMacStringEncoding", {utf8}) == 0x100, "most compatible Mac encoding");
    const auto list = f.call(cf, "_CFStringGetListOfAvailableEncodings");
    bool has_utf8 = false, has_sjis = false;
    std::size_t count = 0;
    for (Address at = list;; at += 4, ++count) {
        const auto encoding = f.engine.read32(at);
        if (encoding == invalid_id || count > 1000) break;
        has_utf8 = has_utf8 || encoding == utf8;
        has_sjis = has_sjis || encoding == shift_jis;
    }
    require(count < 1000 && has_utf8 && has_sjis && list == f.call(cf, "_CFStringGetListOfAvailableEncodings"),
            "the encoding list is terminated and shared");
    require(f.call(cf, "_CFStringGetFastestEncoding", {string(f, "abc")}) == ascii &&
                f.call(cf, "_CFStringGetFastestEncoding", {roman}) == 0x100, "fastest encoding");
    require(f.call(cf, "_CFStringGetSmallestEncoding", {roman}) == mac_roman &&
                f.call(cf, "_CFStringGetSmallestEncoding", {han}) == 0x100, "smallest encoding");
    const auto external = f.call(cf, "_CFDataCreate", {0, bytes(f, {0xff, 0xfe, 'h', 0, 'i', 0}), 6});
    require(f.cf.utf8(f.call(cf, "_CFStringCreateFromExternalRepresentation", {0, external, 0x100})) == "hi",
            "CFStringCreateFromExternalRepresentation reads the byte-order mark");
}

void diagnostics(Fixture& f) {
    static_cast<void>(f.call(cf, "_CFShow", {string(f, "shown")}));
    static_cast<void>(f.call(cf, "_CFShow", {f.call(cf, "_CFArrayCreate", {0, 0, 0, f.symbol(cf, "_kCFTypeArrayCallBacks")})}));
    static_cast<void>(f.call(cf, "_CFShowStr", {string(f, "shown")}));
    require(f.string_constant(cf, "__kCFSystemVersionProductNameKey") == "ProductName", "SystemVersion.plist keys");
}

} // namespace

int main() {
    return run("corefoundation strings", {{"editing", editing}, {"padding and trimming", padding_and_trimming},
        {"case and folding", case_and_folding}, {"transforms", transforms}, {"search", search}, {"lines", lines},
        {"splitting and scanning", splitting_and_scanning}, {"encodings", encodings}, {"diagnostics", diagnostics}});
}
