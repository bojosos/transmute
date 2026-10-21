#include "alchemy/core_foundation.hpp"
#include "alchemy/host/files.hpp"
#include "alchemy/host/locale.hpp"
#include "alchemy/host/process.hpp"
#include "alchemy/objc_runtime.hpp"
#include "alchemy/path.hpp"
#include "alchemy/registration_audit.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>

namespace alchemy {
namespace {
// CFStringCompare(WithOptions) flags (CFString.h): kCFCompareCaseInsensitive 1,
// Backwards 4 and Anchored 8 (searching only: compare ignores them),
// Nonliteral 16, Localized 32, Numerically 64, DiacriticInsensitive 128,
// WidthInsensitive 256, ForcedOrdering 512. Non-localized comparisons order
// UTF-16 units after the requested folding (case folds to lower case, as
// CoreFoundation does); Localized uses the host's collation for the user's
// locale (CompareStringEx). Returns -1, 0 or 1 (CFComparisonResult).
std::vector<std::uint16_t> compare_fold(std::vector<std::uint16_t> units, std::uint64_t options) {
    namespace locale = host::locale;
    const auto unsupported = [](host::Error error) { return error == host::Error::unsupported; };
    const auto apply = [&](const host::Result<std::u16string>& result) {
        if (result) units.assign(result->begin(), result->end());
        else if (unsupported(result.error())) throw std::runtime_error("CoreFoundation: string comparison options need the host's Unicode tables");
    };
    const auto text = [&] { return std::u16string_view(reinterpret_cast<const char16_t*>(units.data()), units.size()); };
    if (options & (16 | 128)) apply(locale::normalize(text(), locale::Form::d));   // canonical equivalence; split diacritics
    if (options & 128)
        std::erase_if(units, [](std::uint16_t c) {
            return (c >= 0x0300 && c <= 0x036f) || (c >= 0x1ab0 && c <= 0x1aff) || (c >= 0x1dc0 && c <= 0x1dff) ||
                (c >= 0x20d0 && c <= 0x20ff) || (c >= 0xfe20 && c <= 0xfe2f);
        });
    if (options & 256) apply(locale::map("und", text(), locale::Mapping::half_width));
    if (options & 1) {
        const auto lower = locale::map("und", text(), locale::Mapping::lower);
        if (lower || !unsupported(lower.error())) apply(lower);
        else for (auto& c : units) if (c < 128) c = static_cast<std::uint16_t>(std::tolower(c));
    }
    return units;
}

int compare_strings(const std::vector<std::uint16_t>& left, const std::vector<std::uint16_t>& right, std::uint64_t options) {
    constexpr std::uint64_t known = 1 | 4 | 8 | 16 | 32 | 64 | 128 | 256 | 512;
    if (options & ~known) throw std::runtime_error("CoreFoundation: unknown string comparison options");
    const auto literal = [&] { return left < right ? -1 : left > right ? 1 : 0; };
    int result = 0;
    if (options & 32) {
        std::uint32_t flags = 0;
        if (options & 1) flags |= host::locale::ignore_case;
        if (options & 64) flags |= host::locale::digits_as_numbers;
        if (options & 128) flags |= host::locale::ignore_nonspace;
        if (options & 256) flags |= host::locale::ignore_width;
        const auto value = host::locale::compare({}, std::u16string_view(reinterpret_cast<const char16_t*>(left.data()), left.size()),
                                                 std::u16string_view(reinterpret_cast<const char16_t*>(right.data()), right.size()), flags);
        if (!value) {
            if (value.error() == host::Error::unsupported) throw std::runtime_error("CoreFoundation: localized string comparison needs the host's collation");
            throw std::runtime_error("CoreFoundation: localized string comparison failed");
        }
        result = *value;
    } else {
        const auto a = compare_fold(left, options), b = compare_fold(right, options);
        const auto digit = [](std::uint16_t c) { return c >= '0' && c <= '9'; };
        std::size_t i = 0, j = 0;
        while (result == 0 && i < a.size() && j < b.size()) {
            if ((options & 64) && digit(a[i]) && digit(b[j])) {
                // Numerically: digit runs compare by value.
                auto start_a = i, start_b = j;
                while (i < a.size() && digit(a[i])) ++i;
                while (j < b.size() && digit(b[j])) ++j;
                while (start_a + 1 < i && a[start_a] == '0') ++start_a;
                while (start_b + 1 < j && b[start_b] == '0') ++start_b;
                const auto length_a = i - start_a, length_b = j - start_b;
                if (length_a != length_b) result = length_a < length_b ? -1 : 1;
                else for (std::size_t k = 0; k < length_a && result == 0; ++k)
                    if (a[start_a + k] != b[start_b + k]) result = a[start_a + k] < b[start_b + k] ? -1 : 1;
                continue;
            }
            if (a[i] != b[j]) result = a[i] < b[j] ? -1 : 1;
            ++i;
            ++j;
        }
        if (result == 0 && (i < a.size() || j < b.size())) result = i < a.size() ? 1 : -1;
    }
    if (result == 0 && (options & 512)) result = literal();   // kCFCompareForcedOrdering
    return result;
}

constexpr std::uint64_t limit = 64ULL * 1024 * 1024;
constexpr std::uint32_t ascii = 0x600, utf8_encoding = 0x08000100, latin1 = 0x201,
    utf16 = 0x100, utf16be = 0x10000100, utf16le = 0x14000100,
    utf32 = 0x0c000100, utf32be = 0x18000100, utf32le = 0x1c000100;
bool is_utf32(std::uint32_t encoding) { return encoding == utf32 || encoding == utf32be || encoding == utf32le; }
[[noreturn]] void invalid(std::string_view why) { throw std::runtime_error("Alchemy CoreFoundation: " + std::string(why)); }
struct EncodingError : std::runtime_error { explicit EncodingError(std::string_view why) : std::runtime_error("Alchemy CoreFoundation: " + std::string(why)) {} };
[[noreturn]] void invalid_encoding(std::string_view why) { throw EncodingError(why); }
std::uint64_t count(std::uint64_t value) { if (value > limit) invalid("negative or excessive length"); return value; }
void range(std::uint64_t size, std::uint64_t location, std::uint64_t length) {
    if (location > size || length > size - location) invalid("range outside object");
}
std::vector<std::uint8_t> bytes(transmute::GuestMemory& memory, std::uint64_t address, std::uint64_t size) {
    std::vector<std::uint8_t> result(static_cast<std::size_t>(count(size)));
    if (size) memory.read_bytes(address, result);
    return result;
}
std::vector<std::uint16_t> decode(std::span<const std::uint8_t> input, std::uint32_t encoding, bool external = false) {
    std::vector<std::uint16_t> result;
    if (encoding == utf16 || encoding == utf16le || encoding == utf16be) {
        if (input.size() % 2) invalid_encoding("odd UTF-16 byte length");
        bool big = encoding == utf16be;
        std::size_t at = 0;
        if (external && input.size() >= 2) {
            if (input[0] == 0xfe && input[1] == 0xff) { big = true; at = 2; }
            else if (input[0] == 0xff && input[1] == 0xfe) { big = false; at = 2; }
        }
        for (; at < input.size(); at += 2) result.push_back(static_cast<std::uint16_t>(big ?
            (input[at] << 8) | input[at + 1] : input[at] | (input[at + 1] << 8)));
        return result;
    }
    if (is_utf32(encoding)) {
        // kCFStringEncodingUTF32 is native (little-endian) order unless a BOM says otherwise.
        if (input.size() % 4) invalid_encoding("UTF-32 byte length is not a multiple of 4");
        bool big = encoding == utf32be;
        std::size_t at = 0;
        if (external && input.size() >= 4) {
            if (input[0] == 0 && input[1] == 0 && input[2] == 0xfe && input[3] == 0xff) { big = true; at = 4; }
            else if (input[0] == 0xff && input[1] == 0xfe && input[2] == 0 && input[3] == 0) { big = false; at = 4; }
        }
        for (; at < input.size(); at += 4) {
            auto scalar = big ? (static_cast<std::uint32_t>(input[at]) << 24) | (static_cast<std::uint32_t>(input[at + 1]) << 16) |
                    (static_cast<std::uint32_t>(input[at + 2]) << 8) | input[at + 3]
                : input[at] | (static_cast<std::uint32_t>(input[at + 1]) << 8) |
                    (static_cast<std::uint32_t>(input[at + 2]) << 16) | (static_cast<std::uint32_t>(input[at + 3]) << 24);
            if (scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) invalid_encoding("invalid UTF-32 scalar");
            if (scalar < 0x10000) result.push_back(static_cast<std::uint16_t>(scalar));
            else { scalar -= 0x10000; result.push_back(static_cast<std::uint16_t>(0xd800 + (scalar >> 10))); result.push_back(static_cast<std::uint16_t>(0xdc00 + (scalar & 0x3ff))); }
        }
        return result;
    }
    if (encoding == latin1) { result.assign(input.begin(), input.end()); return result; }
    if (encoding != ascii && encoding != utf8_encoding) invalid("unsupported string encoding");
    result.reserve(input.size());
    for (std::size_t at = 0; at < input.size();) {
        const auto first = input[at++];
        if (first < 0x80) { result.push_back(first); continue; }
        if (encoding == ascii) invalid_encoding("non-ASCII byte");
        unsigned more; std::uint32_t scalar, minimum;
        if (first >= 0xc2 && first <= 0xdf) { more = 1; scalar = first & 0x1f; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { more = 2; scalar = first & 0xf; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { more = 3; scalar = first & 7; minimum = 0x10000; }
        else invalid_encoding("malformed UTF-8 lead byte");
        if (input.size() - at < more) invalid_encoding("truncated UTF-8 sequence");
        while (more--) { const auto next = input[at++]; if ((next & 0xc0) != 0x80) invalid_encoding("malformed UTF-8 continuation"); scalar = (scalar << 6) | (next & 0x3f); }
        if (scalar < minimum || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) invalid_encoding("invalid UTF-8 scalar");
        if (scalar < 0x10000) result.push_back(static_cast<std::uint16_t>(scalar));
        else { scalar -= 0x10000; result.push_back(static_cast<std::uint16_t>(0xd800 + (scalar >> 10))); result.push_back(static_cast<std::uint16_t>(0xdc00 + (scalar & 0x3ff))); }
    }
    return result;
}
struct Encoded { std::vector<std::uint8_t> bytes; std::uint64_t units{}; };
Encoded encode(std::span<const std::uint16_t> input, std::uint32_t encoding, std::uint8_t loss,
    bool external, std::uint64_t maximum = UINT64_MAX) {
    if (encoding != ascii && encoding != utf8_encoding && encoding != latin1 && encoding != utf16 && encoding != utf16le && encoding != utf16be && !is_utf32(encoding)) invalid("unsupported string encoding");
    Encoded result;
    if (external && is_utf32(encoding)) {
        if (maximum < 4) return result;
        result.bytes = encoding == utf32be ? std::vector<std::uint8_t>{0, 0, 0xfe, 0xff} : std::vector<std::uint8_t>{0xff, 0xfe, 0, 0};
    }
    if (external && (encoding == utf16 || encoding == utf16le || encoding == utf16be)) {
        if (maximum < 2) return result;
        result.bytes = encoding == utf16be ? std::vector<std::uint8_t>{0xfe, 0xff} : std::vector<std::uint8_t>{0xff, 0xfe};
    }
    for (std::size_t at = 0; at < input.size();) {
        const auto first = input[at]; std::uint32_t scalar = first; unsigned units = 1;
        if (encoding != utf16 && encoding != utf16be && encoding != utf16le) {
            if (first >= 0xd800 && first <= 0xdbff) {
                if (at + 1 < input.size() && input[at + 1] >= 0xdc00 && input[at + 1] <= 0xdfff) {
                    scalar = 0x10000 + ((first - 0xd800) << 10) + input[at + 1] - 0xdc00; units = 2;
                } else scalar = UINT32_MAX;
            } else if (first >= 0xdc00 && first <= 0xdfff) scalar = UINT32_MAX;
        }
        std::array<std::uint8_t, 4> out{}; unsigned size{};
        if (encoding == utf16 || encoding == utf16le || encoding == utf16be) {
            out[encoding == utf16be ? 1 : 0] = static_cast<std::uint8_t>(first);
            out[encoding == utf16be ? 0 : 1] = static_cast<std::uint8_t>(first >> 8); size = 2;
        } else if (is_utf32(encoding) && scalar != UINT32_MAX) {
            for (unsigned byte = 0; byte < 4; ++byte)
                out[encoding == utf32be ? 3 - byte : byte] = static_cast<std::uint8_t>(scalar >> (8 * byte));
            size = 4;
        } else if (is_utf32(encoding)) {
            if (!loss) break;
            out[encoding == utf32be ? 3 : 0] = loss; size = 4;
        } else if (scalar == UINT32_MAX || (encoding == ascii && scalar > 0x7f) || (encoding == latin1 && scalar > 0xff)) {
            if (!loss) break;
            out[0] = loss; size = 1;
        } else if (scalar < 0x80 || encoding == latin1) { out[0] = static_cast<std::uint8_t>(scalar); size = 1; }
        else if (scalar < 0x800) { out[0] = static_cast<std::uint8_t>(0xc0 | scalar >> 6); out[1] = static_cast<std::uint8_t>(0x80 | (scalar & 0x3f)); size = 2; }
        else if (scalar < 0x10000) { out[0] = static_cast<std::uint8_t>(0xe0 | scalar >> 12); out[1] = static_cast<std::uint8_t>(0x80 | ((scalar >> 6) & 0x3f)); out[2] = static_cast<std::uint8_t>(0x80 | (scalar & 0x3f)); size = 3; }
        else { out[0] = static_cast<std::uint8_t>(0xf0 | scalar >> 18); out[1] = static_cast<std::uint8_t>(0x80 | ((scalar >> 12) & 0x3f)); out[2] = static_cast<std::uint8_t>(0x80 | ((scalar >> 6) & 0x3f)); out[3] = static_cast<std::uint8_t>(0x80 | (scalar & 0x3f)); size = 4; }
        if (size > maximum - result.bytes.size()) break;
        result.bytes.insert(result.bytes.end(), out.begin(), out.begin() + size); result.units += units; at += units;
    }
    return result;
}
struct StringPayload : ObjectPayload { std::mutex mutex; std::vector<std::uint16_t> units; std::uint64_t storage{}, utf8_storage{}; std::map<std::uint32_t, std::uint64_t> encoded;
    // CFMutableString: replace_string_units retires (never frees early) inner
    // pointers handed out before a mutation; they are freed with the string.
    bool mutable_string{}; std::vector<std::uint64_t> retired; };
struct DataPayload : ObjectPayload { std::mutex mutex; std::vector<std::uint8_t> data; std::uint64_t storage{}, capacity{}; bool mutable_data{}; };
struct NumberPayload : ObjectPayload { std::int64_t integer{}; double floating{}; unsigned type{}; bool is_float{}; };
int compare_number(const NumberPayload& left, const NumberPayload& right) {
    if (!left.is_float && !right.is_float) return left.integer < right.integer ? -1 : left.integer > right.integer ? 1 : 0;
    if (left.is_float && right.is_float) {
        if (std::isnan(left.floating) || std::isnan(right.floating)) return std::isnan(left.floating) == std::isnan(right.floating) ? 0 : std::isnan(left.floating) ? -1 : 1;
        return left.floating < right.floating ? -1 : left.floating > right.floating ? 1 : 0;
    }
    const auto integer = left.is_float ? right.integer : left.integer;
    const auto floating = left.is_float ? left.floating : right.floating;
    int comparison;
    if (std::isnan(floating)) comparison = 1;
    else if (floating >= 0x1p63) comparison = -1;
    else if (floating < -0x1p63) comparison = 1;
    else { const auto truncated = static_cast<std::int64_t>(floating); comparison = integer < truncated ? -1 : integer > truncated ? 1 : floating > static_cast<double>(truncated) ? -1 : floating < static_cast<double>(truncated) ? 1 : 0; }
    return left.is_float ? -comparison : comparison;
}
int cf_number_compare(const NumberPayload& left, const NumberPayload& right) {
    const bool left_nan = left.is_float && std::isnan(left.floating), right_nan = right.is_float && std::isnan(right.floating);
    auto negative = [](const NumberPayload& p) { return p.is_float ? std::signbit(p.floating) : p.integer < 0; };
    if (left_nan && right_nan) return 0;
    if (left_nan) return negative(right) ? 1 : -1;
    if (right_nan) return negative(left) ? -1 : 1;
    const auto comparison = compare_number(left, right);
    if (!comparison && (left.is_float ? left.floating == 0 : left.integer == 0) && negative(left) != negative(right)) return negative(left) ? -1 : 1;
    return comparison;
}
struct ArrayPayload : ObjectPayload { std::mutex mutex; std::vector<std::uint64_t> values; bool mutable_array{}, own{}; };
struct DictionaryPayload : ObjectPayload { std::mutex mutex; std::vector<std::pair<std::uint64_t, std::uint64_t>> values; std::uint64_t revision{}; bool mutable_dictionary{}, own_keys{}, own_values{}; };
struct UrlPayload : ObjectPayload { std::string path; bool directory{}; std::uint64_t string{}; };
struct DatePayload : ObjectPayload { double time{}; };
struct EnumeratorPayload : ObjectPayload { std::mutex mutex; std::vector<std::uint64_t> urls; std::size_t next{}; };
// Locale identifiers follow the ICU/CFLocale form language[_Script][_COUNTRY][_VARIANT].
struct LocaleParts { std::string identifier, language, script, country, variant; };
struct LocalePayload : ObjectPayload { LocaleParts parts; std::mutex mutex; std::map<std::string, std::uint64_t, std::less<>> values; };
bool alphabetic(std::string_view text) { return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }); }
bool numeric(std::string_view text) { return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; }); }
std::string cased(std::string_view text, bool upper, bool title = false) {
    std::string result(text);
    for (std::size_t at = 0; at < result.size(); ++at) {
        const bool up = title ? at == 0 : upper; auto& c = result[at];
        if (up && c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        else if (!up && c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return result;
}
LocaleParts parse_locale(std::string_view text) {
    std::string value(text.substr(0, std::min(text.size(), std::size_t{256})));
    if (const auto cut = value.find_first_of(".@"); cut != std::string::npos) value.resize(cut);
    std::replace(value.begin(), value.end(), '-', '_');
    std::vector<std::string> parts;
    for (std::size_t at = 0; at <= value.size();) {
        const auto next = std::min(value.find('_', at), value.size());
        if (next > at) parts.push_back(value.substr(at, next - at));
        at = next + 1;
    }
    LocaleParts result;
    if (parts.size() == 1 && (cased(parts[0], false) == "c" || cased(parts[0], false) == "posix")) parts = {"en", "US", "POSIX"};
    std::size_t at = 0;
    if (at < parts.size() && alphabetic(parts[at]) && parts[at].size() >= 2 && parts[at].size() <= 3) result.language = cased(parts[at++], false);
    if (at < parts.size() && alphabetic(parts[at]) && parts[at].size() == 4) result.script = cased(parts[at++], false, true);
    if (at < parts.size() && ((alphabetic(parts[at]) && parts[at].size() == 2) || (numeric(parts[at]) && parts[at].size() == 3))) result.country = cased(parts[at++], true);
    for (; at < parts.size(); ++at) { if (!result.variant.empty()) result.variant += '_'; result.variant += cased(parts[at], true); }
    for (const auto* part : {&result.language, &result.script, &result.country, &result.variant})
        if (!part->empty()) { if (!result.identifier.empty()) result.identifier += '_'; result.identifier += *part; }
    return result;
}
// The current locale is the host user's locale, as macOS answers from the
// user's language and region settings rather than from LANG.
LocaleParts host_locale() {
    std::string value;
    if (const auto name = host::locale::user_locale()) {
        for (const auto c : *name) if (static_cast<unsigned char>(c) < 0x80) value += c;
    } else {
        for (const auto* variable : {"LC_ALL", "LC_MESSAGES", "LANG"})
            if (const auto found = host::process::environment(variable); found && !found->empty()) { value = *found; break; }
    }
    auto result = parse_locale(value);
    if (result.language.empty() || result.variant == "POSIX") result = parse_locale("en_US");
    return result;
}
std::vector<std::uint16_t> ascii_units(std::string_view text) { return {text.begin(), text.end()}; }
std::vector<std::uint16_t> locale_separator(const LocaleParts& parts, bool decimal) {
    std::string name;
    for (const auto* part : {&parts.language, &parts.script, &parts.country})
        if (!part->empty()) { if (!name.empty()) name += '-'; name += *part; }
    const auto value = host::locale::text(name.empty() ? "und" : name,
                                          decimal ? host::locale::Text::decimal_separator : host::locale::Text::grouping_separator);
    if (value && !value->empty())
        return decode(std::span(reinterpret_cast<const std::uint8_t*>(value->data()), value->size()), utf8_encoding);
    return ascii_units(decimal ? "." : ",");
}
// Host volumes appear to the guest as /Volumes/<drive letter>, matching Alchemy's
// guest path translation; elsewhere the root file system is the only volume.
struct HostVolume { std::filesystem::path root; std::string name; bool local{true}, ejectable{}, removable{}, read_only{}; };
std::string utf8_text(const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
std::optional<HostVolume> host_volume(const std::filesystem::path& path) {
    HostVolume result;
    result.root = path.root_path();
    if (result.root.empty()) return std::nullopt;
    // The volume of the path's root (drive letter), not of a folder mounted inside it.
    const auto root = utf8_text(path.root_name()) + std::string(1, static_cast<char>(std::filesystem::path::preferred_separator));
    const auto volume = host::files::volume(root);
    if (!volume) {
        if (volume.error() != host::Error::unsupported) return std::nullopt;
        result.name = "/";   // a host without volume facts: the root file system is the only volume
        return result;
    }
    using Kind = host::files::VolumeKind;
    if (volume->kind == Kind::unknown) return std::nullopt;
    result.root = utf8_path(root);
    result.local = volume->kind != Kind::network;
    result.removable = volume->kind == Kind::removable || volume->kind == Kind::optical;
    result.ejectable = result.removable;
    result.read_only = volume->read_only || volume->kind == Kind::optical;
    result.name = volume->name.empty() ? root.substr(0, 1) : volume->name;
    return result;
}
std::vector<std::filesystem::path> host_volume_roots() {
    std::vector<std::filesystem::path> result;
    const auto roots = host::files::volume_roots();
    if (!roots) {
        if (roots.error() == host::Error::unsupported) result.emplace_back("/");
        return result;
    }
    for (const auto& root : *roots)
        if (host_volume(utf8_path(root))) result.emplace_back(utf8_path(root));
    return result;
}
constexpr double absolute_epoch = 978307200.0; // 2001-01-01T00:00:00Z in Unix seconds.
double absolute_time(std::chrono::system_clock::time_point point) {
    return std::chrono::duration<double>(point.time_since_epoch()).count() - absolute_epoch;
}
constexpr std::string_view url_keys[] = {
    "NameKey", "LocalizedNameKey", "IsRegularFileKey", "IsDirectoryKey", "IsSymbolicLinkKey", "IsVolumeKey",
    "IsPackageKey", "IsHiddenKey", "IsReadableKey", "IsWritableKey", "IsExecutableKey", "IsAliasFileKey",
    "FileSizeKey", "ContentModificationDateKey", "VolumeURLKey", "VolumeNameKey", "VolumeLocalizedNameKey",
    "VolumeIsLocalKey", "VolumeIsEjectableKey", "VolumeIsRemovableKey", "VolumeIsInternalKey", "VolumeIsReadOnlyKey"};
// CF-lite key strings; the NSLocale names are the same objects.
constexpr std::pair<std::string_view, std::string_view> locale_keys[] = {
    {"Identifier", "locale:id"}, {"LanguageCode", "locale:language code"}, {"CountryCode", "locale:country code"},
    {"ScriptCode", "locale:script code"}, {"VariantCode", "locale:variant code"}, {"DecimalSeparator", "decimal"},
    {"GroupingSeparator", "grouping"}, {"UsesMetricSystem", "locale:uses metric"},
    {"MeasurementSystem", "locale:measurement system"}, {"CurrencyCode", "currencyCode"}, {"CurrencySymbol", "currency"}};
struct NumberFormat { unsigned size; bool floating; };
NumberFormat number_format(std::uint64_t type) {
    switch (type) {
    case 1: case 7: return {1, false}; case 2: case 8: return {2, false}; case 3: case 9: return {4, false};
    case 4: case 10: case 11: case 14: case 15: return {8, false}; case 5: case 12: return {4, true};
    case 6: case 13: case 16: return {8, true}; default: invalid("unsupported CFNumber type");
    }
}
std::uint64_t type_id(ObjectKind kind) { return static_cast<std::uint64_t>(kind) + 0x100; }
}

struct CoreFoundation::Impl {
    ProcessContext& context;
    std::map<ObjectKind, std::uint64_t> classes;
    std::map<ObjectKind, std::uint64_t> mutable_classes;
    std::array<std::uint64_t, 5> literal_classes{};
    std::function<bool(std::uint64_t)> foreign_adopter;
    std::vector<std::function<bool(std::uint64_t)>> extra_adopters;
    std::uint64_t constant_class{}, array_callbacks{}, key_callbacks{}, value_callbacks{}, true_object{}, false_object{};
    std::mutex locale_mutex;
    std::uint64_t current_locale{}, system_locale{};
    using Operation = std::function<std::uint64_t(std::span<const std::uint64_t>)>;
    std::map<std::string, std::pair<unsigned, Operation>, std::less<>> operations;
    // CoreFoundation::extend handlers (domain, handler) and the export address
    // of each install()ed function; the URL string handlers likewise.
    using Extensions = std::vector<std::pair<std::string, CoreFoundation::Extension>>;
    std::map<std::string, std::shared_ptr<Extensions>, std::less<>> extensions;
    std::map<std::string, std::uint64_t, std::less<>> function_addresses;
    std::vector<std::pair<std::string, CoreFoundation::UrlStringHandler>> url_string_handlers;
    explicit Impl(ProcessContext& process) : context(process) {}
    transmute::GuestMemory& memory() { return context.calling_engine(); }
    std::uint64_t class_address(ObjectKind kind) const {
        const auto found = classes.find(kind);
        return found == classes.end() ? 0 : found->second;
    }
    std::uint64_t create(ObjectKind kind, std::shared_ptr<ObjectPayload> payload, std::function<void(std::uint64_t)> destroy = {}, bool immortal = false) {
        return context.objects().create(32, class_address(kind), kind, std::move(payload), std::move(destroy), immortal);
    }
    void allocator(std::uint64_t value) { if (value) invalid("custom allocators are unsupported"); }
    std::shared_ptr<ObjectRecord> object(std::uint64_t address) {
        if (!address) invalid("null object");
        if (auto found = context.objects().find(address)) return found;
        if (const auto isa = memory().read64(address); isa) {
            const auto literal = std::find(literal_classes.begin(), literal_classes.end(), isa);
            if (literal != literal_classes.end()) return adopt_literal(address, isa, static_cast<LiteralClass>(literal - literal_classes.begin()));
        }
        // Clang's constant CF strings are guest-owned, immutable 32-byte records.
        const auto isa = memory().read64(address);
        if (isa != constant_class && (!class_address(ObjectKind::string) || isa != class_address(ObjectKind::string))) {
            // Objects the store does not know yet (Objective-C class objects):
            // the foreign-object hook adopts them as immortal records.
            for (const auto& adopter : extra_adopters)
                if (adopter(address)) if (auto adopted = context.objects().find(address)) return adopted;
            if (foreign_adopter && foreign_adopter(address)) if (auto adopted = context.objects().find(address)) return adopted;
            invalid("unknown object reference");
        }
        const auto flags = memory().read32(address + 8); const auto buffer = memory().read64(address + 16); const auto length = count(memory().read64(address + 24));
        auto payload = std::make_shared<StringPayload>();
        if ((flags & 0xfff) == 0x7c8) payload->units = decode(bytes(memory(), buffer, length), utf8_encoding);
        else if ((flags & 0xfff) == 0x7d0) payload->units = decode(bytes(memory(), buffer, length * 2), utf16le);
        else invalid("unsupported constant CFString flags");
        context.objects().adopt(address, 32, class_address(ObjectKind::string), ObjectKind::string, payload, true);
        return context.objects().require(address);
    }
    // Constant literals are immutable guest records: NSConstantIntegerNumber
    // {isa, objCType, value}, NSConstant{Float,Double}Number {isa, value},
    // NSConstantArray {isa, count, objects}, NSConstantDictionary
    // {isa, options, count, keys, objects}. Elements are themselves constants.
    std::shared_ptr<ObjectRecord> adopt_literal(std::uint64_t address, std::uint64_t isa, LiteralClass literal) {
        auto elements = [this](std::uint64_t at, std::uint64_t size) {
            std::vector<std::uint64_t> values(static_cast<std::size_t>(count(size)));
            for (std::size_t index = 0; index < values.size(); ++index) { values[index] = memory().read64(at + index * 8); object(values[index]); }
            return values;
        };
        switch (literal) {
        case LiteralClass::integer: {
            bool terminated{};
            const auto encoding = memory().read_string(memory().read64(address + 8), 1, 4, &terminated);
            if (!terminated || encoding.size() != 1) invalid("malformed constant integer literal encoding");
            auto p = std::make_shared<NumberPayload>(); p->integer = std::bit_cast<std::int64_t>(memory().read64(address + 16));
            switch (encoding[0]) {
            case 'c': p->type = 7; break; case 's': p->type = 8; break; case 'i': p->type = 9; break; case 'l': p->type = 10; break; case 'q': p->type = 11; break;
            case 'C': case 'S': case 'I': case 'L': case 'Q': case 'B':
                if (p->integer < 0) invalid("unsigned constant integer literal above INT64_MAX is unsupported");
                p->type = 4; break;
            default: invalid("unsupported constant integer literal encoding");
            }
            context.objects().adopt(address, 24, isa, ObjectKind::number, p, true); break;
        }
        case LiteralClass::floating: case LiteralClass::double_precision: {
            auto p = std::make_shared<NumberPayload>(); p->is_float = true;
            if (literal == LiteralClass::floating) { p->type = 12; p->floating = std::bit_cast<float>(memory().read32(address + 8)); }
            else { p->type = 13; p->floating = std::bit_cast<double>(memory().read64(address + 8)); }
            context.objects().adopt(address, 16, isa, ObjectKind::number, p, true); break;
        }
        case LiteralClass::array: {
            auto p = std::make_shared<ArrayPayload>(); p->own = true; p->values = elements(memory().read64(address + 16), memory().read64(address + 8));
            context.objects().adopt(address, 24, isa, ObjectKind::array, p, true); break;
        }
        case LiteralClass::dictionary: {
            const auto size = memory().read64(address + 16);
            const auto keys = elements(memory().read64(address + 24), size), values = elements(memory().read64(address + 32), size);
            auto p = std::make_shared<DictionaryPayload>(); p->own_keys = p->own_values = true;
            for (std::size_t index = 0; index < keys.size(); ++index) p->values.emplace_back(keys[index], values[index]);
            context.objects().adopt(address, 40, isa, ObjectKind::dictionary, p, true); break;
        }
        }
        return context.objects().require(address);
    }
    // Mutable collections bridge to their NSMutable* class when one is registered.
    void classify(std::uint64_t address, ObjectKind kind, bool mutable_object) {
        const auto found = mutable_classes.find(kind);
        const auto cls = mutable_object && found != mutable_classes.end() ? found->second : class_address(kind);
        const auto record = context.objects().require(address);
        if (record->class_address == cls) return;
        record->class_address = cls;
        if (record->owned_memory) memory().write64(address, cls);
    }
    template<class T> std::shared_ptr<T> payload(std::uint64_t address, ObjectKind kind) {
        auto record = object(address); if (record->kind != kind) invalid("incorrect object type");
        auto result = std::dynamic_pointer_cast<T>(record->payload); if (!result) invalid("unsupported object representation"); return result;
    }
    std::uint64_t string(std::vector<std::uint16_t> units, bool immortal = false) {
        auto p = std::make_shared<StringPayload>(); p->units = std::move(units);
        p->storage = context.allocate_guest((p->units.size() + 1) * 2);
        std::vector<std::uint8_t> data((p->units.size() + 1) * 2);
        for (std::size_t at = 0; at < p->units.size(); ++at) { data[at * 2] = static_cast<std::uint8_t>(p->units[at]); data[at * 2 + 1] = static_cast<std::uint8_t>(p->units[at] >> 8); }
        memory().write_bytes(p->storage, data);
        return create(ObjectKind::string, p, [this, p](auto) {
            context.release_guest(p->storage); if (p->utf8_storage) context.release_guest(p->utf8_storage);
            for (const auto [encoding, storage] : p->encoded) { static_cast<void>(encoding); context.release_guest(storage); }
            for (const auto storage : p->retired) context.release_guest(storage);
        }, immortal);
    }
    std::uint64_t data(std::vector<std::uint8_t> value, bool mutable_data) {
        auto p = std::make_shared<DataPayload>(); p->data = std::move(value); p->mutable_data = mutable_data;
        p->capacity = std::max<std::uint64_t>(p->data.size(), 1); p->storage = context.allocate_guest(p->capacity); memory().write_bytes(p->storage, p->data);
        const auto result = create(ObjectKind::data, p, [this, p](auto) { context.release_guest(p->storage); });
        classify(result, ObjectKind::data, mutable_data);
        return result;
    }
    void sync_data(DataPayload& p) { if (!p.data.empty()) memory().read_bytes(p.storage, p.data); }
    void write_data(DataPayload& p) {
        if (p.data.size() > p.capacity) { const auto fresh = context.allocate_guest(p.data.size()); context.release_guest(p.storage); p.storage = fresh; p.capacity = p.data.size(); }
        memory().write_bytes(p.storage, p.data);
    }
    bool callbacks(std::uint64_t supplied, std::uint64_t expected, unsigned size) {
        if (!supplied) return false;
        const auto record = bytes(memory(), supplied, size);
        if (std::all_of(record.begin(), record.end(), [](auto byte) { return byte == 0; })) return false;
        if (record != bytes(memory(), expected, size)) invalid("custom collection callbacks are unsupported");
        return true;
    }
    std::uint64_t array(std::vector<std::uint64_t> values, bool mutable_array, bool own) {
        if (own) for (auto value : values) object(value);
        auto p = std::make_shared<ArrayPayload>(); p->values = std::move(values); p->mutable_array = mutable_array; p->own = own;
        std::size_t retained{};
        try {
            if (own) for (auto value : p->values) { static_cast<void>(context.objects().retain(value)); ++retained; }
            const auto result = create(ObjectKind::array, p, [this, p](auto) { if (p->own) for (auto value : p->values) context.objects().release(value); });
            classify(result, ObjectKind::array, mutable_array);
            return result;
        } catch (...) { for (std::size_t at = 0; at < retained; ++at) context.objects().release(p->values[at]); throw; }
    }
    std::uint64_t dictionary(bool mutable_dictionary, bool own_keys, bool own_values) {
        auto p = std::make_shared<DictionaryPayload>(); p->mutable_dictionary = mutable_dictionary; p->own_keys = own_keys; p->own_values = own_values;
        const auto result = create(ObjectKind::dictionary, p, [this, p](auto) { for (auto [key, value] : p->values) { if (p->own_keys) context.objects().release(key); if (p->own_values) context.objects().release(value); } });
        classify(result, ObjectKind::dictionary, mutable_dictionary);
        return result;
    }
    void freeze(std::uint64_t address, bool mutable_dictionary) {
        payload<DictionaryPayload>(address, ObjectKind::dictionary)->mutable_dictionary = mutable_dictionary;
        classify(address, ObjectKind::dictionary, mutable_dictionary);
    }
    std::vector<std::uint64_t> array_values(std::uint64_t address) { const auto p = payload<ArrayPayload>(address, ObjectKind::array); std::lock_guard lock(p->mutex); return p->values; }
    std::vector<std::pair<std::uint64_t, std::uint64_t>> dictionary_values(std::uint64_t address) { const auto p = payload<DictionaryPayload>(address, ObjectKind::dictionary); std::lock_guard lock(p->mutex); return p->values; }
    std::uint64_t length(std::uint64_t address) {
        switch (object(address)->kind) {
        case ObjectKind::string: return payload<StringPayload>(address, ObjectKind::string)->units.size();
        case ObjectKind::data: { const auto p = payload<DataPayload>(address, ObjectKind::data); std::lock_guard lock(p->mutex); return p->data.size(); }
        case ObjectKind::array: return array_values(address).size(); case ObjectKind::dictionary: return dictionary_values(address).size();
        default: invalid("object has no length");
        }
    }
    bool equal(std::uint64_t left, std::uint64_t right, unsigned depth = 0) {
        if (depth > 128) invalid("cyclic or excessively nested equality");
        const auto l = object(left), r = object(right); if (left == right) return true; if (l->kind != r->kind) return false;
        switch (l->kind) {
        case ObjectKind::string: return payload<StringPayload>(left, l->kind)->units == payload<StringPayload>(right, r->kind)->units;
        case ObjectKind::number: case ObjectKind::boolean: {
            const auto a = payload<NumberPayload>(left, l->kind), b = payload<NumberPayload>(right, r->kind);
            return compare_number(*a, *b) == 0;
        }
        case ObjectKind::data: { const auto a = payload<DataPayload>(left, l->kind), b = payload<DataPayload>(right, r->kind); std::scoped_lock lock(a->mutex, b->mutex); sync_data(*a); sync_data(*b); return a->data == b->data; }
        case ObjectKind::array: { const auto a = array_values(left), b = array_values(right); if (a.size() != b.size()) return false; const auto p = payload<ArrayPayload>(left, l->kind), q = payload<ArrayPayload>(right, r->kind); if (p->own != q->own) return false; for (std::size_t at = 0; at < a.size(); ++at) if (p->own ? !equal(a[at], b[at], depth + 1) : a[at] != b[at]) return false; return true; }
        case ObjectKind::dictionary: { const auto a = dictionary_values(left), b = dictionary_values(right); if (a.size() != b.size()) return false; const auto p = payload<DictionaryPayload>(left, l->kind), q = payload<DictionaryPayload>(right, r->kind); if (p->own_keys != q->own_keys || p->own_values != q->own_values) return false; for (const auto [key, value] : a) { const auto found = std::find_if(b.begin(), b.end(), [&](const auto& pair) { return p->own_keys ? equal(key, pair.first, depth + 1) : key == pair.first; }); if (found == b.end() || (p->own_values ? !equal(value, found->second, depth + 1) : value != found->second)) return false; } return true; }
        case ObjectKind::url: { const auto a = payload<UrlPayload>(left, l->kind), b = payload<UrlPayload>(right, r->kind); return a->path == b->path && a->directory == b->directory; }
        case ObjectKind::date: return payload<DatePayload>(left, l->kind)->time == payload<DatePayload>(right, r->kind)->time;
        case ObjectKind::locale: return payload<LocalePayload>(left, l->kind)->parts.identifier == payload<LocalePayload>(right, r->kind)->parts.identifier;
        default: return false;
        }
    }
    std::uint64_t hash(std::uint64_t address, unsigned depth = 0) {
        if (depth > 128) invalid("cyclic or excessively nested hash");
        const auto record = object(address); std::uint64_t result = 1469598103934665603ULL;
        auto add = [&](std::uint64_t value) { result = (result ^ value) * 1099511628211ULL; };
        switch (record->kind) {
        case ObjectKind::string: for (auto value : payload<StringPayload>(address, record->kind)->units) add(value); break;
        case ObjectKind::data: { const auto p = payload<DataPayload>(address, record->kind); std::lock_guard lock(p->mutex); sync_data(*p); for (auto value : p->data) add(value); break; }
        case ObjectKind::number: case ObjectKind::boolean: { const auto p = payload<NumberPayload>(address, record->kind); const auto value = p->is_float ? p->floating : static_cast<double>(p->integer); return std::isnan(value) ? 0x7ff80000 : value == 0 ? 0 : std::bit_cast<std::uint64_t>(value); }
        case ObjectKind::array: { const auto p = payload<ArrayPayload>(address, record->kind); for (auto value : array_values(address)) add(p->own ? hash(value, depth + 1) : value); break; }
        case ObjectKind::dictionary: return dictionary_values(address).size();
        case ObjectKind::url: { const auto p = payload<UrlPayload>(address, record->kind); for (const auto value : p->path) add(static_cast<unsigned char>(value)); add(p->directory); break; }
        case ObjectKind::date: { const auto value = payload<DatePayload>(address, record->kind)->time; return value == 0 ? 0 : std::bit_cast<std::uint64_t>(value); }
        case ObjectKind::locale: for (const auto value : payload<LocalePayload>(address, record->kind)->parts.identifier) add(static_cast<unsigned char>(value)); break;
        default: return address;
        }
        return result;
    }
    std::string description(std::uint64_t address, unsigned depth = 0) {
        if (depth > 32) return "...";
        const auto record = object(address);
        switch (record->kind) {
        case ObjectKind::string: { const auto p = payload<StringPayload>(address, record->kind); const auto out = encode(p->units, utf8_encoding, '?', false); return {reinterpret_cast<const char*>(out.bytes.data()), out.bytes.size()}; }
        case ObjectKind::number: case ObjectKind::boolean: { const auto p = payload<NumberPayload>(address, record->kind); return p->is_float ? std::to_string(p->floating) : std::to_string(p->integer); }
        case ObjectKind::data: { const auto p = payload<DataPayload>(address, record->kind); std::lock_guard lock(p->mutex); sync_data(*p); std::string result = "<"; constexpr char hex[] = "0123456789abcdef"; for (std::size_t at = 0; at < std::min<std::size_t>(p->data.size(), 256); ++at) { result += hex[p->data[at] >> 4]; result += hex[p->data[at] & 15]; } if (p->data.size() > 256) result += "..."; return result + ">"; }
        case ObjectKind::array: { const auto p = payload<ArrayPayload>(address, record->kind); const auto values = array_values(address); std::string result = "("; for (std::size_t at = 0; at < std::min<std::size_t>(values.size(), 128); ++at) { if (at) result += ", "; result += p->own ? description(values[at], depth + 1) : std::to_string(values[at]); } if (values.size() > 128) result += ", ..."; return result + ")"; }
        case ObjectKind::dictionary: { const auto p = payload<DictionaryPayload>(address, record->kind); const auto values = dictionary_values(address); std::string result = "{"; for (std::size_t at = 0; at < std::min<std::size_t>(values.size(), 128); ++at) { if (at) result += ", "; result += p->own_keys ? description(values[at].first, depth + 1) : std::to_string(values[at].first); result += " = "; result += p->own_values ? description(values[at].second, depth + 1) : std::to_string(values[at].second); } if (values.size() > 128) result += ", ..."; return result + "}"; }
        case ObjectKind::url: return payload<UrlPayload>(address, record->kind)->path;
        case ObjectKind::date: return date_description(payload<DatePayload>(address, record->kind)->time);
        case ObjectKind::locale: return payload<LocalePayload>(address, record->kind)->parts.identifier;
        default: return "<Alchemy object " + std::to_string(address) + ">";
        }
    }
    // NSDate description form: "2001-01-01 00:00:00 +0000" (UTC).
    static std::string date_description(double time) {
        if (!std::isfinite(time) || std::abs(time) > 1e15) return std::to_string(time);
        const auto seconds = static_cast<std::int64_t>(std::floor(time + absolute_epoch));
        auto days = seconds >= 0 ? seconds / 86400 : (seconds - 86399) / 86400; const auto rest = seconds - days * 86400;
        days += 719468; const auto era = (days >= 0 ? days : days - 146096) / 146097; const auto doe = days - era * 146097;
        const auto yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; const auto doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        const auto mp = (5 * doy + 2) / 153; const auto day = doy - (153 * mp + 2) / 5 + 1; const auto month = mp < 10 ? mp + 3 : mp - 9;
        const auto year = yoe + era * 400 + (month <= 2);
        std::array<char, 64> text{};
        std::snprintf(text.data(), text.size(), "%04lld-%02lld-%02lld %02lld:%02lld:%02lld +0000", static_cast<long long>(year), static_cast<long long>(month),
            static_cast<long long>(day), static_cast<long long>(rest / 3600), static_cast<long long>(rest / 60 % 60), static_cast<long long>(rest % 60));
        return text.data();
    }
    std::uint64_t copy(std::uint64_t address, bool mutable_copy = false) {
        switch (object(address)->kind) {
        // An immutable string's immutable copy is the same object retained, as
        // CF does (Metal's setLabel: copies on every call). Copies of mutable
        // strings, and mutable copies, get fresh storage.
        case ObjectKind::string: {
            const auto source = payload<StringPayload>(address, ObjectKind::string);
            if (!mutable_copy && !source->mutable_string) return context.objects().retain(address);
            const auto result = string(source->units);
            if (mutable_copy) payload<StringPayload>(result, ObjectKind::string)->mutable_string = true;
            return result;
        }
        case ObjectKind::data: { const auto p = payload<DataPayload>(address, ObjectKind::data); std::lock_guard lock(p->mutex); sync_data(*p); return data(p->data, mutable_copy); }
        case ObjectKind::array: { const auto p = payload<ArrayPayload>(address, ObjectKind::array); return array(array_values(address), mutable_copy, p->own); }
        case ObjectKind::dictionary: { const auto p = payload<DictionaryPayload>(address, ObjectKind::dictionary); const auto result = dictionary(true, p->own_keys, p->own_values); try { for (const auto [key, value] : dictionary_values(address)) dictionary_set(result, key, value); } catch (...) { context.objects().release(result); throw; } freeze(result, mutable_copy); return result; }
        default: return context.objects().retain(address);
        }
    }
    void dictionary_set(std::uint64_t address, std::uint64_t key, std::uint64_t value, unsigned mode = 0) {
        if (!key || !value) invalid("null dictionary key or value");
        const auto p = payload<DictionaryPayload>(address, ObjectKind::dictionary); if (!p->mutable_dictionary) invalid("mutation of immutable dictionary");
        if (p->own_values) object(value);
        if (p->own_keys) object(key);
        for (;;) {
            std::vector<std::pair<std::uint64_t, std::uint64_t>> snapshot;
            std::uint64_t revision;
            { std::lock_guard lock(p->mutex); snapshot = p->values; revision = p->revision; }
            const auto found = std::find_if(snapshot.begin(), snapshot.end(), [&](const auto& pair) { return p->own_keys ? equal(key, pair.first) : key == pair.first; });
            const bool inserting = found == snapshot.end();
            const auto index = static_cast<std::size_t>(found - snapshot.begin());
            std::uint64_t old{};
            bool retained_value = false, retained_key = false;
            try {
                std::lock_guard lock(p->mutex);
                if (revision != p->revision) continue;
                if ((mode == 1 && !inserting) || (mode == 2 && inserting)) return;
                if (inserting) { count(p->values.size() + 1); p->values.reserve(p->values.size() + 1); }
                if (p->own_values) { static_cast<void>(context.objects().retain(value)); retained_value = true; }
                if (inserting) {
                    if (p->own_keys) { static_cast<void>(context.objects().retain(key)); retained_key = true; }
                    p->values.emplace_back(key, value);
                } else { old = p->values[index].second; p->values[index].second = value; }
                ++p->revision;
            } catch (...) {
                // Releasing may invoke a guest destructor. The mutation lock has
                // unwound before either ownership rollback runs.
                if (retained_key) context.objects().release(key);
                if (retained_value) context.objects().release(value);
                throw;
            }
            if (!inserting && p->own_values) context.objects().release(old);
            return;
        }
    }
    std::uint64_t dictionary_value(std::uint64_t address, std::uint64_t key) {
        const auto p = payload<DictionaryPayload>(address, ObjectKind::dictionary);
        const auto values = dictionary_values(address); const auto found = std::find_if(values.begin(), values.end(), [&](const auto& pair) { return p->own_keys ? equal(key, pair.first) : key == pair.first; });
        return found == values.end() ? 0 : found->second;
    }
    std::uint64_t utf8_string(std::string_view text, bool immortal = false) {
        return string(decode(std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()), utf8_encoding), immortal);
    }
    std::string utf8(std::uint64_t address) {
        const auto p = payload<StringPayload>(address, ObjectKind::string);
        // ASCII (Blender's 100 KB MSL sources): one narrowing copy.
        if (std::all_of(p->units.begin(), p->units.end(), [](std::uint16_t unit) { return unit < 0x80; })) {
            std::string ascii_text(p->units.size(), '\0');
            std::transform(p->units.begin(), p->units.end(), ascii_text.begin(), [](std::uint16_t unit) { return static_cast<char>(unit); });
            return ascii_text;
        }
        const auto out = encode(p->units, utf8_encoding, 0, false);
        if (out.units != p->units.size()) invalid("unpaired surrogate cannot be encoded as UTF-8");
        return {reinterpret_cast<const char*>(out.bytes.data()), out.bytes.size()};
    }
    std::uint64_t c_string(std::uint64_t address, std::uint32_t encoding) {
        const auto p = payload<StringPayload>(address, ObjectKind::string);
        if (encoding != ascii && encoding != utf8_encoding && encoding != latin1 && encoding != utf16 && encoding != utf16le && encoding != utf16be && !is_utf32(encoding)) return 0;
        const auto out = encode(p->units, encoding, 0, false);
        if (out.units != p->units.size()) return 0;
        std::lock_guard lock(p->mutex);
        if (encoding == utf8_encoding && p->utf8_storage) return p->utf8_storage;
        if (const auto found = p->encoded.find(encoding); found != p->encoded.end()) return found->second;
        const auto terminator = is_utf32(encoding) ? 4U : encoding == utf16 || encoding == utf16le || encoding == utf16be ? 2U : 1U;
        const auto storage = context.allocate_guest(out.bytes.size() + terminator);
        memory().write_bytes(storage, out.bytes); memory().fill_bytes(storage + out.bytes.size(), 0, terminator);
        if (encoding == utf8_encoding) p->utf8_storage = storage; else p->encoded.emplace(encoding, storage);
        return storage;
    }
    std::uint64_t make_url(std::string_view path, bool directory) {
        if (path.find('\0') != std::string_view::npos) invalid("NUL in file URL path");
        std::filesystem::path native = context.map_guest_path ? context.map_guest_path(path) : utf8_path(path);
        if (native.is_relative()) native = context.working_directory / native;
        auto normalized = guest_path_string(native.lexically_normal());
        while (normalized.size() > 1 && normalized.ends_with('/')) normalized.pop_back(); // CFURL paths drop the directory slash.
        auto p = std::make_shared<UrlPayload>(); p->path = normalized; p->directory = directory;
        std::string text = "file://"; constexpr char hex[] = "0123456789ABCDEF";
        for (auto raw : normalized) {
            const auto c = static_cast<unsigned char>(raw);
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') text += static_cast<char>(c);
            else { text += '%'; text += hex[c >> 4]; text += hex[c & 15]; }
        }
        if (directory && !text.ends_with('/')) text += '/';
        p->string = utf8_string(text);
        return create(ObjectKind::url, p, [this, p](auto) { context.objects().release(p->string); });
    }
    std::filesystem::path host_path(const UrlPayload& p) { return context.map_guest_path ? context.map_guest_path(p.path) : utf8_path(p.path); }
    std::uint64_t date(double time, bool immortal = false) { auto p = std::make_shared<DatePayload>(); p->time = time; return create(ObjectKind::date, p, {}, immortal); }
    std::uint64_t locale(const LocaleParts& parts, bool immortal = false) {
        auto p = std::make_shared<LocalePayload>(); p->parts = parts;
        return create(ObjectKind::locale, p, [this, p](auto) { for (const auto& [key, value] : p->values) { static_cast<void>(key); context.objects().release(value); } }, immortal);
    }
    std::uint64_t current() {
        std::lock_guard lock(locale_mutex);
        if (!current_locale) current_locale = locale(host_locale(), true);
        return current_locale;
    }
    std::uint64_t root_locale() {
        std::lock_guard lock(locale_mutex);
        if (!system_locale) system_locale = locale({}, true);
        return system_locale;
    }
    static std::string bcp47(const LocaleParts& parts) {
        std::string result;
        for (const auto* part : {&parts.language, &parts.script, &parts.country}) if (!part->empty()) { if (!result.empty()) result += '-'; result += *part; }
        return result;
    }
    // CFLocaleGetValue: Get-rule values owned by the locale, created on first use.
    std::uint64_t locale_value(std::uint64_t address, std::string_view key) {
        const auto p = payload<LocalePayload>(address, ObjectKind::locale);
        {
            std::lock_guard lock(p->mutex);
            if (const auto found = p->values.find(key); found != p->values.end()) return found->second;
        }
        const auto& parts = p->parts;
        const bool immortal = object(address)->immortal; // Values of process-lifetime locales live as long.
        std::uint64_t value{};
        auto text = [&](const std::string& field) { return field.empty() ? std::uint64_t{0} : utf8_string(field, immortal); };
        if (key == "locale:id") value = utf8_string(parts.identifier, immortal);
        else if (key == "locale:language code") value = text(parts.language);
        else if (key == "locale:country code") value = text(parts.country);
        else if (key == "locale:script code") value = text(parts.script);
        else if (key == "locale:variant code") value = text(parts.variant);
        else if (key == "decimal" || key == "grouping") value = string(locale_separator(parts, key == "decimal"), immortal);
        else if (key == "locale:uses metric") value = parts.country != "US" && parts.country != "LR" && parts.country != "MM" ? true_object : false_object;
        else if (key == "locale:measurement system") value = utf8_string(parts.country == "US" || parts.country == "LR" || parts.country == "MM" ? "U.S." : "Metric", immortal);
        if (!value) return 0;
        std::lock_guard lock(p->mutex);
        const auto [position, inserted] = p->values.try_emplace(std::string(key), value);
        if (!inserted) { context.objects().release(value); return position->second; }
        return value;
    }
    std::uint64_t locale_value(std::uint64_t address, std::uint64_t key) { return locale_value(address, utf8(key)); }
    // CFURLCopyResourcePropertyForKey over the host file system. Values follow
    // the Copy rule; booleans are the immortal kCFBoolean objects.
    std::optional<std::uint64_t> resource_value(std::uint64_t url, std::uint64_t key) {
        const auto p = payload<UrlPayload>(url, ObjectKind::url);
        auto name = utf8(key);
        if (name.starts_with("NSURL")) name.erase(0, 5);
        std::filesystem::path host;
        try { host = host_path(*p); } catch (const std::runtime_error&) { return std::nullopt; }
        std::error_code error;
        const auto status = std::filesystem::symlink_status(host, error);
        if (error || !std::filesystem::exists(status)) return std::nullopt;
        const auto boolean = [this](bool value) { return value ? true_object : false_object; };
        const auto volume = host_volume(host);
        const bool is_volume = host.lexically_normal() == host.root_path();
        const bool directory = std::filesystem::is_directory(status);
        const auto leaf = [&] { auto value = p->path.substr(p->path.find_last_of('/') + 1); return value.empty() ? p->path : value; };
        const auto permissions = std::filesystem::status(host, error).permissions();
        bool read_only_item = false, hidden_item = false;
        if (const auto attributes = host::files::attributes(utf8_text(host))) {
            read_only_item = !directory && attributes->read_only;
            hidden_item = !is_volume && attributes->hidden;
        } else if (attributes.error() == host::Error::unsupported) {
            read_only_item = (permissions & std::filesystem::perms::owner_write) == std::filesystem::perms::none;
        }
        const auto file_name = leaf();
        if (name == "NameKey" || name == "LocalizedNameKey") return is_volume && volume ? utf8_string(volume->name) : utf8_string(file_name);
        if (name == "IsRegularFileKey") return boolean(std::filesystem::is_regular_file(status));
        if (name == "IsDirectoryKey") return boolean(directory);
        if (name == "IsSymbolicLinkKey" || name == "IsAliasFileKey") return boolean(std::filesystem::is_symlink(status)); // Hosts have no Finder alias files.
        if (name == "IsVolumeKey") return boolean(is_volume);
        if (name == "IsPackageKey") {
            const auto extension = host.extension().string();
            return boolean(directory && (extension == ".app" || extension == ".bundle" || extension == ".framework" || extension == ".plugin"));
        }
        if (name == "IsHiddenKey") return boolean(hidden_item || (file_name.size() > 1 && file_name[0] == '.' && file_name != ".."));
        if (name == "IsReadableKey") return boolean(error ? false : (permissions & std::filesystem::perms::owner_read) != std::filesystem::perms::none);
        if (name == "IsWritableKey") return boolean(!read_only_item && !(volume && volume->read_only));
        if (name == "IsExecutableKey") return boolean(directory || (!error && (permissions & std::filesystem::perms::owner_exec) != std::filesystem::perms::none));
        if (name == "FileSizeKey") {
            if (!std::filesystem::is_regular_file(status)) return std::uint64_t{0};
            const auto size = std::filesystem::file_size(host, error);
            if (error) return std::uint64_t{0};
            auto number = std::make_shared<NumberPayload>(); number->type = 4; number->integer = static_cast<std::int64_t>(size);
            return create(ObjectKind::number, number);
        }
        if (name == "ContentModificationDateKey") {
            const auto written = std::filesystem::last_write_time(host, error);
            if (error) return std::uint64_t{0};
            return date(absolute_time(std::chrono::clock_cast<std::chrono::system_clock>(written)));
        }
        if (!name.starts_with("Volume")) return std::uint64_t{0}; // Properties without a value for this item.
        if (!volume) return std::uint64_t{0};
        if (name == "VolumeURLKey") return make_url(guest_path_string(volume->root), true);
        if (name == "VolumeNameKey" || name == "VolumeLocalizedNameKey") return utf8_string(volume->name);
        if (name == "VolumeIsLocalKey") return boolean(volume->local);
        if (name == "VolumeIsEjectableKey") return boolean(volume->ejectable);
        if (name == "VolumeIsRemovableKey") return boolean(volume->removable);
        if (name == "VolumeIsInternalKey") return boolean(volume->local && !volume->removable);
        if (name == "VolumeIsReadOnlyKey") return boolean(volume->read_only);
        return std::uint64_t{0};
    }
    std::uint64_t volume_enumerator() {
        auto p = std::make_shared<EnumeratorPayload>();
        try { for (const auto& root : host_volume_roots()) p->urls.push_back(make_url(guest_path_string(root), true)); }
        catch (...) { for (const auto value : p->urls) context.objects().release(value); throw; }
        return create(ObjectKind::url_enumerator, p, [this, p](auto) { for (const auto value : p->urls) context.objects().release(value); });
    }
    void install_raw(const std::string& name, transmute::GuestCallHandler handler) {
        static_cast<void>(context.install_function("_" + name, std::move(handler), core_foundation_library));
    }
    std::uint64_t export_object(std::string_view symbol, std::uint64_t value, bool foundation) {
        const auto cell = context.allocate_guest(8); memory().write64(cell, value);
        context.install_export(core_foundation_library, "_" + std::string(symbol), {cell, ExportKind::data});
        if (foundation) context.install_export(foundation_library, "_" + std::string(symbol), {cell, ExportKind::data});
        return value;
    }
    void install_extended();
    void install(std::string name, unsigned arguments, Operation operation) {
        operations.emplace(name, std::make_pair(arguments, operation));
        auto handlers = std::make_shared<Extensions>();
        extensions[name] = handlers;
        function_addresses[name] = context.install_function("_" + name, [name, arguments, operation = std::move(operation), handlers](auto& state, auto& mem) {
            // Every extension is asked, so which one answers never depends on install order.
            std::optional<std::uint64_t> claimed;
            const std::string* claimant{};
            for (const auto& [domain, handler] : *handlers) {
                const auto value = handler(state, mem);
                if (!value) continue;
                if (claimed) invalid(name + ": both the " + *claimant + " and the " + domain + " extensions claim the call");
                claimed = value;
                claimant = &domain;
            }
            if (claimed) { state.x[0] = *claimed; return; }
            std::array<std::uint64_t, 12> args{}; for (unsigned at = 0; at < arguments; ++at) args[at] = at < 8 ? state.x[at] : mem.read64(state.sp + (at - 8) * 8);
            state.x[0] = operation(std::span(args.data(), arguments));
        }, core_foundation_library);
    }
    void install_all();
};

void CoreFoundation::Impl::install_all() {
    // A whole class record (40 bytes): the Foundation bridge builds NSString
    // here (ObjcRuntime::register_native_class_in), so constant strings
    // compiled against this export are NSStrings and the export keeps one owner.
    constant_class = context.allocate_guest(40, 16);
    context.install_export(core_foundation_library, "___CFConstantStringClassReference", {constant_class, ExportKind::data});
    auto constant = [&](std::string name, std::uint64_t value) { const auto address = context.allocate_guest(8); memory().write64(address, value); context.install_export(core_foundation_library, "_" + name, {address, ExportKind::data}); return address; };
    constant("kCFAllocatorDefault", 0); constant("kCFAllocatorSystemDefault", 0);
    auto boolean = [&](bool value) { auto p = std::make_shared<NumberPayload>(); p->integer = value; p->type = 1; return create(ObjectKind::boolean, p, {}, true); };
    true_object = boolean(true); false_object = boolean(false); constant("kCFBooleanTrue", true_object); constant("kCFBooleanFalse", false_object);
    auto number_constant = [&](std::string name, double value) { auto p = std::make_shared<NumberPayload>(); p->type = 6; p->is_float = true; p->floating = value; constant(std::move(name), create(ObjectKind::number, p, {}, true)); };
    number_constant("kCFNumberNaN", std::numeric_limits<double>::quiet_NaN()); number_constant("kCFNumberPositiveInfinity", std::numeric_limits<double>::infinity()); number_constant("kCFNumberNegativeInfinity", -std::numeric_limits<double>::infinity());
    install("CFRetain", 1, [this](auto a) { object(a[0]); return context.objects().retain(a[0]); });
    install("CFRelease", 1, [this](auto a) { object(a[0]); context.objects().release(a[0]); return 0; });
    install("CFGetRetainCount", 1, [this](auto a) { const auto p = object(a[0]); return p->immortal ? static_cast<std::uint64_t>(INT64_MAX) : p->references.load(); });
    install("CFGetTypeID", 1, [this](auto a) { return type_id(object(a[0])->kind); });
    install("CFGetAllocator", 1, [this](auto a) { object(a[0]); return 0; });
    install("CFEqual", 2, [this](auto a) { return equal(a[0], a[1]); });
    install("CFHash", 1, [this](auto a) { return hash(a[0]); });
    install("CFCopyDescription", 1, [this](auto a) { if (object(a[0])->kind == ObjectKind::string) return copy(a[0]); const auto text = description(a[0]); return string(decode(std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()), utf8_encoding)); });
    install("CFCopyTypeIDDescription", 1, [this](auto a) { constexpr std::array names{"AlchemyObject", "CFString", "CFData", "CFNumber", "CFBoolean", "CFArray", "CFDictionary", "CFURL", "CFLocale", "CFDate", "CFURLEnumerator"}; if (a[0] < 0x100 || a[0] - 0x100 >= names.size()) return std::uint64_t{0}; const std::string_view text = names[static_cast<std::size_t>(a[0] - 0x100)]; return string(decode(std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()), utf8_encoding)); });
    for (const auto [name, kind] : std::array<std::pair<const char*, ObjectKind>, 7>{{{"CFStringGetTypeID", ObjectKind::string}, {"CFDataGetTypeID", ObjectKind::data}, {"CFNumberGetTypeID", ObjectKind::number}, {"CFBooleanGetTypeID", ObjectKind::boolean}, {"CFArrayGetTypeID", ObjectKind::array}, {"CFDictionaryGetTypeID", ObjectKind::dictionary}, {"CFURLGetTypeID", ObjectKind::url}}}) install(name, 0, [kind](auto) { return type_id(kind); });
    // Structs expose ordinary callable addresses, while validation admits only the
    // standard records and the all-null callback policy in this milestone.
    auto callback = [&](std::string_view name) { return context.resolve(core_foundation_library, "_" + std::string(name))->address; };
    const auto retain_callback = context.install_function("__alchemy_cf_callback_retain", [this](auto& s, auto&) { object(s.x[1]); s.x[0] = context.objects().retain(s.x[1]); }, core_foundation_library);
    const auto release_callback = context.install_function("__alchemy_cf_callback_release", [this](auto& s, auto&) { object(s.x[1]); context.objects().release(s.x[1]); s.x[0] = 0; }, core_foundation_library);
    auto callback_record = [&](std::string name, bool key) { const auto address = context.allocate_guest(key ? 48 : 40); memory().write64(address, 0); memory().write64(address + 8, retain_callback); memory().write64(address + 16, release_callback); memory().write64(address + 24, callback("CFCopyDescription")); memory().write64(address + 32, callback("CFEqual")); if (key) memory().write64(address + 40, callback("CFHash")); context.install_export(core_foundation_library, "_" + name, {address, ExportKind::data}); return address; };
    array_callbacks = callback_record("kCFTypeArrayCallBacks", false); key_callbacks = callback_record("kCFTypeDictionaryKeyCallBacks", true); value_callbacks = callback_record("kCFTypeDictionaryValueCallBacks", false);
    install("CFStringCreateWithCString", 3, [this](auto a) { allocator(a[0]); const auto encoding = static_cast<std::uint32_t>(a[2]); if (encoding != ascii && encoding != utf8_encoding && encoding != latin1) invalid("C strings require ASCII, Latin-1 or UTF-8 encoding"); bool terminated{}; const auto input = memory().read_string(a[1], 1, limit, &terminated); if (!terminated) invalid("unterminated C string"); try { return string(decode(input, encoding)); } catch (const EncodingError&) { return std::uint64_t{0}; } });
    install("CFStringCreateWithBytes", 5, [this](auto a) { allocator(a[0]); const auto input = bytes(memory(), a[1], a[2]); try { return string(decode(input, static_cast<std::uint32_t>(a[3]), static_cast<std::uint8_t>(a[4]) != 0)); } catch (const EncodingError&) { return std::uint64_t{0}; } });
    install("CFStringCreateWithCharacters", 3, [this](auto a) { allocator(a[0]); return string(decode(bytes(memory(), a[1], count(a[2]) * 2), utf16le)); });
    install("CFStringCreateCopy", 2, [this](auto a) { allocator(a[0]); return string(payload<StringPayload>(a[1], ObjectKind::string)->units); });
    install("CFStringCreateWithSubstring", 4, [this](auto a) { allocator(a[0]); const auto p = payload<StringPayload>(a[1], ObjectKind::string); range(p->units.size(), a[2], a[3]); return string({p->units.begin() + static_cast<std::ptrdiff_t>(a[2]), p->units.begin() + static_cast<std::ptrdiff_t>(a[2] + a[3])}); });
    install("CFStringGetLength", 1, [this](auto a) { return payload<StringPayload>(a[0], ObjectKind::string)->units.size(); });
    install("CFStringGetCharacterAtIndex", 2, [this](auto a) { const auto p = payload<StringPayload>(a[0], ObjectKind::string); range(p->units.size(), a[1], 1); return p->units[static_cast<std::size_t>(a[1])]; });
    install("CFStringGetCharacters", 4, [this](auto a) { const auto p = payload<StringPayload>(a[0], ObjectKind::string); range(p->units.size(), a[1], a[2]); const auto converted = encode(std::span(p->units).subspan(static_cast<std::size_t>(a[1]), static_cast<std::size_t>(a[2])), utf16le, 0, false); memory().write_bytes(a[3], converted.bytes); return 0; });
    install("CFStringGetCharactersPtr", 1, [this](auto a) { return payload<StringPayload>(a[0], ObjectKind::string)->storage; });
    install("CFStringGetCStringPtr", 2, [this](auto a) { const auto p = payload<StringPayload>(a[0], ObjectKind::string); if (a[1] != utf8_encoding && a[1] != ascii) return std::uint64_t{0}; const auto out = encode(p->units, static_cast<std::uint32_t>(a[1]), 0, false); if (out.units != p->units.size()) return std::uint64_t{0}; std::lock_guard lock(p->mutex); if (!p->utf8_storage) { p->utf8_storage = context.allocate_guest(out.bytes.size() + 1); memory().write_bytes(p->utf8_storage, out.bytes); memory().write8(p->utf8_storage + out.bytes.size(), 0); } return p->utf8_storage; });
    install("CFStringGetCString", 4, [this](auto a) { const auto p = payload<StringPayload>(a[0], ObjectKind::string); const auto encoding = static_cast<std::uint32_t>(a[3]); if (encoding != ascii && encoding != utf8_encoding && encoding != latin1) invalid("C strings require ASCII, Latin-1 or UTF-8 encoding"); if (!a[2] || a[2] > limit) return std::uint64_t{0}; const auto out = encode(p->units, encoding, 0, false, a[2] - 1); if (out.units != p->units.size()) return std::uint64_t{0}; memory().write_bytes(a[1], out.bytes); memory().write8(a[1] + out.bytes.size(), 0); return std::uint64_t{1}; });
    install("CFStringGetBytes", 9, [this](auto a) { const auto p = payload<StringPayload>(a[0], ObjectKind::string); range(p->units.size(), a[1], a[2]); const auto out = encode(std::span(p->units).subspan(static_cast<std::size_t>(a[1]), static_cast<std::size_t>(a[2])), static_cast<std::uint32_t>(a[3]), static_cast<std::uint8_t>(a[4]), static_cast<std::uint8_t>(a[5]) != 0, a[6] ? count(a[7]) : UINT64_MAX); if (a[6]) memory().write_bytes(a[6], out.bytes); if (a[8]) memory().write64(a[8], out.bytes.size()); return out.units; });
    install("CFStringGetMaximumSizeForEncoding", 2, [](auto a) { if (a[0] > INT64_MAX) return UINT64_MAX; std::uint64_t factor; switch (a[1]) { case ascii: case latin1: factor = 1; break; case utf8_encoding: factor = 3; break; case utf16: case utf16le: case utf16be: factor = 2; break; case utf32: case utf32le: case utf32be: factor = 4; break; default: invalid("unsupported string encoding"); } return a[0] > INT64_MAX / factor ? UINT64_MAX : a[0] * factor; });
    install("CFStringCompare", 3, [this](auto a) { const auto p = payload<StringPayload>(a[0], ObjectKind::string), q = payload<StringPayload>(a[1], ObjectKind::string); return static_cast<std::uint64_t>(static_cast<std::int64_t>(compare_strings(p->units, q->units, a[2]))); });
    // CFStringCompareWithOptions(string1, string2, CFRange rangeToCompare (of string1), options)
    install("CFStringCompareWithOptions", 5, [this](auto a) { const auto p = payload<StringPayload>(a[0], ObjectKind::string), q = payload<StringPayload>(a[1], ObjectKind::string); range(p->units.size(), a[2], a[3]); const std::vector<std::uint16_t> part(p->units.begin() + static_cast<std::ptrdiff_t>(a[2]), p->units.begin() + static_cast<std::ptrdiff_t>(a[2] + a[3])); return static_cast<std::uint64_t>(static_cast<std::int64_t>(compare_strings(part, q->units, a[4]))); });
    install("CFDataCreate", 3, [this](auto a) { allocator(a[0]); return data(bytes(memory(), a[1], a[2]), false); });
    install("CFDataCreateCopy", 2, [this](auto a) { allocator(a[0]); payload<DataPayload>(a[1], ObjectKind::data); return copy(a[1]); });
    install("CFDataCreateMutable", 2, [this](auto a) { allocator(a[0]); count(a[1]); return data({}, true); });
    install("CFDataCreateMutableCopy", 3, [this](auto a) { allocator(a[0]); count(a[1]); payload<DataPayload>(a[2], ObjectKind::data); return copy(a[2], true); });
    install("CFDataGetLength", 1, [this](auto a) { payload<DataPayload>(a[0], ObjectKind::data); return length(a[0]); });
    install("CFDataGetBytePtr", 1, [this](auto a) { const auto p = payload<DataPayload>(a[0], ObjectKind::data); std::lock_guard lock(p->mutex); return p->storage; });
    install("CFDataGetMutableBytePtr", 1, [this](auto a) { const auto p = payload<DataPayload>(a[0], ObjectKind::data); std::lock_guard lock(p->mutex); if (!p->mutable_data) invalid("mutation of immutable data"); return p->storage; });
    install("CFDataGetBytes", 4, [this](auto a) { const auto p = payload<DataPayload>(a[0], ObjectKind::data); std::lock_guard lock(p->mutex); sync_data(*p); range(p->data.size(), a[1], a[2]); memory().write_bytes(a[3], std::span(p->data).subspan(static_cast<std::size_t>(a[1]), static_cast<std::size_t>(a[2]))); return 0; });
    auto change_data = [this](std::uint64_t address, std::uint64_t at, std::uint64_t removed, std::vector<std::uint8_t> added) { const auto p = payload<DataPayload>(address, ObjectKind::data); std::lock_guard lock(p->mutex); if (!p->mutable_data) invalid("mutation of immutable data"); sync_data(*p); range(p->data.size(), at, removed); count(p->data.size() - removed + added.size()); const auto pos = p->data.begin() + static_cast<std::ptrdiff_t>(at); p->data.erase(pos, pos + static_cast<std::ptrdiff_t>(removed)); p->data.insert(p->data.begin() + static_cast<std::ptrdiff_t>(at), added.begin(), added.end()); write_data(*p); };
    install("CFDataAppendBytes", 3, [this, change_data](auto a) { change_data(a[0], length(a[0]), 0, bytes(memory(), a[1], a[2])); return 0; });
    install("CFDataReplaceBytes", 5, [this, change_data](auto a) { change_data(a[0], a[1], a[2], bytes(memory(), a[3], a[4])); return 0; });
    install("CFDataDeleteBytes", 3, [change_data](auto a) { change_data(a[0], a[1], a[2], {}); return 0; });
    auto resize_data = [this](std::uint64_t address, std::uint64_t size, bool increase) { const auto p = payload<DataPayload>(address, ObjectKind::data); std::lock_guard lock(p->mutex); if (!p->mutable_data) invalid("mutation of immutable data"); sync_data(*p); p->data.resize(static_cast<std::size_t>(count(increase ? p->data.size() + count(size) : size))); write_data(*p); };
    install("CFDataSetLength", 2, [resize_data](auto a) { resize_data(a[0], a[1], false); return 0; });
    install("CFDataIncreaseLength", 2, [resize_data](auto a) { resize_data(a[0], a[1], true); return 0; });
    install("CFNumberCreate", 3, [this](auto a) { allocator(a[0]); const auto format = number_format(a[1]); const auto input = bytes(memory(), a[2], format.size); std::uint64_t bits{}; for (unsigned at = 0; at < format.size; ++at) bits |= static_cast<std::uint64_t>(input[at]) << (at * 8); auto p = std::make_shared<NumberPayload>(); p->type = static_cast<unsigned>(a[1]); p->is_float = format.floating; if (format.floating) p->floating = format.size == 4 ? static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(bits))) : std::bit_cast<double>(bits); else { if (format.size < 8 && (bits & (1ULL << (format.size * 8 - 1)))) bits |= UINT64_MAX << (format.size * 8); p->integer = std::bit_cast<std::int64_t>(bits); } return create(ObjectKind::number, p); });
    install("CFNumberGetType", 1, [this](auto a) { return payload<NumberPayload>(a[0], ObjectKind::number)->type; });
    install("CFNumberGetByteSize", 1, [this](auto a) { return number_format(payload<NumberPayload>(a[0], ObjectKind::number)->type).size; });
    install("CFNumberIsFloatType", 1, [this](auto a) { return payload<NumberPayload>(a[0], ObjectKind::number)->is_float; });
    install("CFNumberGetValue", 3, [this](auto a) {
        const auto p = payload<NumberPayload>(a[0], ObjectKind::number); const auto format = number_format(a[1]);
        std::uint64_t bits{}; bool exact = true;
        if (format.floating) {
            NumberPayload converted; converted.is_float = true;
            const double source = p->is_float ? p->floating : static_cast<double>(p->integer);
            if (format.size == 4) { const auto value = static_cast<float>(source); bits = std::bit_cast<std::uint32_t>(value); converted.floating = value; }
            else { bits = std::bit_cast<std::uint64_t>(source); converted.floating = source; }
            exact = compare_number(*p, converted) == 0;
        } else {
            const auto low = format.size == 8 ? INT64_MIN : -(1LL << (format.size * 8 - 1));
            const auto high = format.size == 8 ? INT64_MAX : (1LL << (format.size * 8 - 1)) - 1;
            std::int64_t value;
            if (!p->is_float) { value = std::clamp(p->integer, static_cast<std::int64_t>(low), static_cast<std::int64_t>(high)); exact = value == p->integer; }
            else if (std::isnan(p->floating)) { value = 0; exact = false; }
            else if (p->floating < static_cast<double>(low)) { value = low; exact = false; }
            else if (p->floating >= (format.size == 8 ? 0x1p63 : static_cast<double>(high) + 1)) { value = high; exact = false; }
            else { value = static_cast<std::int64_t>(p->floating); exact = static_cast<double>(value) == p->floating; }
            bits = std::bit_cast<std::uint64_t>(value);
        }
        std::array<std::uint8_t, 8> output{}; for (unsigned at = 0; at < format.size; ++at) output[at] = static_cast<std::uint8_t>(bits >> (at * 8));
        memory().write_bytes(a[2], std::span(output).first(format.size)); return static_cast<std::uint64_t>(exact);
    });
    install("CFNumberCompare", 3, [this](auto a) { if (a[2]) invalid("CFNumber comparison context must be null"); const auto p = payload<NumberPayload>(a[0], ObjectKind::number), q = payload<NumberPayload>(a[1], ObjectKind::number); return static_cast<std::uint64_t>(static_cast<std::int64_t>(cf_number_compare(*p, *q))); });
    install("CFBooleanGetValue", 1, [this](auto a) { return payload<NumberPayload>(a[0], ObjectKind::boolean)->integer ? 1 : 0; });
    auto guest_values = [this](std::uint64_t address, std::uint64_t size) { count(size); std::vector<std::uint64_t> result; result.reserve(static_cast<std::size_t>(size)); for (std::uint64_t at = 0; at < size; ++at) result.push_back(memory().read64(address + at * 8)); return result; };
    install("CFArrayCreate", 4, [this, guest_values](auto a) { allocator(a[0]); return array(guest_values(a[1], a[2]), false, callbacks(a[3], array_callbacks, 40)); });
    install("CFArrayCreateCopy", 2, [this](auto a) { allocator(a[0]); payload<ArrayPayload>(a[1], ObjectKind::array); return copy(a[1]); });
    install("CFArrayCreateMutable", 3, [this](auto a) { allocator(a[0]); count(a[1]); return array({}, true, callbacks(a[2], array_callbacks, 40)); });
    install("CFArrayCreateMutableCopy", 3, [this](auto a) { allocator(a[0]); count(a[1]); payload<ArrayPayload>(a[2], ObjectKind::array); return copy(a[2], true); });
    install("CFArrayGetCount", 1, [this](auto a) { return array_values(a[0]).size(); });
    install("CFArrayGetValueAtIndex", 2, [this](auto a) { const auto values = array_values(a[0]); range(values.size(), a[1], 1); return values[static_cast<std::size_t>(a[1])]; });
    install("CFArrayGetValues", 4, [this](auto a) { const auto values = array_values(a[0]); range(values.size(), a[1], a[2]); for (std::uint64_t at = 0; at < a[2]; ++at) memory().write64(a[3] + at * 8, values[static_cast<std::size_t>(a[1] + at)]); return 0; });
    install("CFArrayContainsValue", 4, [this](auto a) { const auto p = payload<ArrayPayload>(a[0], ObjectKind::array); const auto values = array_values(a[0]); range(values.size(), a[1], a[2]); for (std::uint64_t at = a[1]; at < a[1] + a[2]; ++at) if (p->own ? equal(values[static_cast<std::size_t>(at)], a[3]) : values[static_cast<std::size_t>(at)] == a[3]) return std::uint64_t{1}; return std::uint64_t{0}; });
    auto array_put = [this](std::uint64_t address, std::uint64_t at, std::uint64_t value, bool insert, bool append) {
        const auto p = payload<ArrayPayload>(address, ObjectKind::array); if (!p->mutable_array) invalid("mutation of immutable array");
        if (p->own) { object(value); static_cast<void>(context.objects().retain(value)); }
        std::uint64_t old{};
        try { std::lock_guard lock(p->mutex); if (append) at = p->values.size(); if (at > p->values.size()) invalid("array index outside object"); if (insert || at == p->values.size()) { count(p->values.size() + 1); p->values.insert(p->values.begin() + static_cast<std::ptrdiff_t>(at), value); } else { old = p->values[static_cast<std::size_t>(at)]; p->values[static_cast<std::size_t>(at)] = value; } }
        catch (...) { if (p->own) context.objects().release(value); throw; }
        if (old && p->own) context.objects().release(old);
    };
    install("CFArrayAppendValue", 2, [array_put](auto a) { array_put(a[0], 0, a[1], false, true); return 0; });
    install("CFArraySetValueAtIndex", 3, [array_put](auto a) { array_put(a[0], a[1], a[2], false, false); return 0; });
    install("CFArrayInsertValueAtIndex", 3, [array_put](auto a) { array_put(a[0], a[1], a[2], true, false); return 0; });
    install("CFArrayRemoveValueAtIndex", 2, [this](auto a) { const auto p = payload<ArrayPayload>(a[0], ObjectKind::array); if (!p->mutable_array) invalid("mutation of immutable array"); std::uint64_t old; { std::lock_guard lock(p->mutex); range(p->values.size(), a[1], 1); old = p->values[static_cast<std::size_t>(a[1])]; p->values.erase(p->values.begin() + static_cast<std::ptrdiff_t>(a[1])); } if (p->own) context.objects().release(old); return 0; });
    install("CFArrayRemoveAllValues", 1, [this](auto a) { const auto p = payload<ArrayPayload>(a[0], ObjectKind::array); if (!p->mutable_array) invalid("mutation of immutable array"); std::vector<std::uint64_t> old; { std::lock_guard lock(p->mutex); old.swap(p->values); } if (p->own) for (auto value : old) context.objects().release(value); return 0; });
    install("CFDictionaryCreate", 6, [this, guest_values](auto a) { allocator(a[0]); const auto keys = guest_values(a[1], a[3]), values = guest_values(a[2], a[3]); const auto result = dictionary(true, callbacks(a[4], key_callbacks, 48), callbacks(a[5], value_callbacks, 40)); try { for (std::size_t at = 0; at < keys.size(); ++at) dictionary_set(result, keys[at], values[at]); } catch (...) { context.objects().release(result); throw; } freeze(result, false); return result; });
    install("CFDictionaryCreateCopy", 2, [this](auto a) { allocator(a[0]); payload<DictionaryPayload>(a[1], ObjectKind::dictionary); return copy(a[1]); });
    install("CFDictionaryCreateMutable", 4, [this](auto a) { allocator(a[0]); count(a[1]); return dictionary(true, callbacks(a[2], key_callbacks, 48), callbacks(a[3], value_callbacks, 40)); });
    install("CFDictionaryCreateMutableCopy", 3, [this](auto a) { allocator(a[0]); count(a[1]); payload<DictionaryPayload>(a[2], ObjectKind::dictionary); return copy(a[2], true); });
    install("CFDictionaryGetCount", 1, [this](auto a) { return dictionary_values(a[0]).size(); });
    install("CFDictionaryGetValue", 2, [this](auto a) { return dictionary_value(a[0], a[1]); });
    install("CFDictionaryGetValueIfPresent", 3, [this](auto a) { const auto value = dictionary_value(a[0], a[1]); if (value && a[2]) memory().write64(a[2], value); return value ? 1 : 0; });
    install("CFDictionaryContainsKey", 2, [this](auto a) { return dictionary_value(a[0], a[1]) ? 1 : 0; });
    install("CFDictionaryContainsValue", 2, [this](auto a) { const auto p = payload<DictionaryPayload>(a[0], ObjectKind::dictionary); const auto values = dictionary_values(a[0]); for (const auto& pair : values) if (p->own_values ? equal(pair.second, a[1]) : pair.second == a[1]) return 1; return 0; });
    install("CFDictionaryGetKeysAndValues", 3, [this](auto a) { const auto values = dictionary_values(a[0]); for (std::size_t at = 0; at < values.size(); ++at) { if (a[1]) memory().write64(a[1] + at * 8, values[at].first); if (a[2]) memory().write64(a[2] + at * 8, values[at].second); } return 0; });
    install("CFDictionarySetValue", 3, [this](auto a) { dictionary_set(a[0], a[1], a[2]); return 0; });
    install("CFDictionaryAddValue", 3, [this](auto a) { dictionary_set(a[0], a[1], a[2], 1); return 0; });
    install("CFDictionaryReplaceValue", 3, [this](auto a) { dictionary_set(a[0], a[1], a[2], 2); return 0; });
    auto dictionary_remove = [this](std::uint64_t address, std::uint64_t key, bool all) {
        const auto p = payload<DictionaryPayload>(address, ObjectKind::dictionary);
        if (!p->mutable_dictionary) invalid("mutation of immutable dictionary");
        std::vector<std::pair<std::uint64_t, std::uint64_t>> removed;
        if (all) { std::lock_guard lock(p->mutex); removed.swap(p->values); ++p->revision; }
        else for (;;) {
            std::vector<std::pair<std::uint64_t, std::uint64_t>> snapshot;
            std::uint64_t revision;
            { std::lock_guard lock(p->mutex); snapshot = p->values; revision = p->revision; }
            const auto found = std::find_if(snapshot.begin(), snapshot.end(), [&](const auto& pair) { return p->own_keys ? equal(key, pair.first) : key == pair.first; });
            const auto index = static_cast<std::size_t>(found - snapshot.begin());
            {
                std::lock_guard lock(p->mutex);
                if (revision != p->revision) continue;
                if (found != snapshot.end()) { removed.push_back(p->values[index]); p->values.erase(p->values.begin() + static_cast<std::ptrdiff_t>(index)); ++p->revision; }
            }
            break;
        }
        for (auto [old_key, value] : removed) { if (p->own_keys) context.objects().release(old_key); if (p->own_values) context.objects().release(value); }
    };
    install("CFDictionaryRemoveValue", 2, [dictionary_remove](auto a) { dictionary_remove(a[0], a[1], false); return 0; });
    install("CFDictionaryRemoveAllValues", 1, [dictionary_remove](auto a) { dictionary_remove(a[0], 0, true); return 0; });
    auto url = [this](std::string_view path, bool directory) { return make_url(path, directory); };
    install("CFURLCreateWithFileSystemPath", 4, [this, url](auto a) { allocator(a[0]); if (a[2] != 0) invalid("only POSIX file URL paths are supported"); const auto p = payload<StringPayload>(a[1], ObjectKind::string); const auto out = encode(p->units, utf8_encoding, 0, false); if (out.units != p->units.size()) invalid("file URL has invalid Unicode"); return url(std::string_view(reinterpret_cast<const char*>(out.bytes.data()), out.bytes.size()), a[3] != 0); });
    install("CFURLCreateFromFileSystemRepresentation", 4, [this, url](auto a) { allocator(a[0]); const auto input = bytes(memory(), a[1], a[2]); decode(input, utf8_encoding); return url(std::string_view(reinterpret_cast<const char*>(input.data()), input.size()), a[3] != 0); });
    install("CFURLCopyFileSystemPath", 2, [this](auto a) { if (a[1] != 0) invalid("only POSIX file URL paths are supported"); const auto p = payload<UrlPayload>(a[0], ObjectKind::url); return string(decode(std::span(reinterpret_cast<const std::uint8_t*>(p->path.data()), p->path.size()), utf8_encoding)); });
    install("CFURLGetFileSystemRepresentation", 4, [this](auto a) { const auto p = payload<UrlPayload>(a[0], ObjectKind::url); if (a[3] <= p->path.size() || a[3] > limit) return 0; memory().write_bytes(a[2], std::span(reinterpret_cast<const std::uint8_t*>(p->path.data()), p->path.size())); memory().write8(a[2] + p->path.size(), 0); return 1; });
    install("CFURLHasDirectoryPath", 1, [this](auto a) { return payload<UrlPayload>(a[0], ObjectKind::url)->directory; });
    install("CFURLCopyAbsoluteURL", 1, [this](auto a) { payload<UrlPayload>(a[0], ObjectKind::url); return context.objects().retain(a[0]); });
    install("CFURLGetString", 1, [this](auto a) { return payload<UrlPayload>(a[0], ObjectKind::url)->string; });
    install_extended();
}

