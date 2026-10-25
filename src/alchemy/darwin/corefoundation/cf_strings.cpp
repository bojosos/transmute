#include "alchemy/darwin/corefoundation/cf_encodings.hpp"
#include "alchemy/darwin/headless_provider.hpp"

#include "alchemy/host/icu.hpp"
#include "alchemy/host/locale.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

// CoreFoundation strings (headless-correct, every build): mutable strings and
// their editing calls (append, insert, delete, replace, find-and-replace, pad,
// trim, case mapping, folding, ICU transforms), search (CFStringFind*,
// character sets, composed characters, line and paragraph bounds), splitting
// and joining, number scanning, Pascal strings, the encoding conversions
// (NSStringEncoding, Windows code pages, IANA names, the encoding list) and
// CFShow/CFLog. Strings of the object store are edited in place; any other
// NSString (a guest subclass) goes through its NSString/NSMutableString
// methods, as CF dispatches to Objective-C. Case mapping, folding, transforms
// and character clusters use the host's ICU (host::icu); linguistic search and
// collation use host::locale. CFStringFind's and CFStringGetLineBounds' rules
// follow swift-corelibs-foundation's CFString.c and CFStringScanner.c
// (Apache-2.0, third_party/swift-corelibs-foundation).
namespace alchemy::darwin_headless {
namespace {

using Units = std::vector<std::uint16_t>;
constexpr std::int64_t not_found = -1;   // kCFNotFound

// CFStringCompareFlags (CFString.h).
enum : std::uint64_t {
    compare_case_insensitive = 1, compare_backwards = 4, compare_anchored = 8, compare_nonliteral = 16,
    compare_localized = 32, compare_numerically = 64, compare_diacritic_insensitive = 128,
    compare_width_insensitive = 256, compare_forced_ordering = 512,
};

// CFStringTransform identifiers: each constant's value is the ICU transform it
// names (swift-corelibs-foundation's table), so any other ICU transform ID works
// as well. On macOS kCFStringTransformToLatin reads "Any-Latin"; that the other
// constants also carry their ICU IDs as their values is unverified.
struct TransformName { const char* symbol; const char* id; };
constexpr TransformName transform_names[] = {
    {"_kCFStringTransformStripCombiningMarks", "NFD; [:Mn:] Remove; NFC"},
    {"_kCFStringTransformToLatin", "Any-Latin"},
    {"_kCFStringTransformFullwidthHalfwidth", "Fullwidth-Halfwidth"},
    {"_kCFStringTransformLatinKatakana", "Latin-Katakana"},
    {"_kCFStringTransformLatinHiragana", "Latin-Hiragana"},
    {"_kCFStringTransformHiraganaKatakana", "Hiragana-Katakana"},
    {"_kCFStringTransformMandarinLatin", "Han-Latin"},
    {"_kCFStringTransformLatinHangul", "Latin-Hangul"},
    {"_kCFStringTransformLatinArabic", "Latin-Arabic"},
    {"_kCFStringTransformLatinHebrew", "Latin-Hebrew"},
    {"_kCFStringTransformLatinThai", "Latin-Thai"},
    {"_kCFStringTransformLatinCyrillic", "Latin-Cyrillic"},
    {"_kCFStringTransformLatinGreek", "Latin-Greek"},
    {"_kCFStringTransformToXMLHex", "[^[:ASCII:]-[:C:]]; Any-Hex/XML"},
    {"_kCFStringTransformToUnicodeName", "[^[:ASCII:]-[:C:]]; Any-Name"},
};
// kCFStringTransformStripDiacritics; its set ends with the escape of U+0338,
// built here so the source carries no escape sequence.
std::string strip_diacritics_id() {
    return std::string("NFD; [[[:Mn:]&[:Diacritic:]]-[:Grapheme_Link:]-[:ccc=0:]-[:ccc=8:]-[") + char(92) +
        "u0338]] Remove; NFC";
}

std::u16string_view view(const Units& units, std::size_t at = 0, std::size_t length = std::u16string_view::npos) {
    return std::u16string_view(reinterpret_cast<const char16_t*>(units.data()), units.size()).substr(at, length);
}
Units from_u16(std::u16string_view text) { return Units(text.begin(), text.end()); }

// White_Space plus the line and paragraph separators (kCFUniCharWhitespaceAndNewlineCharacterSet).
bool whitespace_or_newline(std::uint16_t c) {
    return c == 0x20 || (c >= 0x09 && c <= 0x0d) || c == 0x85 || c == 0xa0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200a) ||
        c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000;
}
// __CFIsWhitespace (ForFoundationOnly.h): the scanner's notion of space.
bool scanner_space(std::uint16_t c) {
    return c < 0x21 || (c > 0x7e && c < 0xa1) || (c >= 0x2000 && c <= 0x200b) || c == 0x3000;
}
bool line_separator(std::uint16_t c, bool lines) {
    return c == 0x0a || c == 0x0d || c == 0x2029 || (lines && (c == 0x85 || c == 0x2028));
}

// A CFLocale's identifier as ICU and host::locale take it ("" for NULL: the root locale).
std::string icu_locale(const std::string& identifier) { return identifier; }
std::string bcp47(std::string identifier) {
    std::replace(identifier.begin(), identifier.end(), '_', '-');
    return identifier.empty() ? "und" : identifier;
}

const host::icu::Api& icu() {
    const auto* api = host::icu::api();
    if (!api) throw std::runtime_error("Alchemy CoreFoundation: this string operation needs the host's ICU");
    return *api;
}
// An ICU string function into a buffer that grows until the result fits.
template <class F>
Units icu_string(const Units& input, F call) {
    Units output(input.size() + input.size() / 2 + 16);
    for (int attempt = 0; attempt < 2; ++attempt) {
        host::icu::UErrorCode error = host::icu::zero_error;
        const auto length = call(reinterpret_cast<host::icu::UChar*>(output.data()), static_cast<std::int32_t>(output.size()),
            reinterpret_cast<const host::icu::UChar*>(input.data()), static_cast<std::int32_t>(input.size()), &error);
        if (error == host::icu::buffer_overflow_error) { output.resize(static_cast<std::size_t>(length)); continue; }
        if (host::icu::failure(error)) throw std::runtime_error(std::string("Alchemy CoreFoundation: ICU string operation failed (") +
            (icu().u_errorName ? icu().u_errorName(error) : "error") + ")");
        output.resize(static_cast<std::size_t>(length));
        return output;
    }
    throw std::runtime_error("Alchemy CoreFoundation: ICU string operation did not converge");
}

// An ICU transliteration of `text` (utrans_transUChars in place).
std::optional<Units> transliterate(const std::u16string& id, bool reverse, Units text) {
    const auto& api = icu();
    host::icu::UErrorCode error = host::icu::zero_error;
    const auto transform = api.utrans_openU(id.data(), static_cast<std::int32_t>(id.size()),
        reverse ? host::icu::trans_reverse : host::icu::trans_forward, nullptr, 0, nullptr, &error);
    if (!transform || host::icu::failure(error)) return std::nullopt;
    std::optional<Units> result;
    for (std::size_t capacity = text.size() * 2 + 16; capacity < (1u << 28); capacity *= 2) {
        Units buffer(capacity);
        std::copy(text.begin(), text.end(), buffer.begin());
        auto length = static_cast<std::int32_t>(text.size()), limit = length;
        error = host::icu::zero_error;
        api.utrans_transUChars(transform, reinterpret_cast<host::icu::UChar*>(buffer.data()), &length,
            static_cast<std::int32_t>(capacity), 0, &limit, &error);
        if (error == host::icu::buffer_overflow_error) continue;
        if (!host::icu::failure(error)) { buffer.resize(static_cast<std::size_t>(length)); result = std::move(buffer); }
        break;
    }
    api.utrans_close(transform);
    return result;
}

class Strings final : public Provider {
public:
    using Provider::Provider;
    std::mutex mutex;
    std::map<std::uint32_t, Address> iana_names;
    Address encoding_list{};

