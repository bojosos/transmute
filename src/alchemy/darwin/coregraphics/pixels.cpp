#include "alchemy/darwin/coregraphics/q2d_internal.hpp"

#include <algorithm>
#include <cstring>

// Pixel formats: bitmap-context layouts <-> premultiplied BGRA (Direct2D),
// and decoding of every CGImage sample layout to premultiplied BGRA.
// Layout semantics follow the public CGImage / CGBitmapContext documentation.
namespace alchemy::gui::quartz2d {
namespace {

std::uint16_t swap_bytes(std::uint16_t value) { return static_cast<std::uint16_t>((value >> 8) | (value << 8)); }
std::uint32_t swap_bytes(std::uint32_t value) {
    return (value >> 24) | ((value >> 8) & 0xff00U) | ((value << 8) & 0xff0000U) | (value << 24);
}
std::uint8_t to_byte(double value) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
}
std::uint8_t premultiply(std::uint8_t value, std::uint8_t alpha) {
    return static_cast<std::uint8_t>((value * alpha + 127) / 255);
}
std::uint8_t unpremultiply(std::uint8_t value, std::uint8_t alpha) {
    if (alpha == 0) return 0;
    return static_cast<std::uint8_t>(std::min(255, (value * 255 + alpha / 2) / alpha));
}

// Byte offsets of R, G, B, A (A = -1 when absent or skipped) inside one
// 32-bit RGB pixel of an 8-bit-per-component layout.
struct Rgb32 { int r, g, b, a, skip; };
Rgb32 rgb32_offsets(const PixelLayout& layout) {
    const auto alpha = layout.alpha();
    const bool first = alpha == alpha_info::premultiplied_first || alpha == alpha_info::first || alpha == alpha_info::none_skip_first;
    // Memory order for the default / big-endian byte order: ARGB (first) or
    // RGBA (last); 32-bit little-endian words reverse it (BGRA / ABGR).
    int r = first ? 1 : 0, g = first ? 2 : 1, b = first ? 3 : 2, a = first ? 0 : 3;
    if (layout.byte_order() == bitmap_info::order_32_little) {
        r = 3 - r; g = 3 - g; b = 3 - b; a = 3 - a;
    }
    const bool has = layout.has_alpha();
    return {r, g, b, has ? a : -1, has ? -1 : a};
}

} // namespace

std::optional<std::string> check_context_layout(const PixelLayout& layout) {
    if (layout.width == 0 || layout.height == 0) return "zero width or height";
    if (layout.width > 32768 || layout.height > 32768) return "dimensions exceed 32768";
    if (layout.bitmap_info & bitmap_info::float_components) return "floating-point components";
    if (layout.bits_per_component != 8) return std::to_string(layout.bits_per_component) + " bits per component";
    const auto alpha = layout.alpha();
    const auto order = layout.byte_order();
    if (order != bitmap_info::order_default && order != bitmap_info::order_32_big && order != bitmap_info::order_32_little)
        return "16-bit byte order with 8-bit components";
    std::uint64_t minimum_row{};
    switch (layout.model) {
    case ColorModel::rgb:
        if (layout.bits_per_pixel != 32) return std::to_string(layout.bits_per_pixel) + " bits per pixel for RGB";
        if (alpha == alpha_info::none || ((alpha == alpha_info::last || alpha == alpha_info::first) && !layout.straight_alpha) ||
            alpha == alpha_info::only)
            return "alpha info " + std::to_string(alpha) + " for RGB";
        minimum_row = static_cast<std::uint64_t>(layout.width) * 4;
        break;
    case ColorModel::monochrome:
        if (layout.bits_per_pixel != 8 || alpha != alpha_info::none) return "gray layout other than 8 bpp without alpha";
        minimum_row = layout.width;
        break;
    case ColorModel::unknown:   // alpha only (no color space)
        if (layout.bits_per_pixel != 8 || alpha != alpha_info::only) return "alpha-only layout other than 8 bpp kCGImageAlphaOnly";
        minimum_row = layout.width;
        break;
    default:
        return "color space model " + std::to_string(static_cast<int>(layout.model));
    }
    if (layout.bytes_per_row < minimum_row) return "bytesPerRow " + std::to_string(layout.bytes_per_row) + " is too small";
    return std::nullopt;
}