void CoreFoundation::Impl::install_extended() {
    // __kCFBooleanTrue/False are the boolean objects themselves (clang emits them for @YES/@NO).
    context.install_export(core_foundation_library, "___kCFBooleanTrue", {true_object, ExportKind::data});
    context.install_export(core_foundation_library, "___kCFBooleanFalse", {false_object, ExportKind::data});
    // __NSArray0__struct / __NSDictionary0__struct are the immutable empty
    // array and dictionary singletons themselves (clang emits them for @[] and
    // @{}); __NSArray0__ / __NSDictionary0__ point to them. Immortal.
    {
        const auto empty_array = create(ObjectKind::array, std::make_shared<ArrayPayload>(), {}, true);
        classify(empty_array, ObjectKind::array, false);
        context.install_export(core_foundation_library, "___NSArray0__struct", {empty_array, ExportKind::data});
        export_object("__NSArray0__", empty_array, false);
        const auto empty_dictionary = create(ObjectKind::dictionary, std::make_shared<DictionaryPayload>(), {}, true);
        classify(empty_dictionary, ObjectKind::dictionary, false);
        context.install_export(core_foundation_library, "___NSDictionary0__struct", {empty_dictionary, ExportKind::data});
        export_object("__NSDictionary0__", empty_dictionary, false);
    }
    for (const auto key : url_keys) {
        const auto value = utf8_string("NSURL" + std::string(key), true);
        export_object("NSURL" + std::string(key), value, true);
        export_object("kCFURL" + std::string(key), value, false);
    }
    for (const auto* key : {"Version", "Identifier", "Name", "Executable", "InfoDictionaryVersion", "DevelopmentRegion", "Localizations"})
        export_object("kCFBundle" + std::string(key) + "Key", utf8_string("CFBundle" + std::string(key), true), false);
    for (const auto& [cf, ns] : {std::pair{"kCFRunLoopDefaultMode", "NSDefaultRunLoopMode"}, std::pair{"kCFRunLoopCommonModes", "NSRunLoopCommonModes"}}) {
        const auto value = utf8_string(cf, true);
        export_object(cf, value, false); export_object(ns, value, true);
    }
    for (const auto& [name, key] : locale_keys) {
        const auto value = utf8_string(key, true);
        export_object("kCFLocale" + std::string(name), value, false); export_object("NSLocale" + std::string(name), value, true);
    }
    for (const auto [name, kind] : std::array<std::pair<const char*, ObjectKind>, 3>{{{"CFLocaleGetTypeID", ObjectKind::locale}, {"CFDateGetTypeID", ObjectKind::date}, {"CFURLEnumeratorGetTypeID", ObjectKind::url_enumerator}}})
        install(name, 0, [kind](auto) { return type_id(kind); });
    install("CFLocaleCopyCurrent", 0, [this](auto) { return context.objects().retain(current()); });
    install("CFLocaleGetSystem", 0, [this](auto) { return root_locale(); });
    install("CFLocaleCreate", 2, [this](auto a) { allocator(a[0]); if (!a[1]) return std::uint64_t{0}; return locale(parse_locale(utf8(a[1]))); });
    install("CFLocaleCreateCopy", 2, [this](auto a) { allocator(a[0]); payload<LocalePayload>(a[1], ObjectKind::locale); return context.objects().retain(a[1]); });
    install("CFLocaleGetIdentifier", 1, [this](auto a) { return locale_value(a[0], std::string_view("locale:id")); });
    install("CFLocaleGetValue", 2, [this](auto a) { return locale_value(a[0], a[1]); });
    install("CFLocaleCopyPreferredLanguages", 0, [this](auto) {
        const auto language = utf8_string(bcp47(payload<LocalePayload>(current(), ObjectKind::locale)->parts));
        try { const auto result = array({language}, false, true); context.objects().release(language); return result; }
        catch (...) { context.objects().release(language); throw; }
    });
    install("CFLocaleCreateCanonicalLocaleIdentifierFromString", 2, [this](auto a) { allocator(a[0]); return utf8_string(parse_locale(utf8(a[1])).identifier); });
    install("CFLocaleCreateCanonicalLanguageIdentifierFromString", 2, [this](auto a) { allocator(a[0]); return utf8_string(bcp47(parse_locale(utf8(a[1])))); });
    auto set_double = [](transmute::CpuState& state, double value) { state.v[0] = {std::bit_cast<std::uint64_t>(value), 0}; };
    install_raw("CFAbsoluteTimeGetCurrent", [set_double](auto& state, auto&) { set_double(state, absolute_time(std::chrono::system_clock::now())); });
    install_raw("CFDateCreate", [this](auto& state, auto&) { allocator(state.x[0]); state.x[0] = date(std::bit_cast<double>(state.v[0][0])); });
    install_raw("CFDateGetAbsoluteTime", [this, set_double](auto& state, auto&) { set_double(state, payload<DatePayload>(state.x[0], ObjectKind::date)->time); });
    install_raw("CFDateGetTimeIntervalSinceDate", [this, set_double](auto& state, auto&) {
        set_double(state, payload<DatePayload>(state.x[0], ObjectKind::date)->time - payload<DatePayload>(state.x[1], ObjectKind::date)->time);
    });
    install("CFDateCompare", 3, [this](auto a) {
        const auto left = payload<DatePayload>(a[0], ObjectKind::date)->time, right = payload<DatePayload>(a[1], ObjectKind::date)->time;
        return left < right ? UINT64_MAX : left > right ? std::uint64_t{1} : std::uint64_t{0};
    });
    install("CFURLCopyResourcePropertyForKey", 4, [this](auto a) {
        if (!a[2]) invalid("CFURLCopyResourcePropertyForKey requires a value pointer");
        const auto value = resource_value(a[0], a[1]);
        memory().write64(a[2], value.value_or(0));
        if (!value && a[3]) memory().write64(a[3], 0); // CFError objects are not produced; the failure is the result.
        return value ? std::uint64_t{1} : std::uint64_t{0};
    });
    install("CFURLEnumeratorCreateForMountedVolumes", 3, [this](auto a) {
        allocator(a[0]);
        if (a[2]) payload<ArrayPayload>(a[2], ObjectKind::array); // Prefetch keys only; properties are read on demand.
        return volume_enumerator();
    });
    install("CFURLEnumeratorGetNextURL", 3, [this](auto a) {
        const auto p = payload<EnumeratorPayload>(a[0], ObjectKind::url_enumerator);
        std::lock_guard lock(p->mutex);
        if (p->next >= p->urls.size()) return std::uint64_t{2}; // kCFURLEnumeratorEnd
        if (a[1]) memory().write64(a[1], p->urls[p->next]);
        ++p->next;
        return std::uint64_t{1}; // kCFURLEnumeratorSuccess
    });
}