    bool store_string(Address object) {
        const auto kind = object ? cf.kind(object) : std::nullopt;
        return kind && *kind == ObjectKind::string;
    }
    [[noreturn]] static void fail(const std::string& why) { throw std::runtime_error("Alchemy CoreFoundation: " + why); }
    static void require(Address object, const char* function) {
        if (!object) fail(std::string(function) + " of NULL");
    }
    static std::pair<std::size_t, std::size_t> checked_range(const Units& units, Address location, Address length, const char* function) {
        if (location > units.size() || length > units.size() - location) fail(std::string(function) + ": range outside the string");
        return {static_cast<std::size_t>(location), static_cast<std::size_t>(length)};
    }

    // The characters of any string: the object store's own, or an NSString's (-length, -getCharacters:range:).
    Units read(Address object) {
        if (store_string(object)) return cf.units(object);
        if (!object) fail("NULL string");
        const auto length = send(object, "length");
        if (length > (1ull << 30)) fail("string too long");
        Units units(static_cast<std::size_t>(length));
        if (length) {
            const auto buffer = context.allocate_guest(length * 2);
            CallRegs registers;
            registers.x[2] = buffer;
            registers.x[3] = 0;
            registers.x[4] = length;
            static_cast<void>(objc.send(object, "getCharacters:range:", std::move(registers)));
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length * 2));
            memory().read_bytes(buffer, bytes);
            context.release_guest(buffer);
            for (std::size_t at = 0; at < units.size(); ++at) units[at] = static_cast<std::uint16_t>(bytes[at * 2] | (bytes[at * 2 + 1] << 8));
        }
        return units;
    }
    // Replaces every character of a mutable string.
    void write(Address object, Units units) {
        if (store_string(object)) { cf.replace_string_units(object, std::move(units)); return; }
        if (!object) fail("mutation of a NULL string");
        const auto text = cf.string_utf16(units);
        static_cast<void>(send(object, "setString:", {text}));
        context.objects().release(text);
    }
    // Replaces `length` characters at `location` (CFStringReplace's primitive).
    void replace(Address object, std::size_t location, std::size_t length, const Units& with) {
        if (store_string(object)) {
            auto units = cf.units(object);
            if (location > units.size() || length > units.size() - location) fail("range outside the string");
            units.erase(units.begin() + static_cast<std::ptrdiff_t>(location), units.begin() + static_cast<std::ptrdiff_t>(location + length));
            units.insert(units.begin() + static_cast<std::ptrdiff_t>(location), with.begin(), with.end());
            cf.replace_string_units(object, std::move(units));
            return;
        }
        if (!object) fail("mutation of a NULL string");
        const auto text = cf.string_utf16(with);
        CallRegs registers;
        registers.x[2] = location;
        registers.x[3] = length;
        registers.x[4] = text;
        static_cast<void>(objc.send(object, "replaceCharactersInRange:withString:", std::move(registers)));
        context.objects().release(text);
    }
    void append(Address object, const Units& with) {
        const auto length = store_string(object) ? cf.length(object) : send(object, "length");
        replace(object, static_cast<std::size_t>(length), 0, with);
    }
    std::string locale_identifier(Address locale) {
        if (!locale) return {};
        const auto identifier = cf.invoke("CFLocaleGetIdentifier", std::array<Address, 1>{locale});
        return identifier ? cf.utf8(identifier) : std::string();
    }
    // A C string in a CF encoding (CFStringCreateWithCString semantics).
    std::optional<Units> decode(Address bytes, std::size_t length, std::uint32_t encoding) {
        const auto text = cf.invoke("CFStringCreateWithBytes", std::array<Address, 5>{0, bytes, length, encoding, 0});
        if (!text) return std::nullopt;
        auto units = cf.units(text);
        context.objects().release(text);
        return units;
    }
    std::size_t c_length(Address text) {
        bool terminated{};
        const auto bytes = memory().read_string(text, 1, 1u << 30, &terminated);
        if (!terminated) fail("unterminated C string");
        return bytes.size();
    }

    // CFStringFindWithOptionsAndLocale over characters: the match in [location, location + length).
    std::optional<std::pair<std::size_t, std::size_t>> find(const Units& haystack, const Units& needle, std::size_t location,
                                                            std::size_t length, std::uint64_t options, const std::string& locale) {
        if (needle.empty() || needle.size() > length) return std::nullopt;
        const bool backwards = options & compare_backwards, anchored = options & compare_anchored;
        if (!(options & (compare_case_insensitive | compare_nonliteral | compare_diacritic_insensitive | compare_width_insensitive))) {
            const auto matches = [&](std::size_t at) { return std::equal(needle.begin(), needle.end(), haystack.begin() + static_cast<std::ptrdiff_t>(at)); };
            const auto last = location + length - needle.size();
            if (anchored) {
                const auto at = backwards ? last : location;
                if (matches(at)) return std::pair{at, needle.size()};
                return std::nullopt;
            }
            if (backwards) {
                for (auto at = last + 1; at-- > location;) if (matches(at)) return std::pair{at, needle.size()};
                return std::nullopt;
            }
            const auto begin = haystack.begin() + static_cast<std::ptrdiff_t>(location);
            const auto hit = std::search(begin, begin + static_cast<std::ptrdiff_t>(length), needle.begin(), needle.end());
            if (hit == begin + static_cast<std::ptrdiff_t>(length)) return std::nullopt;
            return std::pair{static_cast<std::size_t>(hit - haystack.begin()), needle.size()};
        }
        namespace locale_api = host::locale;
        std::uint32_t flags = 0;
        if (options & compare_case_insensitive) flags |= locale_api::ignore_case_linguistic;
        if (options & compare_diacritic_insensitive) flags |= locale_api::ignore_diacritics;
        if (options & compare_width_insensitive) flags |= locale_api::ignore_width;
        const auto where = backwards ? (anchored ? locale_api::Search::suffix : locale_api::Search::last)
                                     : (anchored ? locale_api::Search::prefix : locale_api::Search::first);
        const auto match = locale_api::find(bcp47(locale), view(haystack, location, length), view(needle), where, flags);
        if (match) return std::pair{location + match->offset, match->length};
        if (match.error() == host::Error::unsupported) fail("CFStringFind with folding options needs the host's linguistic search");
        return std::nullopt;
    }
    // Every match in the range, in increasing order (CFStringCreateArrayWithFindResults' loop).
    std::vector<std::pair<std::size_t, std::size_t>> find_all(const Units& haystack, const Units& needle, std::size_t location,
                                                              std::size_t length, std::uint64_t options, const std::string& locale) {
        std::vector<std::pair<std::size_t, std::size_t>> found;
        const bool backwards = options & compare_backwards;
        const auto end = location + length;
        while (length > 0) {
            const auto match = find(haystack, needle, location, length, options, locale);
            if (!match) break;
            found.push_back(*match);
            if (backwards) length = match->first - location;
            else { location = match->first + match->second; length = end - location; }
            if (match->second == 0) break;
        }
        if (backwards) std::reverse(found.begin(), found.end());
        return found;
    }

    void write_range(Address out, std::int64_t location, std::int64_t length) {
        if (!out) return;
        memory().write64(out, static_cast<std::uint64_t>(location));
        memory().write64(out + 8, static_cast<std::uint64_t>(length));
    }

    // Lower, upper or title case in a locale (u_strTo*, or the host's case maps without ICU).
    Units mapped(const Units& units, const std::string& locale, int which) {
        if (const auto* api = host::icu::api()) {
            const auto id = icu_locale(locale);
            if (which == 2) return icu_string(units, [&](auto* d, auto c, auto* s, auto l, auto* e) { return api->u_strToTitle(d, c, s, l, nullptr, id.c_str(), e); });
            if (which == 1) return icu_string(units, [&](auto* d, auto c, auto* s, auto l, auto* e) { return api->u_strToUpper(d, c, s, l, id.c_str(), e); });
            return icu_string(units, [&](auto* d, auto c, auto* s, auto l, auto* e) { return api->u_strToLower(d, c, s, l, id.c_str(), e); });
        }
        if (which == 2) fail("CFStringCapitalize needs the host's ICU");
        using host::locale::Mapping;
        const auto result = host::locale::map(bcp47(locale), view(units), which == 1 ? Mapping::linguistic_upper : Mapping::linguistic_lower);
        if (!result) fail("case mapping failed");
        return from_u16(*result);
    }
    // CFStringFold: case folding, then diacritics, then width.
    Units folded(Units units, std::uint64_t flags, const std::string& locale) {
        const auto& api = icu();
        if (flags & compare_case_insensitive) {
            const auto language = locale.substr(0, locale.find_first_of("_-"));
            const auto options = language == "tr" || language == "az" ? host::icu::fold_case_exclude_special_i : host::icu::fold_case_default;
            units = icu_string(units, [&](auto* d, auto c, auto* s, auto l, auto* e) { return api.u_strFoldCase(d, c, s, l, options, e); });
        }
        if (flags & compare_diacritic_insensitive) {
            auto stripped = transliterate(u"NFD; [:Mn:] Remove; NFC", false, units);
            if (!stripped) fail("CFStringFold: the host's ICU has no diacritic transform");
            units = std::move(*stripped);
        }
        if (flags & compare_width_insensitive) {
            auto narrow = transliterate(u"Fullwidth-Halfwidth", false, units);
            if (!narrow) fail("CFStringFold: the host's ICU has no width transform");
            units = std::move(*narrow);
        }
        return units;
    }

    // __CFStringGetLineOrParagraphBounds.
    void line_bounds(CpuState& s, bool lines) {
        require(s.x[0], lines ? "CFStringGetLineBounds" : "CFStringGetParagraphBounds");
        const auto units = read(s.x[0]);
        const auto [location, length] = checked_range(units, s.x[1], s.x[2], "CFStringGetLineBounds");
        const auto at = [&](std::int64_t index) -> std::uint16_t {
            return index >= 0 && static_cast<std::size_t>(index) < units.size() ? units[static_cast<std::size_t>(index)] : 0xffff;
        };
        if (s.x[3]) {
            std::int64_t start = 0;
            if (location != 0) {
                auto index = static_cast<std::int64_t>(location);
                const auto first = at(index);
                --index;
                if (first == 0x0a && at(index) == 0x0d) --index;
                while (true) {
                    if (index < 0) { start = 0; break; }
                    if (line_separator(at(index), lines)) { start = index + 1; break; }
                    --index;
                }
            }
            memory().write64(s.x[3], static_cast<std::uint64_t>(start));
        }
        if (s.x[4] || s.x[5]) {
            std::int64_t contents_end{}, separator = 1;
            auto index = static_cast<std::int64_t>(location + length) - (length ? 1 : 0);
            auto c = at(index);
            if (c == 0x0a) {
                contents_end = index;
                --index;
                if (at(index) == 0x0d) { separator = 2; --contents_end; }
            } else {
                while (true) {
                    if (line_separator(c, lines)) {
                        contents_end = index;
                        ++index;
                        if (c == 0x0d && at(index) == 0x0a) separator = 2;
                        break;
                    }
                    if (index >= static_cast<std::int64_t>(units.size())) { contents_end = static_cast<std::int64_t>(units.size()); separator = 0; break; }
                    ++index;
                    c = at(index);
                }
            }
            if (s.x[5]) memory().write64(s.x[5], static_cast<std::uint64_t>(contents_end));
            if (s.x[4]) memory().write64(s.x[4], static_cast<std::uint64_t>(contents_end + separator));
        }
    }

    Address iana_name(std::uint32_t encoding) {
        std::lock_guard lock(mutex);
        if (const auto found = iana_names.find(encoding); found != iana_names.end()) return found->second;
        const auto name = cf_encodings::iana_name(encoding);
        const auto object = name ? constant_string(*name) : 0;
        iana_names.emplace(encoding, object);
        return object;
    }
    Address available_encodings() {
        std::lock_guard lock(mutex);
        if (encoding_list) return encoding_list;
        auto list = cf_encodings::available();
        list.push_back(cf_encodings::invalid_id);
        const auto address = context.allocate_guest(list.size() * 4);
        std::vector<std::uint8_t> bytes(list.size() * 4);
        for (std::size_t at = 0; at < list.size(); ++at)
            for (unsigned byte = 0; byte < 4; ++byte) bytes[at * 4 + byte] = static_cast<std::uint8_t>(list[at] >> (8 * byte));
        memory().write_bytes(address, bytes);
        encoding_list = address;
        return address;
    }

    void install();
    void install_editing();
    void install_search();
    void install_encodings();
};

