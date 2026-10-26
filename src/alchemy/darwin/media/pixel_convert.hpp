#pragma once

// Pixel-format conversion and scaling for VTPixelTransferSession and vImage (host-only core).
// Images are converted through straight RGBA in [0, 1]: Y'CbCr formats with the BT.601, BT.709
// or BT.2020 matrix in video or full range (8-bit, or 10 bits in the high bits of 16), chroma
// upsampled by replication and downsampled by averaging. Scaling is bilinear (area-averaged when
// shrinking by more than 2). There is no color management: primaries and transfer functions
// are kept as they are.

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace alchemy::gui::completion_kit::media::pixels {

enum class Matrix { bt601, bt709, bt2020 };

struct Plane {
    std::uint8_t* data{};
    std::uint64_t bytes_per_row{};
};

struct Image {
    std::uint32_t format{};            // kCVPixelFormatType_*
    std::uint32_t width{}, height{};
    std::vector<Plane> planes;         // in the format's plane order
    Matrix matrix{Matrix::bt709};
};

// Whether `format` converts here; the number of planes it has (0 when it does not convert).
[[nodiscard]] unsigned plane_count(std::uint32_t format);
// The matrix a Y'CbCr image without a matrix attachment uses (BT.601 up to 576 lines).
[[nodiscard]] Matrix default_matrix(std::uint32_t width, std::uint32_t height);

// The image as straight RGBA floats (width * height * 4).
[[nodiscard]] std::vector<float> to_rgba(const Image& image);
// Writes RGBA floats of the image's size into the image.
void from_rgba(std::span<const float> rgba, Image& image);
// Resizes RGBA floats.
[[nodiscard]] std::vector<float> scale(std::span<const float> rgba, std::uint32_t width, std::uint32_t height, std::uint32_t to_width,
                                       std::uint32_t to_height);

} // namespace alchemy::gui::completion_kit::media::pixels