void row_to_pbgra(const PixelLayout& layout, const std::uint8_t* row, std::uint32_t count, std::uint8_t* out) {
    switch (layout.model) {
    case ColorModel::monochrome:
        for (std::uint32_t x = 0; x < count; ++x) {
            out[x * 4] = out[x * 4 + 1] = out[x * 4 + 2] = row[x];
            out[x * 4 + 3] = 255;
        }
        return;
    case ColorModel::unknown:
        for (std::uint32_t x = 0; x < count; ++x) {
            out[x * 4] = out[x * 4 + 1] = out[x * 4 + 2] = 0;
            out[x * 4 + 3] = row[x];
        }
        return;
    default: break;
    }
    const auto offsets = rgb32_offsets(layout);
    const bool straight = layout.straight_alpha && offsets.a >= 0;
    for (std::uint32_t x = 0; x < count; ++x) {
        const auto* pixel = row + x * 4;
        auto* target = out + x * 4;
        const std::uint8_t alpha = offsets.a >= 0 ? pixel[offsets.a] : 255;
        std::uint8_t r = pixel[offsets.r], g = pixel[offsets.g], b = pixel[offsets.b];
        if (straight) { r = premultiply(r, alpha); g = premultiply(g, alpha); b = premultiply(b, alpha); }
        else if (offsets.a >= 0) { r = std::min(r, alpha); g = std::min(g, alpha); b = std::min(b, alpha); }
        target[0] = b; target[1] = g; target[2] = r; target[3] = alpha;
    }
}

void pbgra_to_row(const PixelLayout& layout, const std::uint8_t* in, std::uint32_t count, std::uint8_t* row) {
    switch (layout.model) {
    case ColorModel::monochrome:
        // Gray contexts have no alpha: premultiplied values over black.
        for (std::uint32_t x = 0; x < count; ++x) {
            const auto* p = in + x * 4;
            row[x] = static_cast<std::uint8_t>((p[2] * 77 + p[1] * 150 + p[0] * 29 + 128) >> 8);
        }
        return;
    case ColorModel::unknown:
        for (std::uint32_t x = 0; x < count; ++x) row[x] = in[x * 4 + 3];
        return;
    default: break;
    }
    const auto offsets = rgb32_offsets(layout);
    const bool straight = layout.straight_alpha && offsets.a >= 0;
    for (std::uint32_t x = 0; x < count; ++x) {
        const auto* p = in + x * 4;
        auto* pixel = row + x * 4;
        std::uint8_t r = p[2], g = p[1], b = p[0];
        const std::uint8_t alpha = p[3];
        if (straight) { r = unpremultiply(r, alpha); g = unpremultiply(g, alpha); b = unpremultiply(b, alpha); }
        pixel[offsets.r] = r; pixel[offsets.g] = g; pixel[offsets.b] = b;
        if (offsets.a >= 0) pixel[offsets.a] = alpha;
        if (offsets.skip >= 0) pixel[offsets.skip] = 0xff;
    }
}

