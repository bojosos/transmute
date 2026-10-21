#pragma once

// MTLPixelFormat / MTLVertexFormat <-> VkFormat tables (owner: A5).
// Numeric values follow the public Metal interface (MTLPixelFormat.h,
// MTLVertexDescriptor.h enumerations as documented); no Apple header text.

#include "alchemy/darwin/metal/vulkan.hpp"

#include <cstdint>
#include <string_view>

namespace alchemy::metal {

// MTLPixelFormat values Alchemy translates.
namespace pixel_format {
inline constexpr std::uint32_t invalid = 0, a8_unorm = 1, r8_unorm = 10, r8_unorm_srgb = 11, r8_snorm = 12,
    r8_uint = 13, r8_sint = 14, r16_unorm = 20, r16_snorm = 22, r16_uint = 23, r16_sint = 24, r16_float = 25,
    rg8_unorm = 30, rg8_unorm_srgb = 31, rg8_snorm = 32, rg8_uint = 33, rg8_sint = 34, b5g6r5_unorm = 40,
    a1bgr5_unorm = 41, abgr4_unorm = 42, bgr5a1_unorm = 43, r32_uint = 53, r32_sint = 54, r32_float = 55,
    rg16_unorm = 60, rg16_snorm = 62, rg16_uint = 63, rg16_sint = 64, rg16_float = 65, rgba8_unorm = 70,
    rgba8_unorm_srgb = 71, rgba8_snorm = 72, rgba8_uint = 73, rgba8_sint = 74, bgra8_unorm = 80,
    bgra8_unorm_srgb = 81, rgb10a2_unorm = 90, rgb10a2_uint = 91, rg11b10_float = 92, rgb9e5_float = 93,
    bgr10a2_unorm = 94, rg32_uint = 103, rg32_sint = 104, rg32_float = 105, rgba16_unorm = 110,
    rgba16_snorm = 112, rgba16_uint = 113, rgba16_sint = 114, rgba16_float = 115, rgba32_uint = 123,
    rgba32_sint = 124, rgba32_float = 125, bc1_rgba = 130, bc1_rgba_srgb = 131, bc2_rgba = 132,
    bc2_rgba_srgb = 133, bc3_rgba = 134, bc3_rgba_srgb = 135, bc4_r_unorm = 140, bc4_r_snorm = 141,
    bc5_rg_unorm = 142, bc5_rg_snorm = 143, bc6h_rgb_float = 150, bc6h_rgb_ufloat = 151, bc7_rgba_unorm = 152,
    bc7_rgba_unorm_srgb = 153, depth16_unorm = 250, depth32_float = 252, stencil8 = 253,
    depth24_unorm_stencil8 = 255, depth32_float_stencil8 = 260, x32_stencil8 = 261, x24_stencil8 = 262;
} // namespace pixel_format

enum class FormatClass : std::uint8_t { color_float, color_unorm, color_snorm, color_uint, color_sint, depth, stencil,
    depth_stencil, compressed };

struct PixelFormatInfo {
    std::uint32_t metal{};               // MTLPixelFormat
    VkFormat vulkan{VK_FORMAT_UNDEFINED};
    std::string_view name;               // "MTLPixelFormatRGBA8Unorm" without the prefix
    std::uint8_t block_bytes{};          // bytes per texel (or per 4x4 block when compressed)
    std::uint8_t block_width{1}, block_height{1};
    FormatClass format_class{FormatClass::color_unorm};
    bool srgb{};
    // A swizzle the Vulkan format needs to look like the Metal one (A8Unorm
    // is R8 read as alpha). Identity when all four are IDENTITY.
    VkComponentMapping swizzle{};
    // Stencil-only views of a combined depth/stencil texture
    // (X32_Stencil8, X24_Stencil8): view the stencil aspect of `vulkan`.
    bool stencil_view{};
    [[nodiscard]] bool has_depth() const noexcept {
        return format_class == FormatClass::depth || format_class == FormatClass::depth_stencil;
    }
    [[nodiscard]] bool has_stencil() const noexcept {
        return format_class == FormatClass::stencil || format_class == FormatClass::depth_stencil;
    }
    [[nodiscard]] bool compressed() const noexcept { return format_class == FormatClass::compressed; }
    [[nodiscard]] bool integer() const noexcept {
        return format_class == FormatClass::color_uint || format_class == FormatClass::color_sint;
    }
    [[nodiscard]] VkImageAspectFlags aspects() const noexcept;
};

// nullptr for MTLPixelFormat values Alchemy does not translate (PVRTC, ETC,
// ASTC, YUV, extended-range): callers fail explicitly.
[[nodiscard]] const PixelFormatInfo* pixel_format_info(std::uint32_t metal) noexcept;
// The MTLPixelFormat for a VkFormat (first match), 0 when none.
[[nodiscard]] std::uint32_t pixel_format_from_vulkan(VkFormat format) noexcept;
// The sRGB / linear partner (RGBA8Unorm <-> RGBA8Unorm_sRGB), 0 when none.
[[nodiscard]] std::uint32_t srgb_partner(std::uint32_t metal) noexcept;

// MTLVertexFormat -> VkFormat. Components and byte size are reported so the
// caller can substitute a wider format the driver supports.
struct VertexFormatInfo {
    std::uint32_t metal{};
    VkFormat vulkan{VK_FORMAT_UNDEFINED};
    VkFormat four_component{VK_FORMAT_UNDEFINED};   // substitute for 3-component 8/16-bit formats
    std::uint8_t bytes{};
};
[[nodiscard]] const VertexFormatInfo* vertex_format_info(std::uint32_t metal) noexcept;
// Metal reads an integer vertex attribute into a shader input of either
// signedness (MTLVertexFormatInt2 into a uint2 [[attribute(n)]]); Vulkan
// requires the attribute format's numeric type to match the shader input
// (VUID-VkGraphicsPipelineCreateInfo-Input-08733). Returns the same-width
// integer format of the other signedness when `shader_signed` disagrees with
// `format`, else `format` (also for non-integer formats).
[[nodiscard]] VkFormat vertex_format_with_signedness(VkFormat format, bool shader_signed) noexcept;

// MTLAttributeFormat shares MTLVertexFormat's values.

} // namespace alchemy::metal