void Strings::install() {
    install_editing();
    install_search();
    install_encodings();
    for (const auto& [symbol, id] : transform_names) string_symbol(core_foundation_library, symbol, id);
    string_symbol(core_foundation_library, "_kCFStringTransformStripDiacritics", strip_diacritics_id());

    // void CFShow(CFTypeRef): a string's characters, otherwise the description, on stderr.
    function(core_foundation_library, "_CFShow", [this](CpuState& s, GuestMemory&) {
        std::string text = "(null)";
        if (s.x[0]) {
            if (store_string(s.x[0])) text = cf.utf8(s.x[0]);
            else {
                const auto description = cf.invoke("CFCopyDescription", std::array<Address, 1>{s.x[0]});
                text = description ? cf.utf8(description) : std::string("(no description)");
                if (description) context.objects().release(description);
            }
        }
        text += '\n';
        static_cast<void>(context.output(2, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size())));
    });
    // void CFShowStr(CFStringRef): the string's length and characters on stderr.
    function(core_foundation_library, "_CFShowStr", [this](CpuState& s, GuestMemory&) {
        std::string text;
        if (!s.x[0]) text = "This is a NULL string!\n";
        else {
            const auto units = read(s.x[0]);
            const auto contents = cf.string_utf16(units);
            text = "Length " + std::to_string(units.size()) + "\nContents \"" + cf.utf8(contents) + "\"\n";
            context.objects().release(contents);
        }
        static_cast<void>(context.output(2, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size())));
    });
    // void CFLog(int32_t level, CFStringRef format, ...): the formatted message on stderr.
    function(core_foundation_library, "_CFLog", [this](CpuState& s, GuestMemory&) {
        require(s.x[1], "CFLog");
        const auto message = send(send(class_named("NSString"), "alloc"), "initWithFormat:arguments:", {s.x[1], s.sp});
        auto text = message ? cf.utf8(message) : std::string();
        if (message) context.objects().release(message);
        text += '\n';
        static_cast<void>(context.output(2, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size())));
    });
}

