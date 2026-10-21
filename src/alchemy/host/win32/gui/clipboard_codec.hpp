#pragma once

// Text and image conversions between AppKit pasteboard representations and
// the Win32 clipboard formats (pure functions; no clipboard access).

#include "alchemy/gui/window_server.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy::gui::win32 {

// UTF-8 <-> UTF-16. Ill-formed input becomes U+FFFD; never throws for bad
// encodings.
[[nodiscard]] std::wstring utf8_to_wide(std::string_view text);
[[nodiscard]] std::string wide_to_utf8(std::wstring_view text);
void append_utf8(std::string& output, char32_t code_point);

// CF_UNICODETEXT payload (up to the first NUL) -> NSPasteboard string: CRLF
// line ends become LF, as AppKit text uses.
[[nodiscard]] std::string clipboard_text_from_windows(std::wstring_view text);
// NSPasteboard string -> CF_UNICODETEXT (NUL-terminated, LF -> CRLF; an
// existing CRLF is kept).
[[nodiscard]] std::wstring clipboard_text_to_windows(std::string_view text);

// gui::Image -> packed CF_DIBV5 (BITMAPV5HEADER, 32 bpp BI_BITFIELDS with an
// alpha mask, bottom-up rows, sRGB, straight alpha).
[[nodiscard]] std::vector<std::uint8_t> image_to_dibv5(const Image& image);
// Packed DIB (CF_DIB or CF_DIBV5: BITMAPINFOHEADER/V4/V5; 1/4/8 bpp
// palettized, 16/24/32 bpp BI_RGB or BI_BITFIELDS; top-down or bottom-up) ->
// gui::Image. A 32 bpp BI_RGB bitmap whose alpha bytes are all zero is
// opaque. Throws std::runtime_error naming the unsupported feature
// (compressed formats, malformed headers).
[[nodiscard]] Image image_from_dib(const std::uint8_t* data, std::size_t size);

} // namespace alchemy::gui::win32
