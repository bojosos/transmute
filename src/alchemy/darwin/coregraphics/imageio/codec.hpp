#pragma once

// Image file codecs for ImageIO and CoreGraphics (owner: Q): PNG, JPEG,
// TIFF, BMP, GIF and ICO through the host (host::gpu; WIC on win32), and Apple
// .icns parsed in-house (PNG / ARGB / RLE RGB + mask entries; JPEG 2000
// entries are reported as undecodable).

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace alchemy::gui::quartz2d::codec {

// Uniform type identifiers.
inline constexpr const char* png = "public.png";
inline constexpr const char* jpeg = "public.jpeg";
inline constexpr const char* tiff = "public.tiff";
inline constexpr const char* bmp = "com.microsoft.bmp";
inline constexpr const char* gif = "com.compuserve.gif";
inline constexpr const char* ico = "com.microsoft.ico";
inline constexpr const char* icns = "com.apple.icns";

struct Frame {
    std::uint32_t width{}, height{};
    std::vector<std::uint8_t> rgba;           // straight RGBA8, top row first
    bool has_alpha{};
    double dpi_x{72}, dpi_y{72};
    double delay{};                           // GIF frame delay, seconds
    std::uint32_t orientation{1};             // EXIF orientation (1: up)
    std::uint32_t bits_per_component{8};      // of the file (reported in properties)
};

// The file type from the leading bytes; nullopt for unknown data.
[[nodiscard]] std::optional<std::string> sniff(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::size_t frame_count(std::span<const std::uint8_t> bytes);
// Decodes frame `index`; nullopt when the data is not decodable.
[[nodiscard]] std::optional<Frame> decode(std::span<const std::uint8_t> bytes, std::size_t index = 0);
// Header information without decoding pixels (sizes for properties); falls back to decode.
[[nodiscard]] std::optional<Frame> describe(std::span<const std::uint8_t> bytes, std::size_t index = 0);
// Encodes frames as `type` (png, jpeg, tiff, bmp, gif); quality in 0..1 for JPEG.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode(const std::string& type, const std::vector<Frame>& frames, double quality = 0.85);
[[nodiscard]] std::vector<std::string> readable_types();
[[nodiscard]] std::vector<std::string> writable_types();

} // namespace alchemy::gui::quartz2d::codec
