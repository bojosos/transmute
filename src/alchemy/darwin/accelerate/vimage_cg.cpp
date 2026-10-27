#include "alchemy/darwin/accelerate/install.hpp"
#include "alchemy/gui/fill_provider.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// vImage's CoreGraphics interoperation (vImage_Utilities.h), on vImage_CGImageFormat
// {bitsPerComponent, bitsPerPixel, colorSpace, bitmapInfo, version, decode, renderingIntent}:
// - vImageBuffer_InitWithCGImage: a buffer (malloc'd through the guest unless kvImageNoAllocate) of
//   the image in the format asked for (a format whose bitsPerComponent is 0 is first filled in from
//   the image); vImageCreateCGImageFromBuffer: a CGImage of a buffer's pixels (copied).
// - vImageConverter_CreateWithCGImageFormat and vImageConvert_AnyToAny between two such formats
//   (one buffer each side); vImageConverter_Retain/Release, the buffer counts,
//   MustOperateOutOfPlace (never: sources are read whole first); vImageCGImageFormat_IsEqual;
//   vImageBuffer_GetSize.
// Pixel formats: gray or RGB colour spaces (a NULL colour space is RGB); 8- and 16-bit integer or
// 32-bit float components; alpha none, first or last, premultiplied or not, or skipped; the default
// (big-endian), 16Little and 32Little byte orders. Pixels pass through straight-alpha RGBA in
// double precision: gray becomes R = G = B, RGB becomes gray by Rec. 709 luma on the encoded values
// (no colour management), and alpha is flattened over the background colour (black without one)
// when the destination has none. Other formats (5-bit components, CMYK, indexed) answer
// kvImageInvalidImageFormat.
namespace alchemy::gui::fill {
namespace {

constexpr std::int64_t no_error = 0, memory_allocation_error = -21771, null_pointer = -21772, invalid_parameter = -21773,
    buffer_size_mismatch = -21774, invalid_image_format = -21778, invalid_image_object = -21781;
constexpr std::uint32_t no_allocate = 512;
// CGImageAlphaInfo and the CGBitmapInfo masks
enum : std::uint32_t { alpha_none, alpha_premultiplied_last, alpha_premultiplied_first, alpha_last, alpha_first, alpha_none_skip_last,
                       alpha_none_skip_first, alpha_only };
constexpr std::uint32_t alpha_mask = 0x1f, float_components = 0x100, byte_order_mask = 0x7000, order_16_little = 0x1000,
    order_32_little = 0x2000;

// A vImage_CGImageFormat decoded.
struct Layout {
    std::uint32_t bpc{}, bpp{}, alpha{}, order{};
    bool floating{};
    unsigned colors{};                 // 1 (gray) or 3 (RGB)
    Address color_space{};
    [[nodiscard]] unsigned channels() const { return colors + (alpha == alpha_none ? 0 : 1); }
    [[nodiscard]] bool has_alpha() const { return alpha != alpha_none && alpha != alpha_none_skip_first && alpha != alpha_none_skip_last; }
    [[nodiscard]] bool first_alpha() const { return alpha == alpha_premultiplied_first || alpha == ::alchemy::gui::fill::alpha_first || alpha == alpha_none_skip_first; }
    [[nodiscard]] bool premultiplied() const { return alpha == alpha_premultiplied_first || alpha == alpha_premultiplied_last; }
    [[nodiscard]] unsigned pixel_bytes() const { return bpp / 8; }
};

struct Buffer { Address data{}; std::uint64_t height{}, width{}, row_bytes{}; };
Buffer read_buffer(GuestMemory& m, Address at) { return {m.read64(at), m.read64(at + 8), m.read64(at + 16), m.read64(at + 24)}; }

// Straight-alpha RGBA pixels, row-major.
using Pixels = std::vector<std::array<double, 4>>;

class VImageCG final : public Provider {
public:
    using Provider::Provider;
    void install();

private:
    void kernel(const std::string& name, transmute::GuestCallHandler handler, const std::source_location& where = std::source_location::current()) {
        if (has(accelerate, name)) return;
        raw(accelerate, name, std::move(handler), where);
    }
    // The layout of a vImage_CGImageFormat in guest memory, or nullopt when unsupported.
    std::optional<Layout> layout(GuestMemory& m, Address format);
    std::optional<Layout> layout(std::uint32_t bpc, std::uint32_t bpp, Address space, std::uint32_t info);
    Pixels decode(GuestMemory& m, const Layout& l, Address data, std::uint64_t row_bytes, std::uint64_t width, std::uint64_t height);
    void encode(GuestMemory& m, const Layout& l, const Pixels& pixels, Address data, std::uint64_t row_bytes, std::uint64_t width, std::uint64_t height,
                const std::array<double, 4>& background);
    std::array<double, 4> background(GuestMemory& m, Address colors, const Layout& l);