// ---- generic image decoding ----------------------------------------------------------------
namespace {
struct SampleReader {
    const PixelLayout& layout;
    std::span<const std::uint8_t> bytes;
    bool little16{}, little32{};
    // Component `index` of pixel x in row y, normalized to 0..1.
    double component(std::uint32_t x, std::uint32_t y, unsigned index) const {
        const auto bpc = layout.bits_per_component;
        const auto bit = static_cast<std::uint64_t>(x) * layout.bits_per_pixel + static_cast<std::uint64_t>(index) * bpc;
        const auto base = static_cast<std::uint64_t>(y) * layout.bytes_per_row;
        if (bpc == 32 && (layout.bitmap_info & bitmap_info::float_components)) {
            const auto at = base + bit / 8;
            if (at + 4 > bytes.size()) return 0;
            std::uint32_t raw{};
            std::memcpy(&raw, bytes.data() + at, 4);
            if (!little32) raw = swap_bytes(raw);
            return static_cast<double>(std::bit_cast<float>(raw));
        }
        if (bpc == 16) {
            const auto at = base + bit / 8;
            if (at + 2 > bytes.size()) return 0;
            std::uint16_t raw = static_cast<std::uint16_t>(bytes[at] << 8 | bytes[at + 1]);
            if (little16) raw = swap_bytes(raw);
            if (layout.bitmap_info & bitmap_info::float_components) {
                // IEEE half
                const auto sign = (raw >> 15) & 1, exponent = (raw >> 10) & 0x1f, mantissa = raw & 0x3ff;
                double value = exponent == 0 ? std::ldexp(mantissa, -24)
                                             : exponent == 31 ? 1.0 : std::ldexp(1024 + mantissa, static_cast<int>(exponent) - 25);
                return sign ? -value : value;
            }
            return raw / 65535.0;
        }
        if (bpc == 8) {
            const auto at = base + bit / 8;
            return at < bytes.size() ? bytes[at] / 255.0 : 0;
        }
        if (bpc == 32) {
            const auto at = base + bit / 8;
            if (at + 4 > bytes.size()) return 0;
            std::uint32_t raw{};
            std::memcpy(&raw, bytes.data() + at, 4);
            if (!little32) raw = swap_bytes(raw);
            return raw / 4294967295.0;
        }
        // 1, 2, 4 bits: most significant bits first.
        const auto at = base + bit / 8;
        if (at >= bytes.size()) return 0;
        const auto shift = 8 - bpc - static_cast<unsigned>(bit % 8);
        const auto value = (bytes[at] >> shift) & ((1U << bpc) - 1);
        return value / static_cast<double>((1U << bpc) - 1);
    }
    std::uint32_t raw_index(std::uint32_t x, std::uint32_t y) const {
        const auto bpc = layout.bits_per_component;
        const auto bit = static_cast<std::uint64_t>(x) * layout.bits_per_pixel;
        const auto at = static_cast<std::uint64_t>(y) * layout.bytes_per_row + bit / 8;
        if (at >= bytes.size()) return 0;
        if (bpc == 8) return bytes[at];
        if (bpc == 16 && at + 1 < bytes.size()) return little16 ? (bytes[at] | bytes[at + 1] << 8) : (bytes[at] << 8 | bytes[at + 1]);
        const auto shift = 8 - bpc - static_cast<unsigned>(bit % 8);
        return (bytes[at] >> shift) & ((1U << bpc) - 1);
    }
};
} // namespace

