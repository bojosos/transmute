#include "alchemy/darwin/appkit/appkit_internal.hpp"

#include "alchemy/host/gpu.hpp"

// Image file decoding and encoding for NSImage / NSBitmapImageRep /
// NSPasteboard: the host's codecs (host::gpu; the Windows Imaging Component
// on win32: PNG, TIFF, JPEG, BMP, GIF, ICO). Apple-only formats (ICNS, HEIC
// without the codec pack) decode to nothing, which AppKit reports as a nil
// image.
namespace alchemy::gui::appkit {

std::optional<DecodedImage> decode_image(std::span<const std::uint8_t> bytes) {
    auto decoded = host::gpu::decode_image(bytes);
    if (!decoded) return std::nullopt;
    DecodedImage result;
    result.pixels.width = decoded->width;
    result.pixels.height = decoded->height;
    result.pixels.rgba = std::move(decoded->rgba);
    return result;
}

std::optional<std::vector<std::uint8_t>> encode_image(const Image& pixels, ImageFileType type) {
    host::gpu::Pixels source{pixels.width, pixels.height, pixels.rgba};
    const auto format = type == ImageFileType::tiff ? host::gpu::ImageFormat::tiff
                      : type == ImageFileType::bmp ? host::gpu::ImageFormat::bmp
                      : type == ImageFileType::gif ? host::gpu::ImageFormat::gif
                      : type == ImageFileType::jpeg ? host::gpu::ImageFormat::jpeg : host::gpu::ImageFormat::png;
    auto encoded = host::gpu::encode_image(source, format);
    if (!encoded) return std::nullopt;
    return std::move(*encoded);
}

} // namespace alchemy::gui::appkit