    struct Converter { Layout from, to; std::array<double, 4> background{}; std::uint32_t references{1}; };
    std::mutex mutex_;
    std::map<Address, Converter> converters_;
};

std::optional<Layout> VImageCG::layout(std::uint32_t bpc, std::uint32_t bpp, Address space, std::uint32_t info) {
    Layout l{bpc, bpp, info & alpha_mask, info & byte_order_mask, (info & float_components) != 0, 3, space};
    if (space) {
        const auto components = call(core_graphics, "_CGColorSpaceGetNumberOfComponents", {space});
        if (components != 1 && components != 3) return std::nullopt;
        l.colors = static_cast<unsigned>(components);
    }
    if (l.alpha == alpha_only || l.alpha > alpha_only) return std::nullopt;
    const bool component_ok = (bpc == 8 && !l.floating) || (bpc == 16 && !l.floating) || (bpc == 32 && l.floating);
    if (!component_ok || bpp != bpc * l.channels()) return std::nullopt;
    return l;
}

std::optional<Layout> VImageCG::layout(GuestMemory& m, Address format) {
    if (!format) return std::nullopt;
    return layout(m.read32(format), m.read32(format + 4), m.read64(format + 8), m.read32(format + 16));
}

// One component at byte `at` of a pixel: normalized to [0, 1] for integers.
double read_component(GuestMemory& m, const Layout& l, Address at) {
    const bool little = (l.bpc == 16 && l.order == order_16_little) || (l.bpc == 32 && l.order == order_32_little);
    if (l.bpc == 8) return m.read8(at) / 255.0;
    if (l.bpc == 16) {
        const unsigned a = m.read8(at), b = m.read8(at + 1);
        return (little ? (a | (b << 8)) : ((a << 8) | b)) / 65535.0;
    }
    std::uint32_t bits = 0;
    for (unsigned i = 0; i < 4; ++i) bits |= static_cast<std::uint32_t>(m.read8(at + i)) << (little ? 8 * i : 8 * (3 - i));
    return std::bit_cast<float>(bits);
}
void write_component(GuestMemory& m, const Layout& l, Address at, double value) {
    const bool little = (l.bpc == 16 && l.order == order_16_little) || (l.bpc == 32 && l.order == order_32_little);
    if (l.bpc == 8) return m.write8(at, static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0, 1.0) * 255)));
    if (l.bpc == 16) {
        const auto v = static_cast<unsigned>(std::lround(std::clamp(value, 0.0, 1.0) * 65535));
        m.write8(at, static_cast<std::uint8_t>(little ? v : v >> 8));
        m.write8(at + 1, static_cast<std::uint8_t>(little ? v >> 8 : v));
        return;
    }
    const auto bits = std::bit_cast<std::uint32_t>(static_cast<float>(value));
    for (unsigned i = 0; i < 4; ++i) m.write8(at + i, static_cast<std::uint8_t>(bits >> (little ? 8 * i : 8 * (3 - i))));
}
// The byte offset of channel slot `c` (in the format's order) within a pixel: 8-bit components of a
// 32Little pixel are stored reversed.
unsigned slot_offset(const Layout& l, unsigned c) {
    const unsigned size = l.bpc / 8;
    if (l.bpc == 8 && l.order == order_32_little) return (l.channels() - 1 - c) * size;
    return c * size;
}

Pixels VImageCG::decode(GuestMemory& m, const Layout& l, Address data, std::uint64_t row_bytes, std::uint64_t width, std::uint64_t height) {
    Pixels out(static_cast<std::size_t>(width * height));
    const unsigned first_color = l.first_alpha() ? 1 : 0;
    const unsigned alpha_slot = l.first_alpha() ? 0 : l.colors;
    for (std::uint64_t y = 0; y < height; ++y)
        for (std::uint64_t x = 0; x < width; ++x) {
            const auto pixel = data + y * row_bytes + x * l.pixel_bytes();
            std::array<double, 4> v{};
            for (unsigned c = 0; c < l.colors; ++c) v[c] = read_component(m, l, pixel + slot_offset(l, first_color + c));
            if (l.colors == 1) v[1] = v[2] = v[0];
            v[3] = l.has_alpha() ? read_component(m, l, pixel + slot_offset(l, alpha_slot)) : 1.0;
            if (l.premultiplied() && v[3] > 0)
                for (unsigned c = 0; c < 3; ++c) v[c] /= v[3];
            out[static_cast<std::size_t>(y * width + x)] = v;
        }
    return out;
}