void Strings::install_editing() {
    // CFMutableStringRef CFStringCreateMutable(CFAllocatorRef, CFIndex maxLength)
    function(core_foundation_library, "_CFStringCreateMutable", [this](CpuState& s, GuestMemory&) {
        if (s.x[0]) fail("custom allocators are unsupported");
        const auto empty = cf.string("");
        s.x[0] = cf.mutable_copy(empty);
        context.objects().release(empty);
    });
    // void CFStringAppendCharacters(CFMutableStringRef, const UniChar*, CFIndex)
    function(core_foundation_library, "_CFStringAppendCharacters", [this](CpuState& s, GuestMemory& m) {
        require(s.x[0], "CFStringAppendCharacters");
        if (s.x[2] > (1ull << 30)) fail("CFStringAppendCharacters: length out of range");
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(s.x[2] * 2));
        if (!bytes.empty()) m.read_bytes(s.x[1], bytes);
        Units units(static_cast<std::size_t>(s.x[2]));
        for (std::size_t at = 0; at < units.size(); ++at) units[at] = static_cast<std::uint16_t>(bytes[at * 2] | (bytes[at * 2 + 1] << 8));
        append(s.x[0], units);
    });
    // void CFStringAppendCString(CFMutableStringRef, const char*, CFStringEncoding)
    function(core_foundation_library, "_CFStringAppendCString", [this](CpuState& s, GuestMemory&) {
        require(s.x[0], "CFStringAppendCString");
        require(s.x[1], "CFStringAppendCString (C string)");
        if (const auto units = decode(s.x[1], c_length(s.x[1]), static_cast<std::uint32_t>(s.x[2]))) append(s.x[0], *units);
    });
    // void CFStringAppendPascalString(CFMutableStringRef, ConstStr255Param, CFStringEncoding)
    function(core_foundation_library, "_CFStringAppendPascalString", [this](CpuState& s, GuestMemory& m) {
        require(s.x[0], "CFStringAppendPascalString");
        require(s.x[1], "CFStringAppendPascalString (Pascal string)");
        const auto length = m.read8(s.x[1]);
        if (const auto units = decode(s.x[1] + 1, length, static_cast<std::uint32_t>(s.x[2]))) append(s.x[0], *units);
    });
    // void CFStringAppendFormat(CFMutableStringRef, CFDictionaryRef formatOptions, CFStringRef format, ...)
    function(core_foundation_library, "_CFStringAppendFormat", [this](CpuState& s, GuestMemory&) {
        require(s.x[0], "CFStringAppendFormat");
        require(s.x[2], "CFStringAppendFormat (format)");
        const auto text = send(send(class_named("NSString"), "alloc"), "initWithFormat:arguments:", {s.x[2], s.sp});
        if (!text) fail("CFStringAppendFormat: the format did not produce a string");
        append(s.x[0], read(text));
        context.objects().release(text);
    });
    // void CFStringAppendFormatAndArguments(CFMutableStringRef, CFDictionaryRef, CFStringRef format, va_list)
    function(core_foundation_library, "_CFStringAppendFormatAndArguments", [this](CpuState& s, GuestMemory&) {
        require(s.x[0], "CFStringAppendFormatAndArguments");
        require(s.x[2], "CFStringAppendFormatAndArguments (format)");
        const auto text = send(send(class_named("NSString"), "alloc"), "initWithFormat:arguments:", {s.x[2], s.x[3]});
        if (!text) fail("CFStringAppendFormatAndArguments: the format did not produce a string");
        append(s.x[0], read(text));
        context.objects().release(text);
    });
    // void CFStringInsert(CFMutableStringRef, CFIndex, CFStringRef)
    function(core_foundation_library, "_CFStringInsert", [this](CpuState& s, GuestMemory&) {
        require(s.x[0], "CFStringInsert");
        require(s.x[2], "CFStringInsert (inserted string)");
        replace(s.x[0], static_cast<std::size_t>(s.x[1]), 0, read(s.x[2]));
    });
    // void CFStringDelete(CFMutableStringRef, CFRange)
    function(core_foundation_library, "_CFStringDelete", [this](CpuState& s, GuestMemory&) {
        require(s.x[0], "CFStringDelete");
        replace(s.x[0], static_cast<std::size_t>(s.x[1]), static_cast<std::size_t>(s.x[2]), {});
    });
    // void CFStringReplace(CFMutableStringRef, CFRange, CFStringRef)
    function(core_foundation_library, "_CFStringReplace", [this](CpuState& s, GuestMemory&) {
        require(s.x[0], "CFStringReplace");
        require(s.x[3], "CFStringReplace (replacement)");
        replace(s.x[0], static_cast<std::size_t>(s.x[1]), static_cast<std::size_t>(s.x[2]), read(s.x[3]));
    });
    // void CFStringReplaceAll(CFMutableStringRef, CFStringRef)
    function(core_foundation_library, "_CFStringReplaceAll", [this](CpuState& s, GuestMemory&) {
        require(s.x[0], "CFStringReplaceAll");
        require(s.x[1], "CFStringReplaceAll (replacement)");
        write(s.x[0], read(s.x[1]));
    });
    // CFIndex CFStringFindAndReplace(CFMutableStringRef, CFStringRef find, CFStringRef replace, CFRange, CFStringCompareFlags)
    function(core_foundation_library, "_CFStringFindAndReplace", [this](CpuState& s, GuestMemory&) {
        require(s.x[0], "CFStringFindAndReplace");
        require(s.x[1], "CFStringFindAndReplace (string to find)");
        require(s.x[2], "CFStringFindAndReplace (replacement)");
        const auto units = read(s.x[0]);
        const auto [location, length] = checked_range(units, s.x[3], s.x[4], "CFStringFindAndReplace");
        const auto found = find_all(units, read(s.x[1]), location, length, s.x[5], {});
        if (!found.empty()) {
            const auto with = read(s.x[2]);
            Units result;
            std::size_t at = 0;
            for (const auto& [start, size] : found) {
                result.insert(result.end(), units.begin() + static_cast<std::ptrdiff_t>(at), units.begin() + static_cast<std::ptrdiff_t>(start));
                result.insert(result.end(), with.begin(), with.end());
                at = start + size;
            }
            result.insert(result.end(), units.begin() + static_cast<std::ptrdiff_t>(at), units.end());
            write(s.x[0], std::move(result));
        }
        s.x[0] = found.size();
    });
    // void CFStringPad(CFMutableStringRef, CFStringRef padString, CFIndex length, CFIndex indexIntoPad)
    function(core_foundation_library, "_CFStringPad", [this](CpuState& s, GuestMemory&) {
        require(s.x[0], "CFStringPad");
        auto units = read(s.x[0]);
        const auto length = static_cast<std::size_t>(s.x[2]);
        if (static_cast<std::int64_t>(s.x[2]) < 0 || static_cast<std::int64_t>(s.x[3]) < 0) fail("CFStringPad: negative length or pad index");
        if (length < units.size()) units.resize(length);
        else if (length > units.size()) {
            require(s.x[1], "CFStringPad (pad string)");
            const auto pad = read(s.x[1]);
            auto index = static_cast<std::size_t>(s.x[3]);
            if (pad.empty() || index >= pad.size()) fail("CFStringPad: pad index outside the pad string");
            while (units.size() < length) {
                units.push_back(pad[index]);
                index = (index + 1) % pad.size();
            }
        } else return;
        write(s.x[0], std::move(units));
    });
    // void CFStringTrim(CFMutableStringRef, CFStringRef trimString): every leading and trailing copy.
    function(core_foundation_library, "_CFStringTrim", [this](CpuState& s, GuestMemory&) {
        require(s.x[0], "CFStringTrim");
        require(s.x[1], "CFStringTrim (trim string)");
        const auto units = read(s.x[0]);
        const auto trim = read(s.x[1]);
        if (trim.empty()) return;
        std::size_t start = 0, end = units.size();
        while (end - start >= trim.size() && std::equal(trim.begin(), trim.end(), units.begin() + static_cast<std::ptrdiff_t>(start))) start += trim.size();
        if (start < end && trim.size() < end - start)
            while (end - start >= trim.size() && std::equal(trim.begin(), trim.end(), units.begin() + static_cast<std::ptrdiff_t>(end - trim.size()))) end -= trim.size();
        if (start == 0 && end == units.size()) return;
        write(s.x[0], Units(units.begin() + static_cast<std::ptrdiff_t>(start), units.begin() + static_cast<std::ptrdiff_t>(std::max(start, end))));
    });
    // void CFStringTrimWhitespace(CFMutableStringRef)
    function(core_foundation_library, "_CFStringTrimWhitespace", [this](CpuState& s, GuestMemory&) {
        require(s.x[0], "CFStringTrimWhitespace");
        const auto units = read(s.x[0]);
        std::size_t start = 0, end = units.size();
        while (start < end && whitespace_or_newline(units[start])) ++start;
        while (end > start && whitespace_or_newline(units[end - 1])) --end;
        if (start == 0 && end == units.size()) return;
        write(s.x[0], Units(units.begin() + static_cast<std::ptrdiff_t>(start), units.begin() + static_cast<std::ptrdiff_t>(end)));
    });
    // void CFStringLowercase/Uppercase/Capitalize(CFMutableStringRef, CFLocaleRef)
    for (const auto& [symbol, which] : {std::pair{"_CFStringLowercase", 0}, std::pair{"_CFStringUppercase", 1}, std::pair{"_CFStringCapitalize", 2}}) {
        function(core_foundation_library, symbol, [this, which](CpuState& s, GuestMemory&) {
            require(s.x[0], "CFString case mapping");
            const auto units = read(s.x[0]);
            if (units.empty()) return;
            write(s.x[0], mapped(units, locale_identifier(s.x[1]), which));
        });
    }
    // void CFStringFold(CFMutableStringRef, CFStringCompareFlags, CFLocaleRef)
    function(core_foundation_library, "_CFStringFold", [this](CpuState& s, GuestMemory&) {
        require(s.x[0], "CFStringFold");
        const auto units = read(s.x[0]);
        if (units.empty() || !(s.x[1] & (compare_case_insensitive | compare_diacritic_insensitive | compare_width_insensitive))) return;
        write(s.x[0], folded(units, s.x[1], locale_identifier(s.x[2])));
    });
    // Boolean CFStringTransform(CFMutableStringRef, CFRange* range, CFStringRef transform, Boolean reverse)
    function(core_foundation_library, "_CFStringTransform", [this](CpuState& s, GuestMemory& m) {
        require(s.x[0], "CFStringTransform");
        require(s.x[2], "CFStringTransform (transform)");
        auto units = read(s.x[0]);
        std::size_t location = 0, length = units.size();
        if (s.x[1]) std::tie(location, length) = checked_range(units, m.read64(s.x[1]), m.read64(s.x[1] + 8), "CFStringTransform");
        const auto id_units = read(s.x[2]);
        const std::u16string id(id_units.begin(), id_units.end());
        const auto result = transliterate(id, (s.x[3] & 0xff) != 0,
            Units(units.begin() + static_cast<std::ptrdiff_t>(location), units.begin() + static_cast<std::ptrdiff_t>(location + length)));
        if (!result) { s.x[0] = 0; return; }
        units.erase(units.begin() + static_cast<std::ptrdiff_t>(location), units.begin() + static_cast<std::ptrdiff_t>(location + length));
        units.insert(units.begin() + static_cast<std::ptrdiff_t>(location), result->begin(), result->end());
        const auto new_length = result->size();
        write(s.x[0], std::move(units));
        if (s.x[1]) m.write64(s.x[1] + 8, new_length);
        s.x[0] = 1;
    });
}

