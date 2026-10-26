#pragma once

// RTF text extraction (owner: AppKit; -[NSText readRTFDFromFile:], QLab's
// license agreement). Project-authored from the RTF 1.x syntax: groups,
// control words with optional numeric parameters, control symbols, and the
// \'hh, \uN (with \ucN fallback skipping) escapes.
//   Text:        plain characters; \par, \line, \sect, \page and a
//                backslash-newline end a paragraph ("\n"); \tab is "\t";
//                \emdash \endash \bullet \lquote \rquote \ldblquote
//                \rdblquote \~ \_ map to their characters; \- is dropped.
//   Skipped:     destinations (\fonttbl, \colortbl, \stylesheet, \info,
//                \pict, \header..., and every \* group).
//   Code pages:  \ansi / \ansicpg1252 (Windows-1252) for \'hh; other code
//                pages fail explicitly.
// plain_text keeps no formatting. runs() keeps the formatting AppKit's text
// actions exchange (and write() writes): the font table's family names, \fs
// sizes, \b, \i, \ul, the colour table and \cf, and the paragraph's \ql /
// \qr / \qc / \qj alignment and \li / \fi / \ri indents (\pard and \plain
// reset them); other formatting is skipped. Malformed input (unbalanced
// groups, truncated escapes) throws std::runtime_error.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace alchemy::gui::rtf {

[[nodiscard]] bool looks_like_rtf(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::u16string plain_text(std::span<const std::uint8_t> bytes);

struct Format {
    std::string font;                             // font family ("": the default)
    double size{12};                              // points
    bool bold{}, italic{}, underline{};
    std::optional<std::array<double, 3>> color;   // sRGB 0..1 (none: automatic)
    int alignment{4};                             // NSTextAlignment: left 0, center 1, right 2, justified 3, natural 4
    double left_indent{}, first_indent{}, right_indent{};   // points; \fi is relative to \li
    bool operator==(const Format&) const = default;
};
struct Run {
    std::u16string text;
    Format format;
};
// The text in runs of equal formatting.
[[nodiscard]] std::vector<Run> runs(std::span<const std::uint8_t> bytes);
// An RTF 1 document of the runs (Windows-1252 with \u escapes beyond it).
[[nodiscard]] std::string write(const std::vector<Run>& runs);

} // namespace alchemy::gui::rtf
