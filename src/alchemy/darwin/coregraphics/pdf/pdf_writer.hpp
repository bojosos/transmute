#pragma once

// A PDF file writer (host-only; ../pdf_context.cpp builds its documents
// with it): numbered objects, streams (Flate-compressed when asked), a
// cross-reference table and the trailer, and the syntax helpers PDF
// objects need (numbers, names, strings).

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy::gui::quartz2d::pdf {

class Writer {
public:
    // A new object number (its body is set later).
    int reserve();
    // The body of object `number` ("<< ... >>", "[ ... ]", a stream from stream_body ...).
    void set(int number, std::string body);
    // An object holding `body`, numbered now.
    int add(std::string body);
    // The file: header, objects, cross-reference table, trailer (/Root, /Info when non-zero, /ID).
    [[nodiscard]] std::vector<std::uint8_t> finish(int root, int info, std::string_view version = "1.4") const;

private:
    std::vector<std::string> bodies_;   // object n is bodies_[n - 1]
};

// A stream object's body: `dictionary_entries` (without << >> and /Length),
// then the data, Flate-compressed (with /Filter) when `compress`.
[[nodiscard]] std::string stream_body(std::string_view dictionary_entries, std::span<const std::uint8_t> data, bool compress);
[[nodiscard]] std::string stream_body(std::string_view dictionary_entries, std::string_view data, bool compress);

// A number as PDF writes it (no exponent, up to 4 decimals, no trailing zeros).
[[nodiscard]] std::string number(double value);
// "/Name" with the characters PDF names cannot hold escaped as #xx.
[[nodiscard]] std::string name(std::string_view text);
// A literal string "( ... )" of `bytes`.
[[nodiscard]] std::string literal(std::string_view bytes);
// A text string: a literal for ASCII, else UTF-16BE with a byte order mark (hexadecimal).
[[nodiscard]] std::string text(std::u16string_view units);
// A PDF date string "(D:YYYYMMDDHHmmSSZ)" for seconds since 2001-01-01 UTC.
[[nodiscard]] std::string date(double absolute_seconds);

} // namespace alchemy::gui::quartz2d::pdf