Pbgra decode_samples(const ImageDesc& desc, std::span<const std::uint8_t> bytes) {
    const auto& layout = desc.layout;
    Pbgra out;
    out.width = layout.width;
    out.height = layout.height;
    out.pixels.assign(static_cast<std::size_t>(layout.width) * layout.height * 4, 0);
    const auto bpc = layout.bits_per_component;
    if (bpc != 1 && bpc != 2 && bpc != 4 && bpc != 8 && bpc != 16 && bpc != 32)
        unsupported("CGImage with " + std::to_string(bpc) + " bits per component");
    if (layout.bytes_per_row * (layout.height ? layout.height - 1 : 0) + (static_cast<std::uint64_t>(layout.width) * layout.bits_per_pixel + 7) / 8 >
        bytes.size())
        fail("CGImage data provider holds fewer bytes than the image needs");
    const auto order = layout.byte_order();
    SampleReader reader{layout, bytes, order == bitmap_info::order_16_little,
                        order == bitmap_info::order_32_little ||
                            (order == bitmap_info::order_default && (layout.bitmap_info & bitmap_info::float_components) && bpc == 32)};
    // Default byte order for float images is host order on macOS (little); integers are big.
    if (order == bitmap_info::order_default && bpc == 16 && (layout.bitmap_info & bitmap_info::float_components)) reader.little16 = true;

    // Fast path: 8-bit RGB(A/X) 32 bpp and 24 bpp.
    const auto alpha = layout.alpha();
    const auto decode_value = [&](unsigned index, double value) {
        if (desc.decode.size() >= 2 * (index + 1)) {
            const auto low = desc.decode[2 * index], high = desc.decode[2 * index + 1];
            return low + value * (high - low);
        }
        return value;
    };
    if (desc.mask) {
        for (std::uint32_t y = 0; y < layout.height; ++y)
            for (std::uint32_t x = 0; x < layout.width; ++x) {
                const auto sample = decode_value(0, reader.component(x, y, 0));
                // Image masks paint where the sample is 0.
                const auto a = to_byte(1.0 - sample);
                auto* p = out.row(y) + x * 4;
                p[0] = p[1] = p[2] = 0;
                p[3] = a;
            }
        return out;
    }
    const unsigned color = layout.model == ColorModel::indexed ? 1 : layout.color_components;
    const bool has_alpha = layout.has_alpha();
    const bool skip = alpha == alpha_info::none_skip_first || alpha == alpha_info::none_skip_last;
    const bool first = alpha == alpha_info::premultiplied_first || alpha == alpha_info::first || alpha == alpha_info::none_skip_first;
    const bool premultiplied = alpha == alpha_info::premultiplied_first || alpha == alpha_info::premultiplied_last;
    const unsigned total = color + ((has_alpha || skip) ? 1U : 0U);
    if (alpha == alpha_info::only) {
        for (std::uint32_t y = 0; y < layout.height; ++y)
            for (std::uint32_t x = 0; x < layout.width; ++x) out.row(y)[x * 4 + 3] = to_byte(reader.component(x, y, 0));
        return out;
    }
    if (static_cast<std::uint64_t>(total) * bpc > layout.bits_per_pixel)
        fail("CGImage bits per pixel " + std::to_string(layout.bits_per_pixel) + " is too small for its components");
    // For 16/32-bit little-endian words holding a whole pixel, the component
    // order within the word reverses (kCGBitmapByteOrder32Little with 8 bpc).
    const bool reversed = (bpc == 8 && order == bitmap_info::order_32_little && layout.bits_per_pixel == 32) ||
                          (bpc == 8 && order == bitmap_info::order_16_little && layout.bits_per_pixel == 16) ||
                          (bpc < 8 && bpc * total <= 16 && order == bitmap_info::order_16_little);
    const auto slot = [&](unsigned logical) {   // logical: 0..total-1 in ARGB/RGBA declaration order
        const auto physical_count = layout.bits_per_pixel / bpc;
        return reversed ? physical_count - 1 - logical : logical;
    };
    for (std::uint32_t y = 0; y < layout.height; ++y) {
        auto* row = out.row(y);
        for (std::uint32_t x = 0; x < layout.width; ++x) {
            double a = 1;
            unsigned color_start = 0;
            if (has_alpha || skip) {
                if (first) { if (has_alpha) a = reader.component(x, y, slot(0)); color_start = 1; }
                else if (has_alpha) a = reader.component(x, y, slot(color));
            }
            if (has_alpha) a = std::clamp(decode_value(color, a), 0.0, 1.0);
            std::array<double, 4> c{};
            double r{}, g{}, b{};
            if (layout.model == ColorModel::indexed) {
                const auto index = reader.raw_index(x, y);
                const auto entry = static_cast<std::size_t>(index) * 3;
                if (entry + 2 < desc.palette.size()) { r = desc.palette[entry] / 255.0; g = desc.palette[entry + 1] / 255.0; b = desc.palette[entry + 2] / 255.0; }
            } else {
                for (unsigned index = 0; index < color && index < 4; ++index)
                    c[index] = decode_value(index, reader.component(x, y, slot(color_start + index)));
                switch (layout.model) {
                case ColorModel::monochrome: r = g = b = c[0]; break;
                case ColorModel::cmyk:
                    r = (1 - c[0]) * (1 - c[3]); g = (1 - c[1]) * (1 - c[3]); b = (1 - c[2]) * (1 - c[3]);
                    break;
                default: r = c[0]; g = c[1]; b = c[2]; break;
                }
            }
            if (premultiplied && a > 0) { r /= a; g /= a; b /= a; }
            else if (premultiplied) { r = g = b = 0; }
            auto* p = row + x * 4;
            const auto alpha_byte = to_byte(a);
            p[0] = premultiply(to_byte(b), alpha_byte);
            p[1] = premultiply(to_byte(g), alpha_byte);
            p[2] = premultiply(to_byte(r), alpha_byte);
            p[3] = alpha_byte;
        }
    }
    return out;
}

} // namespace alchemy::gui::quartz2d