void VImageCG::encode(GuestMemory& m, const Layout& l, const Pixels& pixels, Address data, std::uint64_t row_bytes, std::uint64_t width,
                      std::uint64_t height, const std::array<double, 4>& background) {
    const unsigned first_color = l.first_alpha() ? 1 : 0;
    const unsigned alpha_slot = l.first_alpha() ? 0 : l.colors;
    for (std::uint64_t y = 0; y < height; ++y)
        for (std::uint64_t x = 0; x < width; ++x) {
            auto v = pixels[static_cast<std::size_t>(y * width + x)];
            if (!l.has_alpha()) {
                for (unsigned c = 0; c < 3; ++c) v[c] = v[c] * v[3] + background[c] * (1 - v[3]);
                v[3] = 1;
            }
            std::array<double, 3> color{v[0], v[1], v[2]};
            if (l.colors == 1) color[0] = 0.2126 * v[0] + 0.7152 * v[1] + 0.0722 * v[2];
            const auto pixel = data + y * row_bytes + x * l.pixel_bytes();
            for (unsigned c = 0; c < l.colors; ++c)
                write_component(m, l, pixel + slot_offset(l, first_color + c), l.premultiplied() ? color[c] * v[3] : color[c]);
            if (l.alpha != alpha_none) write_component(m, l, pixel + slot_offset(l, alpha_slot), l.has_alpha() ? v[3] : 1.0);
        }
}

// The background colour (const CGFloat *, in the destination's colour components), RGB.
std::array<double, 4> VImageCG::background(GuestMemory& m, Address colors, const Layout& l) {
    std::array<double, 4> out{0, 0, 0, 1};
    if (!colors) return out;
    for (unsigned c = 0; c < l.colors; ++c) out[c] = std::bit_cast<double>(m.read64(colors + 8ULL * c));
    if (l.colors == 1) out[1] = out[2] = out[0];
    return out;
}

void result(CpuState& s, std::int64_t value) { s.x[0] = static_cast<std::uint64_t>(value); }