void Strings::install_search() {
    // Boolean CFStringFindWithOptions(CFStringRef, CFStringRef find, CFRange, CFStringCompareFlags, CFRange* result)
    function(core_foundation_library, "_CFStringFindWithOptions", [this](CpuState& s, GuestMemory&) {
        require(s.x[0], "CFStringFindWithOptions");
        require(s.x[1], "CFStringFindWithOptions (string to find)");
        const auto units = read(s.x[0]);
        const auto [location, length] = checked_range(units, s.x[2], s.x[3], "CFStringFindWithOptions");
        const auto found = find(units, read(s.x[1]), location, length, s.x[4], {});
        if (found) write_range(s.x[5], static_cast<std::int64_t>(found->first), static_cast<std::int64_t>(found->second));
        s.x[0] = found.has_value();
    });
    // Boolean CFStringFindWithOptionsAndLocale(CFStringRef, CFStringRef, CFRange, CFStringCompareFlags, CFLocaleRef, CFRange*)
    function(core_foundation_library, "_CFStringFindWithOptionsAndLocale", [this](CpuState& s, GuestMemory&) {
        require(s.x[0], "CFStringFindWithOptionsAndLocale");
        require(s.x[1], "CFStringFindWithOptionsAndLocale (string to find)");
        const auto units = read(s.x[0]);
        const auto [location, length] = checked_range(units, s.x[2], s.x[3], "CFStringFindWithOptionsAndLocale");
        const auto found = find(units, read(s.x[1]), location, length, s.x[4], locale_identifier(s.x[5]));
        if (found) write_range(s.x[6], static_cast<std::int64_t>(found->first), static_cast<std::int64_t>(found->second));
        s.x[0] = found.has_value();
    });
    // CFRange CFStringFind(CFStringRef, CFStringRef find, CFStringCompareFlags): {kCFNotFound, 0} when absent.
    function(core_foundation_library, "_CFStringFind", [this](CpuState& s, GuestMemory&) {
        require(s.x[0], "CFStringFind");
        require(s.x[1], "CFStringFind (string to find)");
        const auto units = read(s.x[0]);
        const auto found = find(units, read(s.x[1]), 0, units.size(), s.x[2], {});
        s.x[0] = found ? found->first : static_cast<std::uint64_t>(not_found);
        s.x[1] = found ? found->second : 0;
    });
    // Boolean CFStringFindCharacterFromSet(CFStringRef, CFCharacterSetRef, CFRange, CFStringCompareFlags, CFRange* result)
    function(core_foundation_library, "_CFStringFindCharacterFromSet", [this](CpuState& s, GuestMemory&) {
        require(s.x[0], "CFStringFindCharacterFromSet");
        require(s.x[1], "CFStringFindCharacterFromSet (character set)");
        const auto units = read(s.x[0]);
        const auto [location, length] = checked_range(units, s.x[2], s.x[3], "CFStringFindCharacterFromSet");
        const bool backwards = s.x[4] & compare_backwards, anchored = s.x[4] & compare_anchored;
        const auto member = [&](std::size_t at, std::size_t& size) {
            std::uint32_t c = units[at];
            size = 1;
            if (c >= 0xd800 && c < 0xdc00 && at + 1 < location + length && units[at + 1] >= 0xdc00 && units[at + 1] < 0xe000) {
                c = 0x10000 + ((c - 0xd800) << 10) + (units[at + 1] - 0xdc00u);
                size = 2;
            }
            return (guest_call(core_foundation_library, "_CFCharacterSetIsLongCharacterMember", {s.x[1], c}) & 0xff) != 0;
        };
        std::optional<std::pair<std::size_t, std::size_t>> found;
        std::size_t size{};
        if (length) {
            if (backwards) {
                for (auto at = location + length; at-- > location;) {
                    // A low surrogate belongs to the pair that ends at it.
                    auto start = at;
                    if (at > location && units[at] >= 0xdc00 && units[at] < 0xe000 && units[at - 1] >= 0xd800 && units[at - 1] < 0xdc00) --start;
                    if (member(start, size)) { found = std::pair{start, size}; break; }
                    if (anchored) break;
                    at = start;
                }
            } else {
                for (auto at = location; at < location + length; at += size) {
                    if (member(at, size)) { found = std::pair{at, size}; break; }
                    if (anchored) break;
                }
            }
        }
        if (found) write_range(s.x[5], static_cast<std::int64_t>(found->first), static_cast<std::int64_t>(found->second));
        s.x[0] = found.has_value();
    });
    // CFComparisonResult CFStringCompareWithOptionsAndLocale(CFStringRef, CFStringRef, CFRange, CFStringCompareFlags, CFLocaleRef)
    function(core_foundation_library, "_CFStringCompareWithOptionsAndLocale", [this](CpuState& s, GuestMemory&) {
        require(s.x[0], "CFStringCompareWithOptionsAndLocale");
        require(s.x[1], "CFStringCompareWithOptionsAndLocale (second string)");
        if (!s.x[5]) {
            s.x[0] = cf.invoke("CFStringCompareWithOptions", std::array<Address, 5>{s.x[0], s.x[1], s.x[2], s.x[3], s.x[4] & ~compare_localized});
            return;
        }
        const auto units = read(s.x[0]);
        const auto [location, length] = checked_range(units, s.x[2], s.x[3], "CFStringCompareWithOptionsAndLocale");
        const auto other = read(s.x[1]);
        namespace locale_api = host::locale;
        std::uint32_t flags = 0;
        if (s.x[4] & compare_case_insensitive) flags |= locale_api::ignore_case;
        if (s.x[4] & compare_numerically) flags |= locale_api::digits_as_numbers;
        if (s.x[4] & compare_diacritic_insensitive) flags |= locale_api::ignore_nonspace;
        if (s.x[4] & compare_width_insensitive) flags |= locale_api::ignore_width;
        const auto part = view(units, location, length);
        const auto result = locale_api::compare(bcp47(locale_identifier(s.x[5])), part, view(other), flags);
        if (!result) fail("CFStringCompareWithOptionsAndLocale needs the host's collation");
        int order = *result;
        if (order == 0 && (s.x[4] & compare_forced_ordering)) order = part < view(other) ? -1 : part > view(other) ? 1 : 0;
        s.x[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(order));
    });
    // CFRange CFStringGetRangeOfComposedCharactersAtIndex(CFStringRef, CFIndex): the grapheme cluster.
    function(core_foundation_library, "_CFStringGetRangeOfComposedCharactersAtIndex", [this](CpuState& s, GuestMemory&) {
        require(s.x[0], "CFStringGetRangeOfComposedCharactersAtIndex");
        const auto units = read(s.x[0]);
        if (s.x[1] >= units.size()) fail("CFStringGetRangeOfComposedCharactersAtIndex: index outside the string");
        const auto& api = icu();
        host::icu::UErrorCode error = host::icu::zero_error;
        const auto breaks = api.ubrk_open(host::icu::break_character, "", reinterpret_cast<const host::icu::UChar*>(units.data()),
            static_cast<std::int32_t>(units.size()), &error);
        if (!breaks || host::icu::failure(error)) fail("CFStringGetRangeOfComposedCharactersAtIndex: no ICU character breaks");
        const auto index = static_cast<std::int32_t>(s.x[1]);
        const auto start = api.ubrk_isBoundary(breaks, index) ? index : api.ubrk_preceding(breaks, index);
        const auto end = api.ubrk_following(breaks, index);
        api.ubrk_close(breaks);
        const auto first = start == host::icu::break_done ? 0 : start;
        const auto last = end == host::icu::break_done ? static_cast<std::int32_t>(units.size()) : end;
        s.x[0] = static_cast<std::uint64_t>(first);
        s.x[1] = static_cast<std::uint64_t>(last - first);
    });
    // void CFStringGetLineBounds / CFStringGetParagraphBounds(CFStringRef, CFRange, CFIndex* begin, CFIndex* end, CFIndex* contentsEnd)
    function(core_foundation_library, "_CFStringGetLineBounds", [this](CpuState& s, GuestMemory&) { line_bounds(s, true); });
    function(core_foundation_library, "_CFStringGetParagraphBounds", [this](CpuState& s, GuestMemory&) { line_bounds(s, false); });
    // CFArrayRef CFStringCreateArrayBySeparatingStrings(CFAllocatorRef, CFStringRef, CFStringRef separator)
    function(core_foundation_library, "_CFStringCreateArrayBySeparatingStrings", [this](CpuState& s, GuestMemory&) {
        if (s.x[0]) fail("custom allocators are unsupported");
        require(s.x[1], "CFStringCreateArrayBySeparatingStrings");
        require(s.x[2], "CFStringCreateArrayBySeparatingStrings (separator)");
        const auto units = read(s.x[1]);
        const auto found = find_all(units, read(s.x[2]), 0, units.size(), 0, {});
        std::vector<Address> parts;
        if (found.empty()) parts.push_back(cf.string_utf16(units));
        else {
            std::size_t start = 0;
            for (const auto& [at, size] : found) {
                parts.push_back(cf.string_utf16(std::span(units).subspan(start, at - start)));
                start = at + size;
            }
            parts.push_back(cf.string_utf16(std::span(units).subspan(start)));
        }
        s.x[0] = cf.array(parts);
        for (const auto part : parts) context.objects().release(part);
    });
    // CFStringRef CFStringCreateByCombiningStrings(CFAllocatorRef, CFArrayRef, CFStringRef separator)
    function(core_foundation_library, "_CFStringCreateByCombiningStrings", [this](CpuState& s, GuestMemory&) {
        if (s.x[0]) fail("custom allocators are unsupported");
        require(s.x[1], "CFStringCreateByCombiningStrings");
        const auto values = cf.array_values(s.x[1]);
        const auto separator = values.size() > 1 ? (require(s.x[2], "CFStringCreateByCombiningStrings (separator)"), read(s.x[2])) : Units{};
        Units result;
        for (std::size_t at = 0; at < values.size(); ++at) {
            if (at) result.insert(result.end(), separator.begin(), separator.end());
            const auto part = read(values[at]);
            result.insert(result.end(), part.begin(), part.end());
        }
        s.x[0] = cf.string_utf16(result);
    });
    // SInt32 CFStringGetIntValue(CFStringRef) (__CFStringScanInteger): 0 without digits, clamped on overflow.
    function(core_foundation_library, "_CFStringGetIntValue", [this](CpuState& s, GuestMemory&) {
        require(s.x[0], "CFStringGetIntValue");
        const auto units = read(s.x[0]);
        std::size_t at = 0;
        const auto skip = [&] { while (at < units.size() && scanner_space(units[at])) ++at; };
        skip();
        bool negative = false;
        if (at < units.size() && (units[at] == '-' || units[at] == '+')) { negative = units[at] == '-'; ++at; skip(); }
        std::int64_t value = 0;
        bool digits = false, clamped = false;
        while (at < units.size() && units[at] >= '0' && units[at] <= '9') {
            digits = true;
            if (!clamped) {
                value = value * 10 + (units[at] - '0');
                if (value > (negative ? 2147483648ll : 2147483647ll)) clamped = true;
            }
            ++at;
        }
        std::int32_t result = 0;
        if (digits) {
            if (clamped) result = negative ? INT32_MIN : INT32_MAX;
            else result = static_cast<std::int32_t>(negative ? -value : value);
        }
        s.x[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(result));
    });
    // double CFStringGetDoubleValue(CFStringRef) (__CFStringScanDouble): strtod of the leading [+-.0-9Ee] run, else 0.
    function(core_foundation_library, "_CFStringGetDoubleValue", [this](CpuState& s, GuestMemory&) {
        require(s.x[0], "CFStringGetDoubleValue");
        const auto units = read(s.x[0]);
        std::size_t at = 0;
        while (at < units.size() && scanner_space(units[at])) ++at;
        std::string text;
        for (; at < units.size(); ++at) {
            const auto c = units[at];
            if (!((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.' || c == 'e' || c == 'E')) break;
            text.push_back(static_cast<char>(c));
        }
        double value = 0;
        std::string_view digits(text);
        bool negative = false;
        if (!digits.empty() && (digits[0] == '+' || digits[0] == '-')) { negative = digits[0] == '-'; digits.remove_prefix(1); }
        if (!digits.empty() && digits[0] != '+' && digits[0] != '-') {
            double parsed{};
            const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), parsed, std::chars_format::general);
            if (error == std::errc::result_out_of_range) {
                // strtod's answers: an overflow is HUGE_VAL, an underflow 0.
                const std::string_view used(digits.data(), static_cast<std::size_t>(end - digits.data()));
                const auto exponent = used.find_first_of("eE");
                parsed = exponent != std::string_view::npos && exponent + 1 < used.size() && used[exponent + 1] == '-' ? 0.0 : HUGE_VAL;
            }
            if ((error == std::errc() || error == std::errc::result_out_of_range) && end != digits.data()) value = negative ? -parsed : parsed;
        }
        return_real(s, value);
    });
    // CFStringRef CFStringCreateWithPascalString(CFAllocatorRef, ConstStr255Param, CFStringEncoding)
    function(core_foundation_library, "_CFStringCreateWithPascalString", [this](CpuState& s, GuestMemory& m) {
        if (s.x[0]) fail("custom allocators are unsupported");
        require(s.x[1], "CFStringCreateWithPascalString");
        s.x[0] = cf.invoke("CFStringCreateWithBytes", std::array<Address, 5>{0, s.x[1] + 1, m.read8(s.x[1]), s.x[2], 0});
    });
    // Boolean CFStringGetPascalString(CFStringRef, StringPtr buffer, CFIndex bufferSize, CFStringEncoding)
    function(core_foundation_library, "_CFStringGetPascalString", [this](CpuState& s, GuestMemory& m) {
        require(s.x[0], "CFStringGetPascalString");
        const auto length = cf.length(s.x[0]);
        if (!s.x[1] || static_cast<std::int64_t>(s.x[2]) < 1) { s.x[0] = 0; return; }
        const auto capacity = std::min<Address>(s.x[2] - 1, 255);
        const auto used = context.allocate_guest(8);
        m.write64(used, 0);
        const auto converted = cf.invoke("CFStringGetBytes", std::array<Address, 9>{s.x[0], 0, length, s.x[3], 0, 0, s.x[1] + 1, capacity, used});
        const auto bytes = m.read64(used);
        context.release_guest(used);
        if (converted != length) { s.x[0] = 0; return; }
        m.write8(s.x[1], static_cast<std::uint8_t>(bytes));
        s.x[0] = 1;
    });
    // ConstStringPtr CFStringGetPascalStringPtr(CFStringRef, CFStringEncoding): strings here keep no
    // Pascal storage, which the documentation allows (NULL: use CFStringGetPascalString).
    function(core_foundation_library, "_CFStringGetPascalStringPtr", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    // CFStringRef CFStringCreateFromExternalRepresentation(CFAllocatorRef, CFDataRef, CFStringEncoding)
    function(core_foundation_library, "_CFStringCreateFromExternalRepresentation", [this](CpuState& s, GuestMemory&) {
        if (s.x[0]) fail("custom allocators are unsupported");
        require(s.x[1], "CFStringCreateFromExternalRepresentation");
        const auto bytes = cf.invoke("CFDataGetBytePtr", std::array<Address, 1>{s.x[1]});
        const auto length = cf.invoke("CFDataGetLength", std::array<Address, 1>{s.x[1]});
        s.x[0] = cf.invoke("CFStringCreateWithBytes", std::array<Address, 5>{0, bytes, length, s.x[2], 1});
    });
}

void Strings::install_encodings() {
    function(core_foundation_library, "_CFStringConvertEncodingToNSStringEncoding", [](CpuState& s, GuestMemory&) {
        s.x[0] = cf_encodings::to_ns(static_cast<std::uint32_t>(s.x[0]));
    });
    function(core_foundation_library, "_CFStringConvertNSStringEncodingToEncoding", [](CpuState& s, GuestMemory&) {
        s.x[0] = cf_encodings::from_ns(s.x[0]);
    });
    // UInt32 CFStringConvertEncodingToWindowsCodepage(CFStringEncoding): kCFStringEncodingInvalidId when none.
    function(core_foundation_library, "_CFStringConvertEncodingToWindowsCodepage", [](CpuState& s, GuestMemory&) {
        const auto codepage = cf_encodings::windows_codepage(static_cast<std::uint32_t>(s.x[0]));
        s.x[0] = codepage ? codepage : cf_encodings::invalid_id;
    });
    function(core_foundation_library, "_CFStringConvertWindowsCodepageToEncoding", [](CpuState& s, GuestMemory&) {
        s.x[0] = cf_encodings::from_windows_codepage(static_cast<std::uint32_t>(s.x[0]));
    });
    // CFStringRef CFStringConvertEncodingToIANACharSetName(CFStringEncoding): a cached string (Get rule).
    function(core_foundation_library, "_CFStringConvertEncodingToIANACharSetName", [this](CpuState& s, GuestMemory&) {
        s.x[0] = iana_name(static_cast<std::uint32_t>(s.x[0]));
    });
    function(core_foundation_library, "_CFStringConvertIANACharSetNameToEncoding", [this](CpuState& s, GuestMemory&) {
        require(s.x[0], "CFStringConvertIANACharSetNameToEncoding");
        const auto name = cf.string_utf16(read(s.x[0]));
        const auto text = cf.utf8(name);
        context.objects().release(name);
        s.x[0] = cf_encodings::from_iana_name(text);
    });
    function(core_foundation_library, "_CFStringGetMostCompatibleMacStringEncoding", [](CpuState& s, GuestMemory&) {
        s.x[0] = cf_encodings::most_compatible_mac(static_cast<std::uint32_t>(s.x[0]));
    });
    // const CFStringEncoding* CFStringGetListOfAvailableEncodings(void): kCFStringEncodingInvalidId-terminated.
    function(core_foundation_library, "_CFStringGetListOfAvailableEncodings", [this](CpuState& s, GuestMemory&) {
        s.x[0] = available_encodings();
    });
    // CFStringGetFastestEncoding / CFStringGetSmallestEncoding: ASCII for 7-bit strings, as CF's
    // eight-bit strings; the smallest encoding is then Mac OS Roman when it holds every character.
    function(core_foundation_library, "_CFStringGetFastestEncoding", [this](CpuState& s, GuestMemory&) {
        require(s.x[0], "CFStringGetFastestEncoding");
        const auto units = read(s.x[0]);
        s.x[0] = std::all_of(units.begin(), units.end(), [](std::uint16_t c) { return c < 0x80; }) ? cf_encodings::ascii : cf_encodings::unicode;
    });
    function(core_foundation_library, "_CFStringGetSmallestEncoding", [this](CpuState& s, GuestMemory&) {
        require(s.x[0], "CFStringGetSmallestEncoding");
        const auto units = read(s.x[0]);
        if (std::all_of(units.begin(), units.end(), [](std::uint16_t c) { return c < 0x80; })) { s.x[0] = cf_encodings::ascii; return; }
        if (cf_encodings::converts(cf_encodings::mac_roman) &&
            cf_encodings::encode(units, cf_encodings::mac_roman, 0, UINT64_MAX).units == units.size()) { s.x[0] = cf_encodings::mac_roman; return; }
        s.x[0] = cf_encodings::unicode;
    });
}

} // namespace

void install_corefoundation_strings(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf) {
    std::make_shared<Strings>(context, objc, cf)->install();
}

} // namespace alchemy::darwin_headless
