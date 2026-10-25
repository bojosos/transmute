// The encoding database and ICU converters of CoreFoundation (cf_encodings.hpp).
//
// The tables and the name rules are ported from swift-corelibs-foundation
// swift-6.4.0-RELEASE, Sources/CoreFoundation/CFStringEncodingDatabase.c,
// CFStringUtilities.c and CFICUConverters.c: Copyright (c) 2004-2019 Apple Inc.
// and the Swift project authors, licensed under the Apache License v2.0 with
// Runtime Library Exception (third_party/swift-corelibs-foundation/LICENSE).
// Local changes: one table of rows instead of parallel arrays, C++ in place of
// the CF dictionaries, and converters from host::icu.
#include "alchemy/darwin/corefoundation/cf_encodings.hpp"

#include "alchemy/host/icu.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <map>
#include <mutex>
#include <stdexcept>

namespace alchemy::cf_encodings {
namespace {

// One row of CF's known-encoding list: the encoding, its Windows code page
// (0: none), its canonical (IANA) name without the "x-mac-" prefix, its
// display name, and the Mac script CFStringGetMostCompatibleMacStringEncoding
// answers for the rows from DOSLatinUS on.
struct Row {
    std::uint16_t encoding;
    std::uint16_t codepage;
    const char* canonical;
    const char* display;
    std::uint16_t mac;
};

// Mac scripts (CFStringEncodingExt.h) used by the compatibility column.
enum : std::uint16_t {
    m_roman = 0, m_japanese = 1, m_chinese_trad = 2, m_korean = 3, m_arabic = 4, m_hebrew = 5, m_greek = 6,
    m_cyrillic = 7, m_thai = 21, m_chinese_simp = 25, m_central_eur = 29, m_vietnamese = 30, m_turkish = 35,
    m_icelandic = 37, m_romanian = 38, m_celtic = 39, m_ukrainian = 0x98, m_inuit = 0xec,
};

// Sorted by encoding, as CF's binary search needs.
constexpr Row rows[] = {
    {0x0000, 10000, "macintosh", "Western (Mac OS Roman)", 0},
    {0x0001, 10001, "japanese", "Japanese (Mac OS)", 0},
    {0x0002, 10002, "trad-chinese", "Traditional Chinese (Mac OS)", 0},
    {0x0003, 10003, "korean", "Korean (Mac OS)", 0},
    {0x0004, 10004, "arabic", "Arabic (Mac OS)", 0},
    {0x0005, 10005, "hebrew", "Hebrew (Mac OS)", 0},
    {0x0006, 10006, "greek", "Greek (Mac OS)", 0},
    {0x0007, 10007, "cyrillic", "Cyrillic (Mac OS)", 0},
    {0x0009, 0, "devanagari", "Devanagari (Mac OS)", 0},
    {0x000a, 0, "gurmukhi", "Gurmukhi (Mac OS)", 0},
    {0x000b, 0, "gujarati", "Gujarati (Mac OS)", 0},
    {0x000c, 0, "oriya", "Oriya (Mac OS)", 0},
    {0x000d, 0, "bengali", "Bengali (Mac OS)", 0},
    {0x000e, 0, "tamil", "Tamil (Mac OS)", 0},
    {0x000f, 0, "telugu", "Telugu (Mac OS)", 0},
    {0x0010, 0, "kannada", "Kannada (Mac OS)", 0},
    {0x0011, 0, "malayalam", "Malayalam (Mac OS)", 0},
    {0x0012, 0, "sinhalese", "Sinhalese (Mac OS)", 0},
    {0x0013, 0, "burmese", "Burmese (Mac OS)", 0},
    {0x0014, 0, "khmer", "Khmer (Mac OS)", 0},
    {0x0015, 10021, "thai", "Thai (Mac OS)", 0},
    {0x0016, 0, "laotian", "Laotian (Mac OS)", 0},
    {0x0017, 0, "georgian", "Georgian (Mac OS)", 0},
    {0x0018, 0, "armenian", "Armenian (Mac OS)", 0},
    {0x0019, 10008, "simp-chinese", "Simplified Chinese (Mac OS)", 0},
    {0x001a, 0, "tibetan", "Tibetan (Mac OS)", 0},
    {0x001b, 0, "mongolian", "Mongolian (Mac OS)", 0},
    {0x001c, 0, "ethiopic", "Ethiopic (Mac OS)", 0},
    {0x001d, 10029, "centraleurroman", "Central European (Mac OS)", 0},
    {0x001e, 0, "vietnamese", "Vietnamese (Mac OS)", 0},
    {0x0021, 0, "symbol", "Symbol (Mac OS)", 0},
    {0x0022, 0, "dingbats", "Dingbats (Mac OS)", 0},
    {0x0023, 10081, "turkish", "Turkish (Mac OS)", 0},
    {0x0024, 10082, "croatian", "Croatian (Mac OS)", 0},
    {0x0025, 10079, "icelandic", "Icelandic (Mac OS)", 0},
    {0x0026, 10010, "romanian", "Romanian (Mac OS)", 0},
    {0x0027, 0, "celtic", "Celtic (Mac OS)", 0},
    {0x0028, 0, "gaelic", "Gaelic (Mac OS)", 0},
    {0x008c, 0, "farsi", "Farsi (Mac OS)", 0},
    {0x0098, 10017, "ukrainian", "Cyrillic (Mac OS Ukrainian)", 0},
    {0x00ec, 0, "inuit", "Inuit (Mac OS)", 0},
    {0x0400, 437, nullptr, "Latin-US (DOS)", m_roman},
    {0x0405, 737, nullptr, "Greek (DOS)", m_greek},
    {0x0406, 775, nullptr, "Baltic (DOS)", m_central_eur},
    {0x0410, 850, nullptr, "Western (DOS Latin 1)", m_roman},
    {0x0411, 851, nullptr, "Greek (DOS Greek 1)", m_greek},
    {0x0412, 852, nullptr, "Central European (DOS Latin 2)", m_central_eur},
    {0x0413, 855, nullptr, "Cyrillic (DOS)", m_cyrillic},
    {0x0414, 857, nullptr, "Turkish (DOS)", m_turkish},
    {0x0415, 860, nullptr, "Portuguese (DOS)", m_roman},
    {0x0416, 861, nullptr, "Icelandic (DOS)", m_icelandic},
    {0x0417, 862, nullptr, "Hebrew (DOS)", m_hebrew},
    {0x0418, 863, nullptr, "Canadian French (DOS)", m_roman},
    {0x0419, 864, nullptr, "Arabic (DOS)", m_arabic},
    {0x041a, 865, nullptr, "Nordic (DOS)", m_inuit},
    {0x041b, 866, nullptr, "Russian (DOS)", m_cyrillic},
    {0x041c, 869, nullptr, "Greek (DOS Greek 2)", m_greek},
    {0x041d, 874, nullptr, "Thai (Windows, DOS)", m_thai},
    {0x0420, 932, nullptr, "Japanese (Windows, DOS)", m_japanese},
    {0x0421, 936, nullptr, "Simplified Chinese (Windows, DOS)", m_chinese_simp},
    {0x0422, 949, nullptr, "Korean (Windows, DOS)", m_korean},
    {0x0423, 950, nullptr, "Traditional Chinese (Windows, DOS)", m_chinese_trad},
    {0x0500, 1252, nullptr, "Western (Windows Latin 1)", m_roman},
    {0x0501, 1250, nullptr, "Central European (Windows Latin 2)", m_central_eur},
    {0x0502, 1251, nullptr, "Cyrillic (Windows)", m_cyrillic},
    {0x0503, 1253, nullptr, "Greek (Windows)", m_greek},
    {0x0504, 1254, nullptr, "Turkish (Windows Latin 5)", m_turkish},
    {0x0505, 1255, nullptr, "Hebrew (Windows)", m_hebrew},
    {0x0506, 1256, nullptr, "Arabic (Windows)", m_arabic},
    {0x0507, 1257, nullptr, "Baltic (Windows)", m_central_eur},
    {0x0508, 1258, nullptr, "Vietnamese (Windows)", m_vietnamese},
    {0x0510, 1361, nullptr, "Korean (Windows Johab)", m_korean},
    {0x0600, 20127, "us-ascii", "Western (ASCII)", m_roman},
    {0x0628, 0, nullptr, "Japanese (Shift JIS X0213)", m_japanese},
    {0x0632, 54936, "gb18030", "Chinese (GB 18030)", m_chinese_simp},
    {0x0820, 50221, "iso-2022-jp", "Japanese (ISO 2022-JP)", m_japanese},
    {0x0821, 0, "iso-2022-jp-2", "Japanese (ISO 2022-JP-2)", m_japanese},
    {0x0822, 0, "iso-2022-jp-1", "Japanese (ISO 2022-JP-1)", m_japanese},
    {0x0823, 0, "iso-2022-jp-3", "Japanese (ISO 2022-JP-3)", m_japanese},
    {0x0830, 50227, "iso-2022-cn", "Chinese (ISO 2022-CN)", m_chinese_simp},
    {0x0831, 0, "iso-2022-cn-ext", "Chinese (ISO 2022-CN-EXT)", m_chinese_simp},
    {0x0840, 50225, "iso-2022-kr", "Korean (ISO 2022-KR)", m_korean},
    {0x0920, 51932, "euc-jp", "Japanese (EUC)", m_japanese},
    {0x0930, 51936, "gb2312", "Simplified Chinese (GB 2312)", m_chinese_simp},
    {0x0931, 51950, "euc-tw", "Traditional Chinese (EUC)", m_chinese_trad},
    {0x0940, 51949, "euc-kr", "Korean (EUC)", m_korean},
    {0x0a01, 0, "shift_jis", "Japanese (Shift JIS)", m_japanese},
    {0x0a02, 20866, "koi8-r", "Cyrillic (KOI8-R)", m_cyrillic},
    {0x0a03, 0, "big5", "Traditional Chinese (Big 5)", m_chinese_trad},
    {0x0a04, 0, "roman-latin1", "Western (Mac Mail)", m_roman},
    {0x0a05, 52936, "hz-gb-2312", "Simplified Chinese (HZ GB 2312)", m_chinese_simp},
    {0x0a06, 0, "big5-hkscs", "Traditional Chinese (Big 5 HKSCS)", m_chinese_trad},
    {0x0a07, 0, "viscii", nullptr, m_vietnamese},
    {0x0a08, 21866, "koi8-u", "Ukrainian (KOI8-U)", m_ukrainian},
    {0x0a09, 0, nullptr, "Traditional Chinese (Big 5-E)", m_chinese_trad},
    {0x0a10, 0, "utf7-imap", nullptr, m_roman},
    {0x0b01, 0, "x-nextstep", "Western (NextStep)", m_roman},
    {0x0c02, 37, "ibm037", "Western (EBCDIC Latin 1)", m_roman},
};

// ISO 8859-1 ... -16 (index part - 1): display names and the most compatible Mac script.
constexpr const char* iso_names[] = {
    "Western (ISO Latin 1)", "Central European (ISO Latin 2)", "Western (ISO Latin 3)", "Central European (ISO Latin 4)",
    "Cyrillic (ISO 8859-5)", "Arabic (ISO 8859-6)", "Greek (ISO 8859-7)", "Hebrew (ISO 8859-8)",
    "Turkish (ISO Latin 5)", "Nordic (ISO Latin 6)", "Thai (ISO 8859-11)", nullptr,
    "Baltic (ISO Latin 7)", "Celtic (ISO Latin 8)", "Western (ISO Latin 9)", "Romanian (ISO Latin 10)",
};
constexpr std::uint16_t iso_mac[] = {
    m_roman, m_central_eur, m_roman, m_central_eur, m_cyrillic, m_arabic, m_greek, m_hebrew,
    m_turkish, m_inuit, m_thai, m_roman, m_central_eur, m_celtic, m_roman, m_romanian,
};
constexpr std::uint32_t iso_codepage_base = 28590;

const Row* find_row(std::uint32_t encoding) {
    encoding &= 0x0fff;
    const auto found = std::lower_bound(std::begin(rows), std::end(rows), encoding,
        [](const Row& row, std::uint32_t value) { return row.encoding < value; });
    return found != std::end(rows) && found->encoding == encoding ? found : nullptr;
}

bool equal_ignoring_case(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}
bool starts_ignoring_case(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && equal_ignoring_case(text.substr(0, prefix.size()), prefix);
}
// strtol of the digits at the start of `text` (0 when there are none).
std::uint32_t leading_number(std::string_view text) {
    std::uint32_t value = 0;
    std::from_chars(text.data(), text.data() + text.size(), value);
    return value;
}

// CF's canonical-name lookup (__CFStringEncodingGetFromCanonicalName).
std::uint32_t from_canonical_name(std::string_view name) {
    if (starts_ignoring_case(name, "iso-8859-")) {
        const auto part = leading_number(name.substr(9));
        return part == 0 || part > 16 ? invalid_id : part + 0x0200;
    }
    if (starts_ignoring_case(name, "cp")) return from_windows_codepage(leading_number(name.substr(2)));
    if (starts_ignoring_case(name, "windows-")) return from_windows_codepage(leading_number(name.substr(8)));
    static constexpr std::pair<std::string_view, std::uint32_t> utf_names[] = {
        {"utf-7", utf7}, {"utf-8", utf8}, {"utf-16", utf16}, {"utf-16be", utf16be}, {"utf-16le", utf16le},
        {"utf-32", utf32}, {"utf-32be", utf32be}, {"utf-32le", utf32le}};
    for (const auto& [utf, encoding] : utf_names)
        if (equal_ignoring_case(name, utf)) return encoding;
    if (starts_ignoring_case(name, "macintosh")) return mac_roman;
    if (starts_ignoring_case(name, "x-mac-")) name.remove_prefix(6);
    for (const auto& row : rows)
        if (row.canonical && equal_ignoring_case(name, row.canonical)) return row.encoding == 0 ? invalid_id : row.encoding;
    return invalid_id;
}

const host::icu::Api& icu_api() {
    const auto* api = host::icu::api();
    if (!api) throw std::runtime_error("Alchemy CoreFoundation: converting this string encoding needs the host's ICU");
    return *api;
}

// The ICU converter name of an encoding (__CFStringEncodingGetICUName), cached.
std::optional<std::string> icu_name(std::uint32_t encoding) {
    static std::mutex mutex;
    static std::map<std::uint32_t, std::optional<std::string>> cache;
    std::lock_guard lock(mutex);
    if (const auto found = cache.find(encoding); found != cache.end()) return found->second;
    std::optional<std::string> result;
    if (const auto* api = host::icu::api()) {
        if (encoding == utf7_imap) result = "IMAP-mailbox-name";
        const auto alias = [api](const std::string& name) -> std::optional<std::string> {
            host::icu::UErrorCode error = host::icu::zero_error;
            const char* found = api->ucnv_getAlias(name.c_str(), 0, &error);
            if (host::icu::failure(error) || !found) return std::nullopt;
            return std::string(found);
        };
        if (!result && (encoding & 0x0f00) != unicode)
            if (const auto codepage = windows_codepage(encoding)) result = alias("windows-" + std::to_string(codepage));
        if (!result)
            if (const auto name = iana_name(encoding)) result = alias(*name);
    }
    cache.emplace(encoding, result);
    return result;
}

// The encodings core_foundation.cpp converts itself.
bool built_in(std::uint32_t encoding) {
    return encoding == utf8 || encoding == utf16 || encoding == utf16be || encoding == utf16le || encoding == utf32 ||
        encoding == utf32be || encoding == utf32le || encoding == ascii || encoding == iso_latin1;
}

struct Converter {
    const host::icu::Api& api;
    host::icu::UConverter* handle{};
    Converter(const host::icu::Api& table, const std::string& name) : api(table) {
        host::icu::UErrorCode error = host::icu::zero_error;
        handle = api.ucnv_open(name.c_str(), &error);
        if (host::icu::failure(error)) handle = nullptr;
    }
    ~Converter() { if (handle) api.ucnv_close(handle); }
    Converter(const Converter&) = delete;
    Converter& operator=(const Converter&) = delete;
};

} // namespace

std::uint32_t windows_codepage(std::uint32_t encoding) {
    const auto base = encoding & 0x0f00;
    if (base == unicode) {
        switch (encoding) {
        case utf7: return 65000;
        case utf8: return 65001;
        case utf16: return 1200;
        case utf16be: return 1201;
        case utf32: return 65005;
        case utf32be: return 65006;
        default: return 0;
        }
    }
    if (base == 0x0200) return iso_codepage_base + (encoding & 0xff);
    const auto* row = find_row(encoding);
    return row ? row->codepage : 0;
}

std::uint32_t from_windows_codepage(std::uint32_t codepage) {
    switch (codepage) {
    case 65001: return utf8;
    case 1200: return utf16;
    case 0: return invalid_id;
    case 1201: return utf16be;
    case 65005: return utf32;
    case 65006: return utf32be;
    case 65000: return utf7;
    default: break;
    }
    if (codepage > iso_codepage_base && codepage <= iso_codepage_base + 16) return codepage - iso_codepage_base + 0x0200;
    // CF fills a dictionary in list order, so a later row wins.
    std::uint32_t result = invalid_id;
    for (const auto& row : rows)
        if (row.codepage == codepage) result = row.encoding;
    return result;
}

std::optional<std::string> iana_name(std::uint32_t encoding) {
    switch (encoding & 0x0f00) {
    case unicode:
        switch (encoding) {
        case utf7: return "utf-7";
        case utf8: return "utf-8";
        case utf16: return "utf-16";
        case utf16be: return "utf-16be";
        case utf16le: return "utf-16le";
        case utf32: return "utf-32";
        case utf32be: return "utf-32be";
        case utf32le: return "utf-32le";
        default: return std::nullopt;
        }
    case 0x0200: return "iso-8859-" + std::to_string(encoding & 0xff);
    case 0x0400:
    case 0x0500:
        if (const auto* row = find_row(encoding); row && row->codepage)
            return ((encoding & 0x0f00) == 0x0400 ? "cp" : "windows-") + std::to_string(row->codepage);
        return std::nullopt;
    default:
        if (const auto* row = find_row(encoding); row && row->canonical) {
            const bool mac = ((encoding & 0x0f00) == 0 && encoding != mac_roman) || encoding == 0x0a04;
            return (mac ? "x-mac-" : "") + std::string(row->canonical);
        }
        return std::nullopt;
    }
}

std::uint32_t from_iana_name(std::string_view name) {
    auto encoding = from_canonical_name(name);
    if (encoding == invalid_id) {
        // __CFStringEncodingGetFromICUName: ICU's Windows, Java and MIME names of the alias.
        if (const auto* api = host::icu::api()) {
            const std::string text(name);
            host::icu::UErrorCode error = host::icu::zero_error;
            if (starts_ignoring_case(text, "windows-")) {
                const auto codepage = leading_number(std::string_view(text).substr(8));
                if (codepage && std::to_string(codepage) == text.substr(8)) return from_windows_codepage(codepage);
            }
            if (api->ucnv_countAliases(text.c_str(), &error) != 0 && !host::icu::failure(error)) {
                for (const char* standard : {"WINDOWS", "JAVA", "MIME"}) {
                    error = host::icu::zero_error;
                    const char* standard_name = api->ucnv_getStandardName(text.c_str(), standard, &error);
                    if (host::icu::failure(error) || !standard_name) continue;
                    const std::string_view found(standard_name);
                    if (std::string_view(standard) == "WINDOWS" && starts_ignoring_case(found, "windows-")) {
                        const auto codepage = leading_number(found.substr(8));
                        if (codepage && std::to_string(codepage) == found.substr(8)) return from_windows_codepage(codepage);
                    }
                    if (!starts_ignoring_case(text, found) && (encoding = from_canonical_name(found)) != invalid_id) return encoding;
                }
            }
        }
    }
    // Java's name for code page 950.
    if (encoding == invalid_id && starts_ignoring_case(name, "ms950")) encoding = from_canonical_name("cp950");
    return encoding;
}

std::optional<std::string_view> display_name(std::uint32_t encoding) {
    switch (encoding) {
    case utf8: return "Unicode (UTF-8)";
    case utf16: return "Unicode (UTF-16)";
    case utf16be: return "Unicode (UTF-16BE)";
    case utf16le: return "Unicode (UTF-16LE)";
    case utf32: return "Unicode (UTF-32)";
    case utf32be: return "Unicode (UTF-32BE)";
    case utf32le: return "Unicode (UTF-32LE)";
    case nonlossy_ascii: return "Non-lossy ASCII";
    case utf7: return "Unicode (UTF-7)";
    default: break;
    }
    if ((encoding & 0x0f00) == 0x0200) {
        const auto part = encoding & 0xff;
        if (part > 0 && part <= std::size(iso_names) && iso_names[part - 1]) return iso_names[part - 1];
        return std::nullopt;
    }
    if (const auto* row = find_row(encoding); row && row->display) return row->display;
    return std::nullopt;
}

// NSStringEncoding (NSString.h): NSASCIIStringEncoding 1 ... NSWindowsCP1250StringEncoding 15,
// NSISO2022JPStringEncoding 21, NSMacOSRomanStringEncoding 30; others are the CF value | 1 << 31.
constexpr std::uint64_t ns_mask = 1ull << 31;
std::uint64_t to_ns(std::uint32_t encoding) {
    if (encoding == utf8) return 4;
    if (encoding == ascii) return 1;
    switch (encoding & 0x0fff) {
    case unicode:
        if (encoding == utf16) return 10;
        break;
    case windows_latin1: return 12;
    case mac_roman: return 30;
    case dos_japanese: return 8;
    case windows_cyrillic: return 11;
    case windows_greek: return 13;
    case windows_latin5: return 14;
    case windows_latin2: return 15;
    case iso_latin1: return 5;
    case nonlossy_ascii: return 7;
    case euc_jp: return 3;
    case mac_symbol: return 6;
    case iso_latin2: return 9;
    case iso_2022_jp: return 21;
    case nextstep_latin: return 2;
    default: break;
    }
    return ns_mask | encoding;
}

std::uint32_t from_ns(std::uint64_t encoding) {
    static constexpr std::uint32_t small[] = {
        ascii, nextstep_latin, euc_jp, 0, iso_latin1, mac_symbol, nonlossy_ascii, dos_japanese, iso_latin2, utf16,
        windows_cyrillic, windows_latin1, windows_greek, windows_latin5, windows_latin2};
    if (encoding == 4) return utf8;
    if (encoding > 0 && encoding <= 15) return small[encoding - 1];
    if (encoding == 30) return mac_roman;
    if (encoding == 21) return iso_2022_jp;
    return (encoding & ns_mask) ? static_cast<std::uint32_t>(encoding & ~ns_mask) : invalid_id;
}

std::uint32_t most_compatible_mac(std::uint32_t encoding) {
    switch (encoding & 0x0f00) {
    case 0: return encoding & 0xff;
    case unicode: return unicode;
    case 0x0200: {
        const auto part = encoding & 0xff;
        return part > 0 && part <= std::size(iso_mac) ? iso_mac[part - 1] : invalid_id;
    }
    default:
        if (const auto* row = find_row(encoding); row && row->encoding >= 0x0400) return row->mac;
        return invalid_id;
    }
}

bool converts(std::uint32_t encoding) {
    if (built_in(encoding) || !host::icu::api()) return false;
    const auto name = icu_name(encoding);
    if (!name) return false;
    Converter converter(*host::icu::api(), *name);
    return converter.handle != nullptr;
}

std::vector<std::uint32_t> available() {
    std::vector<std::uint32_t> result{mac_roman, windows_latin1, iso_latin1, ascii, utf8, utf16, utf16be, utf16le,
                                      utf32, utf32be, utf32le};
    const auto add = [&](std::uint32_t encoding) {
        if (std::find(result.begin(), result.end(), encoding) == result.end() && converts(encoding)) result.push_back(encoding);
    };
    for (const auto& row : rows) add(row.encoding);
    for (std::uint32_t part = 2; part <= 16; ++part) add(0x0200 + part);
    add(utf7);
    // Mac OS Roman and Windows Latin 1 lead the list only when this host converts them.
    std::erase_if(result, [](std::uint32_t encoding) { return !built_in(encoding) && !converts(encoding); });
    return result;
}

unsigned max_bytes_per_unit(std::uint32_t encoding) {
    const auto name = built_in(encoding) ? std::nullopt : icu_name(encoding);
    if (!name) return 0;
    const auto& api = icu_api();
    Converter converter(api, *name);
    if (!converter.handle) return 0;
    // A surrogate pair is one character: at most the converter's character size per unit.
    return static_cast<unsigned>(std::max<std::int8_t>(api.ucnv_getMaxCharSize(converter.handle), 1));
}

std::optional<std::vector<std::uint16_t>> decode(std::span<const std::uint8_t> bytes, std::uint32_t encoding) {
    const auto name = icu_name(encoding);
    if (!name) throw std::runtime_error("Alchemy CoreFoundation: unsupported string encoding " + std::to_string(encoding));
    const auto& api = icu_api();
    Converter converter(api, *name);
    if (!converter.handle) throw std::runtime_error("Alchemy CoreFoundation: no converter for string encoding " + std::to_string(encoding));
    host::icu::UErrorCode error = host::icu::zero_error;
    api.ucnv_setToUCallBack(converter.handle, api.UCNV_TO_U_CALLBACK_STOP, nullptr, nullptr, nullptr, &error);
    if (bytes.size() > INT32_MAX / 2) throw std::runtime_error("Alchemy CoreFoundation: string too long to convert");
    const auto source = reinterpret_cast<const char*>(bytes.data());
    const auto length = static_cast<std::int32_t>(bytes.size());
    error = host::icu::zero_error;
    const auto needed = api.ucnv_toUChars(converter.handle, nullptr, 0, source, length, &error);
    if (error != host::icu::buffer_overflow_error && host::icu::failure(error)) return std::nullopt;
    std::vector<std::uint16_t> units(static_cast<std::size_t>(needed));
    if (needed == 0) return units;
    api.ucnv_reset(converter.handle);
    error = host::icu::zero_error;
    const auto written = api.ucnv_toUChars(converter.handle, reinterpret_cast<host::icu::UChar*>(units.data()), needed,
        source, length, &error);
    if (host::icu::failure(error)) return std::nullopt;
    units.resize(static_cast<std::size_t>(written));
    return units;
}

Encoded encode(std::span<const std::uint16_t> units, std::uint32_t encoding, std::uint8_t loss, std::uint64_t maximum) {
    const auto name = icu_name(encoding);
    if (!name) throw std::runtime_error("Alchemy CoreFoundation: unsupported string encoding " + std::to_string(encoding));
    const auto& api = icu_api();
    Converter converter(api, *name);
    if (!converter.handle) throw std::runtime_error("Alchemy CoreFoundation: no converter for string encoding " + std::to_string(encoding));
    host::icu::UErrorCode error = host::icu::zero_error;
    if (loss) {
        const char substitute = static_cast<char>(loss);
        api.ucnv_setSubstChars(converter.handle, &substitute, 1, &error);
        if (host::icu::failure(error)) {   // a converter whose characters are wider than one byte
            error = host::icu::zero_error;
            api.ucnv_setFromUCallBack(converter.handle, api.UCNV_FROM_U_CALLBACK_STOP, nullptr, nullptr, nullptr, &error);
            loss = 0;
        }
    } else {
        api.ucnv_setFromUCallBack(converter.handle, api.UCNV_FROM_U_CALLBACK_STOP, nullptr, nullptr, nullptr, &error);
    }
    Encoded result;
    std::array<char, 64> buffer{};
    const auto chars = reinterpret_cast<const host::icu::UChar*>(units.data());
    std::size_t at = 0;
    bool stopped = false;
    while (at < units.size()) {
        // One character at a time (a surrogate pair together), so `units` counts whole characters.
        const std::size_t count = units[at] >= 0xd800 && units[at] < 0xdc00 && at + 1 < units.size() &&
            units[at + 1] >= 0xdc00 && units[at + 1] < 0xe000 ? 2 : 1;
        const host::icu::UChar* source = chars + at;
        char* target = buffer.data();
        error = host::icu::zero_error;
        api.ucnv_fromUnicode(converter.handle, &target, buffer.data() + buffer.size(), &source, chars + at + count, nullptr, 0, &error);
        if (host::icu::failure(error)) { stopped = true; break; }
        const auto size = static_cast<std::size_t>(target - buffer.data());
        if (size > maximum - result.bytes.size()) { stopped = true; break; }
        result.bytes.insert(result.bytes.end(), buffer.data(), target);
        result.units += count;
        at += count;
    }
    if (!stopped) {
        // A stateful encoding (ISO 2022, HZ, UTF-7) ends with its reset sequence.
        const host::icu::UChar* source = chars + at;
        char* target = buffer.data();
        error = host::icu::zero_error;
        api.ucnv_fromUnicode(converter.handle, &target, buffer.data() + buffer.size(), &source, source, nullptr, 1, &error);
        const auto size = static_cast<std::size_t>(target - buffer.data());
        if (!host::icu::failure(error) && size <= maximum - result.bytes.size())
            result.bytes.insert(result.bytes.end(), buffer.data(), target);
    }
    return result;
}

} // namespace alchemy::cf_encodings