void VImageCG::install() {
    // vImageBuffer_InitWithCGImage(vImage_Buffer *buf, vImage_CGImageFormat *format,
    //     const CGFloat *backgroundColor, CGImageRef image, vImage_Flags flags)
    kernel("_vImageBuffer_InitWithCGImage", [this](CpuState& s, GuestMemory& m) {
        const auto out = s.x[0], format = s.x[1], image = s.x[3];
        const auto flags = static_cast<std::uint32_t>(s.x[4]);
        if (!out || !format) return result(s, null_pointer);
        if (!image) return result(s, invalid_image_object);
        const auto width = call(core_graphics, "_CGImageGetWidth", {image}), height = call(core_graphics, "_CGImageGetHeight", {image});
        const auto image_layout = layout(static_cast<std::uint32_t>(call(core_graphics, "_CGImageGetBitsPerComponent", {image})),
                                         static_cast<std::uint32_t>(call(core_graphics, "_CGImageGetBitsPerPixel", {image})),
                                         call(core_graphics, "_CGImageGetColorSpace", {image}),
                                         static_cast<std::uint32_t>(call(core_graphics, "_CGImageGetBitmapInfo", {image})));
        if (!image_layout) return result(s, invalid_image_format);
        if (m.read32(format) == 0) {   // an empty format: the image's own (its colour space retained for the caller)
            m.write32(format, image_layout->bpc);
            m.write32(format + 4, image_layout->bpp);
            m.write64(format + 8, image_layout->color_space ? call(core_graphics, "_CGColorSpaceRetain", {image_layout->color_space}) : 0);
            m.write32(format + 16, static_cast<std::uint32_t>(call(core_graphics, "_CGImageGetBitmapInfo", {image})));
        }
        const auto target = layout(m, format);
        if (!target) return result(s, invalid_image_format);
        Buffer b{};
        if (flags & no_allocate) {
            b = read_buffer(m, out);
            if (!b.data || b.width != width || b.height != height || b.row_bytes < width * target->pixel_bytes()) return result(s, buffer_size_mismatch);
        } else {
            b = {0, height, width, ((width * target->pixel_bytes()) + 15) & ~std::uint64_t{15}};
            b.data = call(libsystem, "_malloc", {std::max<std::uint64_t>(b.row_bytes * height, 16)});
            if (!b.data) return result(s, memory_allocation_error);
            m.write64(out, b.data);
            m.write64(out + 8, height);
            m.write64(out + 16, width);
            m.write64(out + 24, b.row_bytes);
        }
        const auto provider = call(core_graphics, "_CGImageGetDataProvider", {image});
        const auto bytes = provider ? call(core_graphics, "_CGDataProviderCopyData", {provider}) : 0;
        if (!bytes) return result(s, invalid_image_object);
        const auto source = call(core_foundation, "_CFDataGetBytePtr", {bytes});
        const auto row_bytes = call(core_graphics, "_CGImageGetBytesPerRow", {image});
        const auto pixels = decode(m, *image_layout, source, row_bytes, width, height);
        static_cast<void>(call(core_foundation, "_CFRelease", {bytes}));
        encode(m, *target, pixels, b.data, b.row_bytes, width, height, background(m, s.x[2], *target));
        result(s, no_error);
    });
    // CGImageRef vImageCreateCGImageFromBuffer(const vImage_Buffer *buf, const vImage_CGImageFormat *format,
    //     callback, void *userData, vImage_Flags flags, vImage_Error *error): the pixels copied.
    kernel("_vImageCreateCGImageFromBuffer", [this](CpuState& s, GuestMemory& m) {
        const auto error_out = s.x[5];
        const auto fail = [&](std::int64_t code) {
            if (error_out) m.write64(error_out, static_cast<std::uint64_t>(code));
            s.x[0] = 0;
        };
        if (!s.x[0] || !s.x[1]) return fail(null_pointer);
        const auto b = read_buffer(m, s.x[0]);
        const auto l = layout(m, s.x[1]);
        if (!l) return fail(invalid_image_format);
        if (!b.data || !b.width || !b.height || b.row_bytes < b.width * l->pixel_bytes()) return fail(invalid_parameter);
        const auto row_bytes = b.width * l->pixel_bytes();
        const auto size = row_bytes * b.height;
        const auto copy = call(libsystem, "_malloc", {std::max<std::uint64_t>(size, 16)});
        if (!copy) return fail(memory_allocation_error);
        std::vector<std::uint8_t> row(static_cast<std::size_t>(row_bytes));
        for (std::uint64_t y = 0; y < b.height; ++y) {
            m.read_bytes(b.data + y * b.row_bytes, row);
            m.write_bytes(copy + y * row_bytes, row);
        }
        // The provider frees the copy: releaseData(info, data, size) with info = data calls free(data).
        const auto provider = call(core_graphics, "_CGDataProviderCreateWithData", {copy, copy, size, symbol(libsystem, "_free")});
        const auto space = l->color_space ? l->color_space : call(core_graphics, "_CGColorSpaceCreateDeviceRGB", {});
        CallRegs r{};
        r.x = {b.width, b.height, l->bpc, l->bpp, row_bytes, space, m.read32(s.x[1] + 16), provider};
        r.push_stack(m.read64(s.x[1] + 24));      // decode
        r.push_stack(std::uint8_t{1});            // shouldInterpolate
        r.push_stack(m.read32(s.x[1] + 32));      // renderingIntent
        const auto image = call_regs(core_graphics, "_CGImageCreate", r).x0;
        static_cast<void>(call(core_graphics, "_CGDataProviderRelease", {provider}));
        if (!l->color_space) static_cast<void>(call(core_graphics, "_CGColorSpaceRelease", {space}));
        if (!image) return fail(invalid_image_format);
        if (error_out) m.write64(error_out, no_error);
        s.x[0] = image;
    });
    // vImageConverterRef vImageConverter_CreateWithCGImageFormat(const vImage_CGImageFormat *src,
    //     const vImage_CGImageFormat *dest, const CGFloat *backgroundColor, vImage_Flags flags, vImage_Error *error)
    kernel("_vImageConverter_CreateWithCGImageFormat", [this](CpuState& s, GuestMemory& m) {
        const auto error_out = s.x[4];
        const auto from = layout(m, s.x[0]), to = layout(m, s.x[1]);
        if (!s.x[0] || !s.x[1] || !from || !to) {
            if (error_out) m.write64(error_out, static_cast<std::uint64_t>(!s.x[0] || !s.x[1] ? null_pointer : invalid_image_format));
            s.x[0] = 0;
            return;
        }
        const auto handle = process.allocate_guest(16, 16);
        {
            std::lock_guard lock(mutex_);
            converters_[handle] = Converter{*from, *to, background(m, s.x[2], *to)};
        }
        if (error_out) m.write64(error_out, no_error);
        s.x[0] = handle;
    });
    kernel("_vImageConverter_Retain", [this](CpuState& s, GuestMemory&) {
        std::lock_guard lock(mutex_);
        if (const auto found = converters_.find(s.x[0]); found != converters_.end()) ++found->second.references;
    });
    kernel("_vImageConverter_Release", [this](CpuState& s, GuestMemory&) {
        bool gone = false;
        {
            std::lock_guard lock(mutex_);
            const auto found = converters_.find(s.x[0]);
            if (found == converters_.end()) return;
            if (--found->second.references == 0) { converters_.erase(found); gone = true; }
        }
        if (gone) process.release_guest(s.x[0]);
    });
    for (const auto* name : {"_vImageConverter_GetNumberOfSourceBuffers", "_vImageConverter_GetNumberOfDestinationBuffers"})
        kernel(name, [this](CpuState& s, GuestMemory&) {
            std::lock_guard lock(mutex_);
            s.x[0] = converters_.contains(s.x[0]) ? 1 : 0;
        });
    // vImageConverter_MustOperateOutOfPlace(converter, srcs, dests, flags): sources are read whole first.
    kernel("_vImageConverter_MustOperateOutOfPlace", [](CpuState& s, GuestMemory&) { result(s, no_error); });
    // vImageConvert_AnyToAny(converter, const vImage_Buffer *srcs, const vImage_Buffer *dests, void *tempBuffer, flags)
    kernel("_vImageConvert_AnyToAny", [this](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[1] || !s.x[2]) return result(s, null_pointer);
        Converter c;
        {
            std::lock_guard lock(mutex_);
            const auto found = converters_.find(s.x[0]);
            if (found == converters_.end()) return result(s, invalid_parameter);
            c = found->second;
        }
        const auto src = read_buffer(m, s.x[1]), dest = read_buffer(m, s.x[2]);
        if (!src.data || !dest.data || src.width < dest.width || src.height < dest.height ||
            src.row_bytes < src.width * c.from.pixel_bytes() || dest.row_bytes < dest.width * c.to.pixel_bytes())
            return result(s, invalid_parameter);
        const auto pixels = decode(m, c.from, src.data, src.row_bytes, dest.width, dest.height);
        encode(m, c.to, pixels, dest.data, dest.row_bytes, dest.width, dest.height, c.background);
        result(s, no_error);
    });
    // Boolean vImageCGImageFormat_IsEqual(const vImage_CGImageFormat *f1, const vImage_CGImageFormat *f2)
    kernel("_vImageCGImageFormat_IsEqual", [this](CpuState& s, GuestMemory& m) {
        const auto a = s.x[0], b = s.x[1];
        if (!a || !b) { s.x[0] = a == b; return; }
        bool same = m.read32(a) == m.read32(b) && m.read32(a + 4) == m.read32(b + 4) && m.read32(a + 16) == m.read32(b + 16) &&
                    m.read32(a + 32) == m.read32(b + 32);
        const auto sa = m.read64(a + 8), sb = m.read64(b + 8);
        if (same && sa != sb) same = sa && sb && call(core_foundation, "_CFEqual", {sa, sb}) != 0;
        s.x[0] = same ? 1 : 0;
    });
    // CGSize vImageBuffer_GetSize(const vImage_Buffer *buf): (width, height) in d0, d1.
    kernel("_vImageBuffer_GetSize", [](CpuState& s, GuestMemory& m) {
        const auto b = s.x[0] ? read_buffer(m, s.x[0]) : Buffer{};
        return_real(s, static_cast<double>(b.width), 0);
        return_real(s, static_cast<double>(b.height), 1);
    });
}

} // namespace

void install_vimage_cg(InstallContext& context) { std::make_shared<VImageCG>(context)->install(); }

} // namespace alchemy::gui::fill