CoreFoundation::CoreFoundation(ProcessContext& process) : impl_(std::make_unique<Impl>(process)) {
    impl_->install_all();
    process.adopt_object = [this](std::uint64_t address) { try { static_cast<void>(impl_->object(address)); return true; } catch (const std::runtime_error&) { return false; } };
}
CoreFoundation::~CoreFoundation() { impl_->context.adopt_object = {}; }
void CoreFoundation::set_class(ObjectKind kind, std::uint64_t address) { impl_->classes[kind] = address; impl_->context.objects().set_class(kind, address); }
void CoreFoundation::set_foreign_adopter(std::function<bool(std::uint64_t)> adopter) { impl_->foreign_adopter = std::move(adopter); }
void CoreFoundation::add_foreign_adopter(std::function<bool(std::uint64_t)> adopter) { impl_->extra_adopters.push_back(std::move(adopter)); }
void CoreFoundation::set_mutable_class(ObjectKind kind, std::uint64_t address) { impl_->mutable_classes[kind] = address; }
void CoreFoundation::set_literal_class(LiteralClass literal, std::uint64_t address) { impl_->literal_classes.at(static_cast<std::size_t>(literal)) = address; }
std::uint64_t CoreFoundation::constant_string(std::string_view value) { return impl_->utf8_string(value, true); }
std::vector<std::uint16_t> CoreFoundation::units(std::uint64_t object) { return impl_->payload<StringPayload>(object, ObjectKind::string)->units; }
std::uint64_t CoreFoundation::c_string(std::uint64_t object, std::uint32_t encoding) { return impl_->c_string(object, encoding); }
std::uint64_t CoreFoundation::mutable_copy(std::uint64_t object) { return impl_->copy(object, true); }
void CoreFoundation::replace_string_units(std::uint64_t object, std::vector<std::uint16_t> units) {
    const auto p = impl_->payload<StringPayload>(object, ObjectKind::string);
    std::lock_guard lock(p->mutex);
    if (!p->mutable_string) invalid("mutation of an immutable string");
    const auto storage = impl_->context.allocate_guest((units.size() + 1) * 2);
    std::vector<std::uint8_t> data((units.size() + 1) * 2);
    for (std::size_t at = 0; at < units.size(); ++at) { data[at * 2] = static_cast<std::uint8_t>(units[at]); data[at * 2 + 1] = static_cast<std::uint8_t>(units[at] >> 8); }
    impl_->memory().write_bytes(storage, data);
    p->retired.push_back(p->storage);
    if (p->utf8_storage) p->retired.push_back(p->utf8_storage);
    for (const auto [encoding, old] : p->encoded) { static_cast<void>(encoding); p->retired.push_back(old); }
    p->utf8_storage = 0; p->encoded.clear();
    p->storage = storage; p->units = std::move(units);
}
std::uint64_t CoreFoundation::description(std::uint64_t object) { return impl_->utf8_string(impl_->description(object)); }
std::optional<ObjectKind> CoreFoundation::kind(std::uint64_t object) {
    try { return impl_->object(object)->kind; } catch (const std::runtime_error&) { return std::nullopt; }
}
bool CoreFoundation::is_mutable(std::uint64_t object) {
    switch (impl_->object(object)->kind) {
    case ObjectKind::array: return impl_->payload<ArrayPayload>(object, ObjectKind::array)->mutable_array;
    case ObjectKind::dictionary: return impl_->payload<DictionaryPayload>(object, ObjectKind::dictionary)->mutable_dictionary;
    case ObjectKind::data: return impl_->payload<DataPayload>(object, ObjectKind::data)->mutable_data;
    case ObjectKind::string: return impl_->payload<StringPayload>(object, ObjectKind::string)->mutable_string;
    default: return false;
    }
}
std::uint64_t CoreFoundation::array(std::span<const std::uint64_t> values, bool mutable_array) { return impl_->array({values.begin(), values.end()}, mutable_array, true); }
std::uint64_t CoreFoundation::dictionary(std::span<const std::pair<std::uint64_t, std::uint64_t>> values, bool mutable_dictionary) {
    const auto result = impl_->dictionary(true, true, true);
    try { for (const auto& [key, value] : values) impl_->dictionary_set(result, key, value); }
    catch (...) { impl_->context.objects().release(result); throw; }
    impl_->freeze(result, mutable_dictionary);
    return result;
}
std::vector<std::uint64_t> CoreFoundation::array_values(std::uint64_t object) { return impl_->array_values(object); }
std::vector<std::pair<std::uint64_t, std::uint64_t>> CoreFoundation::dictionary_entries(std::uint64_t object) { return impl_->dictionary_values(object); }
std::uint64_t CoreFoundation::integer(std::int64_t value, unsigned type) {
    if (number_format(type).floating) invalid("integer CFNumber requires an integer type");
    auto p = std::make_shared<NumberPayload>(); p->type = type; p->integer = value; return impl_->create(ObjectKind::number, p);
}
std::uint64_t CoreFoundation::real(double value) { auto p = std::make_shared<NumberPayload>(); p->type = 13; p->is_float = true; p->floating = value; return impl_->create(ObjectKind::number, p); }
std::uint64_t CoreFoundation::boolean(bool value) const { return value ? impl_->true_object : impl_->false_object; }
std::optional<double> CoreFoundation::number_value(std::uint64_t object) {
    const auto record = impl_->object(object);
    if (record->kind != ObjectKind::number && record->kind != ObjectKind::boolean) return std::nullopt;
    const auto p = impl_->payload<NumberPayload>(object, record->kind);
    return p->is_float ? p->floating : static_cast<double>(p->integer);
}
std::uint64_t CoreFoundation::date(double absolute_time, bool immortal) { return impl_->date(absolute_time, immortal); }
double CoreFoundation::date_time(std::uint64_t object) { return impl_->payload<DatePayload>(object, ObjectKind::date)->time; }
double CoreFoundation::absolute_time_now() { return absolute_time(std::chrono::system_clock::now()); }
std::uint64_t CoreFoundation::file_url(std::string_view guest_path, bool directory) { return impl_->make_url(guest_path, directory); }
std::string CoreFoundation::url_path(std::uint64_t object) { return impl_->payload<UrlPayload>(object, ObjectKind::url)->path; }
bool CoreFoundation::url_is_directory(std::uint64_t object) { return impl_->payload<UrlPayload>(object, ObjectKind::url)->directory; }
std::optional<std::uint64_t> CoreFoundation::resource_value(std::uint64_t url, std::uint64_t key) { return impl_->resource_value(url, key); }
std::uint64_t CoreFoundation::locale(std::string_view identifier) { return impl_->locale(parse_locale(identifier)); }
std::uint64_t CoreFoundation::current_locale() { return impl_->current(); }
std::uint64_t CoreFoundation::string(std::string_view value) { return impl_->string(decode(std::span(reinterpret_cast<const std::uint8_t*>(value.data()), value.size()), utf8_encoding)); }
std::uint64_t CoreFoundation::string_utf16(std::span<const std::uint16_t> units) { return impl_->string({units.begin(), units.end()}); }
std::string CoreFoundation::utf8(std::uint64_t object) { return impl_->utf8(object); }
std::uint64_t CoreFoundation::length(std::uint64_t object) { return impl_->length(object); }
std::uint64_t CoreFoundation::element(std::uint64_t object, std::uint64_t index) { const auto values = impl_->array_values(object); range(values.size(), index, 1); return values[static_cast<std::size_t>(index)]; }
std::uint64_t CoreFoundation::dictionary_value(std::uint64_t object, std::uint64_t key) { return impl_->dictionary_value(object, key); }
bool CoreFoundation::equal(std::uint64_t left, std::uint64_t right) { return impl_->equal(left, right); }
std::uint64_t CoreFoundation::hash(std::uint64_t object) { return impl_->hash(object); }
std::uint64_t CoreFoundation::copy(std::uint64_t object) { return impl_->copy(object); }
void CoreFoundation::extend(std::string_view function, std::string_view domain, Extension handler, const std::source_location& where) {
    const auto found = impl_->extensions.find(function);
    if (found == impl_->extensions.end()) invalid("CoreFoundation does not implement " + std::string(function));
    for (const auto& entry : *found->second)
        if (entry.first == domain) invalid(std::string(function) + " is already extended for " + std::string(domain));
    registration_audit::record_extension(std::string(core_foundation_library) + " _" + std::string(function), domain, where);
    found->second->emplace_back(std::string(domain), std::move(handler));
}
bool CoreFoundation::implements(std::string_view function) const {
    const auto found = impl_->function_addresses.find(function);
    if (found == impl_->function_addresses.end()) return false;
    const ProcessContext::RuntimeLookup lookup;   // never a probe stub
    const auto bound = impl_->context.resolve(core_foundation_library, "_" + std::string(function));
    return bound && bound->address == found->second;
}
void CoreFoundation::extend_url_strings(std::string_view domain, UrlStringHandler handler, const std::source_location& where) {
    for (const auto& entry : impl_->url_string_handlers)
        if (entry.first == domain) invalid("NSURL strings are already extended for " + std::string(domain));
    registration_audit::record_extension(std::string(foundation_library) + " +[NSURL URLWithString:] -[NSURL initWithString:]", domain, where);
    impl_->url_string_handlers.emplace_back(std::string(domain), std::move(handler));
}
std::optional<std::uint64_t> CoreFoundation::handle_url_string(std::uint64_t string) {
    std::optional<std::uint64_t> claimed;
    const std::string* claimant{};
    for (const auto& [domain, handler] : impl_->url_string_handlers) {
        const auto value = handler(string);
        if (!value) continue;
        if (claimed) invalid("NSURL string: both the " + *claimant + " and the " + domain + " extensions claim it");
        claimed = value;
        claimant = &domain;
    }
    return claimed;
}
std::uint64_t CoreFoundation::invoke(std::string_view symbol, std::span<const std::uint64_t> arguments) { if (symbol.starts_with('_')) symbol.remove_prefix(1); const auto found = impl_->operations.find(symbol); if (found == impl_->operations.end()) invalid("unsupported API " + std::string(symbol)); if (arguments.size() != found->second.first) invalid("incorrect argument count for " + std::string(symbol)); return found->second.second(arguments); }
} // namespace alchemy
