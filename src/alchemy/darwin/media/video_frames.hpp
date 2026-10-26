#pragma once

// Decoded pictures (media_codecs.hpp) as CVPixelBuffer contents (host-only core, media group):
// the planes of the Y'CbCr formats 420v/420f and x420/xf20 (bi-planar 4:2:0, 8 bits or 10 bits
// in the high bits of 16), y420/f420 (tri-planar 4:2:0), 422v/422f and 444v/444f (bi-planar),
// and the RGB formats pixel_convert.hpp writes (BGRA, ...), from any decoded layout: chroma is
// averaged down or replicated up, bit depth shifted with rounding, and video and full range
// rescaled to the format's. RGB goes through pixel_convert with the picture's matrix, from 8-bit
// 4:2:0.

#include "alchemy/darwin/media/media_codecs.hpp"
#include "alchemy/darwin/media/pixel_convert.hpp"

#include <cstdint>
#include <span>

namespace alchemy::gui::completion_kit::media::video_frames {

// The format a decoder hands out for this picture when the client asks for none: 420v/420f for
// 8-bit 4:2:0 (and monochrome), x420/xf20 above 8 bits, 422v/422f and 444v/444f for those layouts
// (8 bits).
[[nodiscard]] std::uint32_t native_format(const codecs::VideoFrame& frame);
// Whether write() produces `format`.
[[nodiscard]] bool writable(std::uint32_t format);
// The picture's Y'CbCr matrix (its matrix coefficients, else BT.601 up to 576 lines, BT.709 above).
[[nodiscard]] pixels::Matrix matrix_of(const codecs::VideoFrame& frame);

// Writes the picture into `planes` (the buffer's, in the format's plane order, of the picture's
// size). False for formats writable() refuses.
bool write(const codecs::VideoFrame& frame, std::uint32_t format, std::span<const pixels::Plane> planes);

} // namespace alchemy::gui::completion_kit::media::video_frames
