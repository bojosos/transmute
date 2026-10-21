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
// Formatting (fonts, sizes, colors, alignment) is not kept: AppKit's
// NSTextView holds one attribute run. Malformed input (unbalanced groups,
// truncated escapes) throws std::runtime_error.

#include <cstdint>
#include <span>
#include <string>

namespace alchemy::gui::rtf {

[[nodiscard]] bool looks_like_rtf(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::u16string plain_text(std::span<const std::uint8_t> bytes);

} // namespace alchemy::gui::rtf
