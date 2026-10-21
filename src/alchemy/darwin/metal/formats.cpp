#include "alchemy/darwin/metal/formats.hpp"

#include <array>
#include <utility>

// MTLPixelFormat / MTLVertexFormat translation tables. Packed 16- and
// 32-bit formats pair the way their documented bit layouts agree
// (B5G6R5Unorm = VK_FORMAT_R5G6B5_UNORM_PACK16, RGB10A2Unorm =
// VK_FORMAT_A2B10G10R10_UNORM_PACK32, ...).
namespace alchemy::metal {
namespace {

using F = FormatClass;
constexpr VkComponentMapping identity{};
constexpr VkComponentMapping alpha_only{VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO,
                                        VK_COMPONENT_SWIZZLE_R};

constexpr PixelFormatInfo pixel_formats[] = {
    {pixel_format::a8_unorm, VK_FORMAT_R8_UNORM, "A8Unorm", 1, 1, 1, F::color_unorm, false, alpha_only},
    {pixel_format::r8_unorm, VK_FORMAT_R8_UNORM, "R8Unorm", 1, 1, 1, F::color_unorm},
    {pixel_format::r8_unorm_srgb, VK_FORMAT_R8_SRGB, "R8Unorm_sRGB", 1, 1, 1, F::color_unorm, true},
    {pixel_format::r8_snorm, VK_FORMAT_R8_SNORM, "R8Snorm", 1, 1, 1, F::color_snorm},
    {pixel_format::r8_uint, VK_FORMAT_R8_UINT, "R8Uint", 1, 1, 1, F::color_uint},
    {pixel_format::r8_sint, VK_FORMAT_R8_SINT, "R8Sint", 1, 1, 1, F::color_sint},
    {pixel_format::r16_unorm, VK_FORMAT_R16_UNORM, "R16Unorm", 2, 1, 1, F::color_unorm},
    {pixel_format::r16_snorm, VK_FORMAT_R16_SNORM, "R16Snorm", 2, 1, 1, F::color_snorm},
    {pixel_format::r16_uint, VK_FORMAT_R16_UINT, "R16Uint", 2, 1, 1, F::color_uint},
    {pixel_format::r16_sint, VK_FORMAT_R16_SINT, "R16Sint", 2, 1, 1, F::color_sint},
    {pixel_format::r16_float, VK_FORMAT_R16_SFLOAT, "R16Float", 2, 1, 1, F::color_float},
    {pixel_format::rg8_unorm, VK_FORMAT_R8G8_UNORM, "RG8Unorm", 2, 1, 1, F::color_unorm},
    {pixel_format::rg8_unorm_srgb, VK_FORMAT_R8G8_SRGB, "RG8Unorm_sRGB", 2, 1, 1, F::color_unorm, true},
    {pixel_format::rg8_snorm, VK_FORMAT_R8G8_SNORM, "RG8Snorm", 2, 1, 1, F::color_snorm},
    {pixel_format::rg8_uint, VK_FORMAT_R8G8_UINT, "RG8Uint", 2, 1, 1, F::color_uint},
    {pixel_format::rg8_sint, VK_FORMAT_R8G8_SINT, "RG8Sint", 2, 1, 1, F::color_sint},
    {pixel_format::b5g6r5_unorm, VK_FORMAT_R5G6B5_UNORM_PACK16, "B5G6R5Unorm", 2, 1, 1, F::color_unorm},
    {pixel_format::a1bgr5_unorm, VK_FORMAT_R5G5B5A1_UNORM_PACK16, "A1BGR5Unorm", 2, 1, 1, F::color_unorm},
    {pixel_format::abgr4_unorm, VK_FORMAT_R4G4B4A4_UNORM_PACK16, "ABGR4Unorm", 2, 1, 1, F::color_unorm},
    {pixel_format::bgr5a1_unorm, VK_FORMAT_A1R5G5B5_UNORM_PACK16, "BGR5A1Unorm", 2, 1, 1, F::color_unorm},
    {pixel_format::r32_uint, VK_FORMAT_R32_UINT, "R32Uint", 4, 1, 1, F::color_uint},
    {pixel_format::r32_sint, VK_FORMAT_R32_SINT, "R32Sint", 4, 1, 1, F::color_sint},
    {pixel_format::r32_float, VK_FORMAT_R32_SFLOAT, "R32Float", 4, 1, 1, F::color_float},
    {pixel_format::rg16_unorm, VK_FORMAT_R16G16_UNORM, "RG16Unorm", 4, 1, 1, F::color_unorm},
    {pixel_format::rg16_snorm, VK_FORMAT_R16G16_SNORM, "RG16Snorm", 4, 1, 1, F::color_snorm},
    {pixel_format::rg16_uint, VK_FORMAT_R16G16_UINT, "RG16Uint", 4, 1, 1, F::color_uint},
    {pixel_format::rg16_sint, VK_FORMAT_R16G16_SINT, "RG16Sint", 4, 1, 1, F::color_sint},
    {pixel_format::rg16_float, VK_FORMAT_R16G16_SFLOAT, "RG16Float", 4, 1, 1, F::color_float},
    {pixel_format::rgba8_unorm, VK_FORMAT_R8G8B8A8_UNORM, "RGBA8Unorm", 4, 1, 1, F::color_unorm},
    {pixel_format::rgba8_unorm_srgb, VK_FORMAT_R8G8B8A8_SRGB, "RGBA8Unorm_sRGB", 4, 1, 1, F::color_unorm, true},
    {pixel_format::rgba8_snorm, VK_FORMAT_R8G8B8A8_SNORM, "RGBA8Snorm", 4, 1, 1, F::color_snorm},
    {pixel_format::rgba8_uint, VK_FORMAT_R8G8B8A8_UINT, "RGBA8Uint", 4, 1, 1, F::color_uint},
    {pixel_format::rgba8_sint, VK_FORMAT_R8G8B8A8_SINT, "RGBA8Sint", 4, 1, 1, F::color_sint},
    {pixel_format::bgra8_unorm, VK_FORMAT_B8G8R8A8_UNORM, "BGRA8Unorm", 4, 1, 1, F::color_unorm},
    {pixel_format::bgra8_unorm_srgb, VK_FORMAT_B8G8R8A8_SRGB, "BGRA8Unorm_sRGB", 4, 1, 1, F::color_unorm, true},
    {pixel_format::rgb10a2_unorm, VK_FORMAT_A2B10G10R10_UNORM_PACK32, "RGB10A2Unorm", 4, 1, 1, F::color_unorm},
    {pixel_format::rgb10a2_uint, VK_FORMAT_A2B10G10R10_UINT_PACK32, "RGB10A2Uint", 4, 1, 1, F::color_uint},
    {pixel_format::rg11b10_float, VK_FORMAT_B10G11R11_UFLOAT_PACK32, "RG11B10Float", 4, 1, 1, F::color_float},
    {pixel_format::rgb9e5_float, VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, "RGB9E5Float", 4, 1, 1, F::color_float},
    {pixel_format::bgr10a2_unorm, VK_FORMAT_A2R10G10B10_UNORM_PACK32, "BGR10A2Unorm", 4, 1, 1, F::color_unorm},
    {pixel_format::rg32_uint, VK_FORMAT_R32G32_UINT, "RG32Uint", 8, 1, 1, F::color_uint},
    {pixel_format::rg32_sint, VK_FORMAT_R32G32_SINT, "RG32Sint", 8, 1, 1, F::color_sint},
    {pixel_format::rg32_float, VK_FORMAT_R32G32_SFLOAT, "RG32Float", 8, 1, 1, F::color_float},
    {pixel_format::rgba16_unorm, VK_FORMAT_R16G16B16A16_UNORM, "RGBA16Unorm", 8, 1, 1, F::color_unorm},
    {pixel_format::rgba16_snorm, VK_FORMAT_R16G16B16A16_SNORM, "RGBA16Snorm", 8, 1, 1, F::color_snorm},
    {pixel_format::rgba16_uint, VK_FORMAT_R16G16B16A16_UINT, "RGBA16Uint", 8, 1, 1, F::color_uint},
    {pixel_format::rgba16_sint, VK_FORMAT_R16G16B16A16_SINT, "RGBA16Sint", 8, 1, 1, F::color_sint},
    {pixel_format::rgba16_float, VK_FORMAT_R16G16B16A16_SFLOAT, "RGBA16Float", 8, 1, 1, F::color_float},
    {pixel_format::rgba32_uint, VK_FORMAT_R32G32B32A32_UINT, "RGBA32Uint", 16, 1, 1, F::color_uint},
    {pixel_format::rgba32_sint, VK_FORMAT_R32G32B32A32_SINT, "RGBA32Sint", 16, 1, 1, F::color_sint},
    {pixel_format::rgba32_float, VK_FORMAT_R32G32B32A32_SFLOAT, "RGBA32Float", 16, 1, 1, F::color_float},
    {pixel_format::bc1_rgba, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, "BC1_RGBA", 8, 4, 4, F::compressed},
    {pixel_format::bc1_rgba_srgb, VK_FORMAT_BC1_RGBA_SRGB_BLOCK, "BC1_RGBA_sRGB", 8, 4, 4, F::compressed, true},
    {pixel_format::bc2_rgba, VK_FORMAT_BC2_UNORM_BLOCK, "BC2_RGBA", 16, 4, 4, F::compressed},
    {pixel_format::bc2_rgba_srgb, VK_FORMAT_BC2_SRGB_BLOCK, "BC2_RGBA_sRGB", 16, 4, 4, F::compressed, true},
    {pixel_format::bc3_rgba, VK_FORMAT_BC3_UNORM_BLOCK, "BC3_RGBA", 16, 4, 4, F::compressed},
    {pixel_format::bc3_rgba_srgb, VK_FORMAT_BC3_SRGB_BLOCK, "BC3_RGBA_sRGB", 16, 4, 4, F::compressed, true},
    {pixel_format::bc4_r_unorm, VK_FORMAT_BC4_UNORM_BLOCK, "BC4_RUnorm", 8, 4, 4, F::compressed},
    {pixel_format::bc4_r_snorm, VK_FORMAT_BC4_SNORM_BLOCK, "BC4_RSnorm", 8, 4, 4, F::compressed},
    {pixel_format::bc5_rg_unorm, VK_FORMAT_BC5_UNORM_BLOCK, "BC5_RGUnorm", 16, 4, 4, F::compressed},
    {pixel_format::bc5_rg_snorm, VK_FORMAT_BC5_SNORM_BLOCK, "BC5_RGSnorm", 16, 4, 4, F::compressed},
    {pixel_format::bc6h_rgb_float, VK_FORMAT_BC6H_SFLOAT_BLOCK, "BC6H_RGBFloat", 16, 4, 4, F::compressed},
    {pixel_format::bc6h_rgb_ufloat, VK_FORMAT_BC6H_UFLOAT_BLOCK, "BC6H_RGBUfloat", 16, 4, 4, F::compressed},
    {pixel_format::bc7_rgba_unorm, VK_FORMAT_BC7_UNORM_BLOCK, "BC7_RGBAUnorm", 16, 4, 4, F::compressed},
    {pixel_format::bc7_rgba_unorm_srgb, VK_FORMAT_BC7_SRGB_BLOCK, "BC7_RGBAUnorm_sRGB", 16, 4, 4, F::compressed, true},
    {pixel_format::depth16_unorm, VK_FORMAT_D16_UNORM, "Depth16Unorm", 2, 1, 1, F::depth},
    {pixel_format::depth32_float, VK_FORMAT_D32_SFLOAT, "Depth32Float", 4, 1, 1, F::depth},
    {pixel_format::stencil8, VK_FORMAT_S8_UINT, "Stencil8", 1, 1, 1, F::stencil},
    // Copies of the depth plane use 4 bytes per texel (Metal's packed
    // 24-bit depth in 32-bit words), the stencil plane 1.
    {pixel_format::depth24_unorm_stencil8, VK_FORMAT_D24_UNORM_S8_UINT, "Depth24Unorm_Stencil8", 4, 1, 1,
     F::depth_stencil},
    {pixel_format::depth32_float_stencil8, VK_FORMAT_D32_SFLOAT_S8_UINT, "Depth32Float_Stencil8", 4, 1, 1,
     F::depth_stencil},
    {pixel_format::x32_stencil8, VK_FORMAT_D32_SFLOAT_S8_UINT, "X32_Stencil8", 1, 1, 1, F::stencil, false, identity,
     true},
    {pixel_format::x24_stencil8, VK_FORMAT_D24_UNORM_S8_UINT, "X24_Stencil8", 1, 1, 1, F::stencil, false, identity,
     true},
};

constexpr std::pair<std::uint32_t, std::uint32_t> srgb_pairs[] = {
    {pixel_format::r8_unorm, pixel_format::r8_unorm_srgb}, {pixel_format::rg8_unorm, pixel_format::rg8_unorm_srgb},
    {pixel_format::rgba8_unorm, pixel_format::rgba8_unorm_srgb},
    {pixel_format::bgra8_unorm, pixel_format::bgra8_unorm_srgb}, {pixel_format::bc1_rgba, pixel_format::bc1_rgba_srgb},
    {pixel_format::bc2_rgba, pixel_format::bc2_rgba_srgb}, {pixel_format::bc3_rgba, pixel_format::bc3_rgba_srgb},
    {pixel_format::bc7_rgba_unorm, pixel_format::bc7_rgba_unorm_srgb}};

constexpr VertexFormatInfo vertex_formats[] = {
    {1, VK_FORMAT_R8G8_UINT, VK_FORMAT_UNDEFINED, 2},          // UChar2
    {2, VK_FORMAT_R8G8B8_UINT, VK_FORMAT_R8G8B8A8_UINT, 3},     // UChar3
    {3, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_UNDEFINED, 4},      // UChar4
    {4, VK_FORMAT_R8G8_SINT, VK_FORMAT_UNDEFINED, 2},          // Char2
    {5, VK_FORMAT_R8G8B8_SINT, VK_FORMAT_R8G8B8A8_SINT, 3},     // Char3
    {6, VK_FORMAT_R8G8B8A8_SINT, VK_FORMAT_UNDEFINED, 4},      // Char4
    {7, VK_FORMAT_R8G8_UNORM, VK_FORMAT_UNDEFINED, 2},         // UChar2Normalized
    {8, VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, 3},   // UChar3Normalized
    {9, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_UNDEFINED, 4},     // UChar4Normalized
    {10, VK_FORMAT_R8G8_SNORM, VK_FORMAT_UNDEFINED, 2},        // Char2Normalized
    {11, VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_R8G8B8A8_SNORM, 3},  // Char3Normalized
    {12, VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_UNDEFINED, 4},    // Char4Normalized
    {13, VK_FORMAT_R16G16_UINT, VK_FORMAT_UNDEFINED, 4},       // UShort2
    {14, VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R16G16B16A16_UINT, 6},
    {15, VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_UNDEFINED, 8},
    {16, VK_FORMAT_R16G16_SINT, VK_FORMAT_UNDEFINED, 4},       // Short2
    {17, VK_FORMAT_R16G16B16_SINT, VK_FORMAT_R16G16B16A16_SINT, 6},
    {18, VK_FORMAT_R16G16B16A16_SINT, VK_FORMAT_UNDEFINED, 8},
    {19, VK_FORMAT_R16G16_UNORM, VK_FORMAT_UNDEFINED, 4},      // UShort2Normalized
    {20, VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16A16_UNORM, 6},
    {21, VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_UNDEFINED, 8},
    {22, VK_FORMAT_R16G16_SNORM, VK_FORMAT_UNDEFINED, 4},      // Short2Normalized
    {23, VK_FORMAT_R16G16B16_SNORM, VK_FORMAT_R16G16B16A16_SNORM, 6},
    {24, VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_UNDEFINED, 8},
    {25, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_UNDEFINED, 4},     // Half2
    {26, VK_FORMAT_R16G16B16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, 6},
    {27, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_UNDEFINED, 8},
    {28, VK_FORMAT_R32_SFLOAT, VK_FORMAT_UNDEFINED, 4},        // Float
    {29, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_UNDEFINED, 8},
    {30, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_UNDEFINED, 12},
    {31, VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_UNDEFINED, 16},
    {32, VK_FORMAT_R32_SINT, VK_FORMAT_UNDEFINED, 4},          // Int
    {33, VK_FORMAT_R32G32_SINT, VK_FORMAT_UNDEFINED, 8},
    {34, VK_FORMAT_R32G32B32_SINT, VK_FORMAT_UNDEFINED, 12},
    {35, VK_FORMAT_R32G32B32A32_SINT, VK_FORMAT_UNDEFINED, 16},
    {36, VK_FORMAT_R32_UINT, VK_FORMAT_UNDEFINED, 4},          // UInt
    {37, VK_FORMAT_R32G32_UINT, VK_FORMAT_UNDEFINED, 8},
    {38, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_UNDEFINED, 12},
    {39, VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_UNDEFINED, 16},
    {40, VK_FORMAT_A2B10G10R10_SNORM_PACK32, VK_FORMAT_UNDEFINED, 4},   // Int1010102Normalized
    {41, VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_UNDEFINED, 4},   // UInt1010102Normalized
    {42, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_UNDEFINED, 4},    // UChar4Normalized_BGRA
    {45, VK_FORMAT_R8_UINT, VK_FORMAT_UNDEFINED, 1},           // UChar
    {46, VK_FORMAT_R8_SINT, VK_FORMAT_UNDEFINED, 1},           // Char
    {47, VK_FORMAT_R8_UNORM, VK_FORMAT_UNDEFINED, 1},          // UCharNormalized
    {48, VK_FORMAT_R8_SNORM, VK_FORMAT_UNDEFINED, 1},          // CharNormalized
    {49, VK_FORMAT_R16_UINT, VK_FORMAT_UNDEFINED, 2},          // UShort
    {50, VK_FORMAT_R16_SINT, VK_FORMAT_UNDEFINED, 2},          // Short
    {51, VK_FORMAT_R16_UNORM, VK_FORMAT_UNDEFINED, 2},         // UShortNormalized
    {52, VK_FORMAT_R16_SNORM, VK_FORMAT_UNDEFINED, 2},         // ShortNormalized
    {53, VK_FORMAT_R16_SFLOAT, VK_FORMAT_UNDEFINED, 2},        // Half
    {54, VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_FORMAT_UNDEFINED, 4},   // FloatRG11B10
    {55, VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, VK_FORMAT_UNDEFINED, 4},    // FloatRGB9E5
};

} // namespace

VkImageAspectFlags PixelFormatInfo::aspects() const noexcept {
    switch (format_class) {
    case FormatClass::depth: return VK_IMAGE_ASPECT_DEPTH_BIT;
    case FormatClass::stencil:
        return stencil_view || vulkan == VK_FORMAT_S8_UINT ? VK_IMAGE_ASPECT_STENCIL_BIT
                                                           : VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    case FormatClass::depth_stencil: return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default: return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

const PixelFormatInfo* pixel_format_info(std::uint32_t metal) noexcept {
    for (const auto& info : pixel_formats)
        if (info.metal == metal) return &info;
    return nullptr;
}

std::uint32_t pixel_format_from_vulkan(VkFormat format) noexcept {
    for (const auto& info : pixel_formats)
        if (info.vulkan == format && !info.stencil_view && info.metal != pixel_format::a8_unorm) return info.metal;
    return 0;
}

std::uint32_t srgb_partner(std::uint32_t metal) noexcept {
    for (const auto& [linear, srgb] : srgb_pairs) {
        if (linear == metal) return srgb;
        if (srgb == metal) return linear;
    }
    return 0;
}

const VertexFormatInfo* vertex_format_info(std::uint32_t metal) noexcept {
    for (const auto& info : vertex_formats)
        if (info.metal == metal) return &info;
    return nullptr;
}

VkFormat vertex_format_with_signedness(VkFormat format, bool shader_signed) noexcept {
    // {unsigned, signed} pairs of every integer vertex format.
    constexpr std::pair<VkFormat, VkFormat> pairs[] = {
        {VK_FORMAT_R8_UINT, VK_FORMAT_R8_SINT}, {VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8_SINT},
        {VK_FORMAT_R8G8B8_UINT, VK_FORMAT_R8G8B8_SINT}, {VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT},
        {VK_FORMAT_R16_UINT, VK_FORMAT_R16_SINT}, {VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_SINT},
        {VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R16G16B16_SINT}, {VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_SINT},
        {VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT}, {VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_SINT},
        {VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32_SINT}, {VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_SINT}};
    for (const auto& [unsigned_format, signed_format] : pairs) {
        if (format == unsigned_format && shader_signed) return signed_format;
        if (format == signed_format && !shader_signed) return unsigned_format;
    }
    return format;
}

} // namespace alchemy::metal
