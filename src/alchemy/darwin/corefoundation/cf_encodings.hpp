#pragma once

// CFStringEncoding facts and the encodings CoreFoundation converts through the
// host's ICU (host::icu). The built-in encodings (UTF-8/16/32, ASCII, ISO Latin 1)
// stay in core_foundation.cpp; every other encoding with an ICU converter
// (Mac OS Roman and the other Mac scripts, the DOS and Windows code pages, ISO
// 8859, the CJK sets, ISO 2022, KOI8, EBCDIC ...) converts here, as Apple's CF
// converts its non-built-in encodings through ICU. The encoding database
// (names, IANA names, Windows code pages, NSStringEncoding values, the most
// compatible Mac encoding) is ported from swift-corelibs-foundation
// (third_party/swift-corelibs-foundation, Apache-2.0).

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy::cf_encodings {

inline constexpr std::uint32_t invalid_id = 0xffffffffu;   // kCFStringEncodingInvalidId
inline constexpr std::uint32_t mac_roman = 0, unicode = 0x0100, utf8 = 0x08000100, utf16 = 0x0100,
    utf16be = 0x10000100, utf16le = 0x14000100, utf32 = 0x0c000100, utf32be = 0x18000100, utf32le = 0x1c000100,
    utf7 = 0x04000100, utf7_imap = 0x0a10, iso_latin1 = 0x0201, windows_latin1 = 0x0500, ascii = 0x0600,
    nonlossy_ascii = 0x0bff, nextstep_latin = 0x0b01, dos_japanese = 0x0420, euc_jp = 0x0920, mac_symbol = 33,
    iso_latin2 = 0x0202, windows_cyrillic = 0x0502, windows_greek = 0x0503, windows_latin5 = 0x0504,
    windows_latin2 = 0x0501, iso_2022_jp = 0x0820;

// CFStringConvertEncodingToWindowsCodepage (0 when the encoding has none) and back.
[[nodiscard]] std::uint32_t windows_codepage(std::uint32_t encoding);
[[nodiscard]] std::uint32_t from_windows_codepage(std::uint32_t codepage);
// CFStringConvertEncodingToIANACharSetName ("utf-8", "iso-8859-2", "x-mac-japanese").
[[nodiscard]] std::optional<std::string> iana_name(std::uint32_t encoding);
// CFStringConvertIANACharSetNameToEncoding: CF's own names, then ICU's aliases.
[[nodiscard]] std::uint32_t from_iana_name(std::string_view name);
// CFStringGetNameOfEncoding ("Western (Mac OS Roman)").
[[nodiscard]] std::optional<std::string_view> display_name(std::uint32_t encoding);
// CFStringConvertEncodingToNSStringEncoding and CFStringConvertNSStringEncodingToEncoding.
[[nodiscard]] std::uint64_t to_ns(std::uint32_t encoding);
[[nodiscard]] std::uint32_t from_ns(std::uint64_t encoding);
// CFStringGetMostCompatibleMacStringEncoding.
[[nodiscard]] std::uint32_t most_compatible_mac(std::uint32_t encoding);
// Every encoding this process converts (built-in and ICU), for
// CFStringGetListOfAvailableEncodings, in CF's database order.
[[nodiscard]] std::vector<std::uint32_t> available();

// The ICU-converted encodings. converts() is false for the built-in ones and
// for encodings with no converter on this host (no ICU, or none for it).
[[nodiscard]] bool converts(std::uint32_t encoding);
// The longest byte sequence of one UTF-16 unit, 0 when not converted here.
[[nodiscard]] unsigned max_bytes_per_unit(std::uint32_t encoding);
// Bytes to UTF-16; nullopt when the bytes are not valid in the encoding.
[[nodiscard]] std::optional<std::vector<std::uint16_t>> decode(std::span<const std::uint8_t> bytes, std::uint32_t encoding);
// UTF-16 to bytes, at most `maximum` of them. A character the encoding lacks
// becomes `loss` (when not 0) or ends the conversion; `units` is how many
// units were converted.
struct Encoded {
    std::vector<std::uint8_t> bytes;
    std::uint64_t units{};
};
[[nodiscard]] Encoded encode(std::span<const std::uint16_t> units, std::uint32_t encoding, std::uint8_t loss,
                             std::uint64_t maximum);

} // namespace alchemy::cf_encodings
