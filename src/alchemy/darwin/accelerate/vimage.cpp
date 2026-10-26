#include "alchemy/darwin/accelerate/install.hpp"
#include "alchemy/gui/fill_provider.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

// vImage beyond vdsp.cpp's Y'CbCr conversions, on vImage_Buffer {void *data; vImagePixelCount
// height, width; size_t rowBytes}, computed on the host per the documented semantics:
// - Buffers: vImageBuffer_Init (malloc'd through the guest, rows 16-byte aligned; kvImageNoAllocate
//   fills in the geometry only), vImageCopyBuffer, vImageBufferFill_ARGB8888,
//   vImageOverwriteChannelsWithScalar_ARGB8888.
// - Conversions: Planar8 <-> PlanarF (by a min/max range), ARGB8888 <-> four Planar8 planes,
//   RGB888 <-> ARGB8888, RGB565 <-> ARGB8888, vImagePermuteChannels_ARGB8888, premultiplying and
//   unpremultiplying ARGB8888 and RGBA8888 (alpha first or last).
// - Geometry: horizontal and vertical reflection and 90-degree rotation of ARGB8888, Planar8 and
//   PlanarF; scaling of ARGB8888, Planar8, PlanarF and ARGBFFFF by a separable Lanczos filter
//   (3 lobes; 5 with kvImageHighQualityResampling), each channel on its own.
// - Convolution: box and tent of ARGB8888 and Planar8; general kernels of Planar8, PlanarF and
//   ARGB8888 (integer kernels with a divisor); the edge styles kvImageCopyInPlace,
//   kvImageBackgroundColorFill, kvImageEdgeExtend and kvImageTruncateKernel (exactly one).
// - Histograms of Planar8 and ARGB8888.
// Temporary buffers are not used: a kvImageGetTempBufferSize query answers 0.
namespace alchemy::gui::fill {
namespace {

constexpr std::int64_t no_error = 0, roi_larger_than_input = -21766, invalid_kernel_size = -21767, invalid_edge_style = -21768,
    invalid_offset_x = -21769, invalid_offset_y = -21770, memory_allocation_error = -21771, null_pointer = -21772,
    invalid_parameter = -21773;
// vImage_Flags
constexpr std::uint32_t leave_alpha_unchanged = 1, copy_in_place = 2, background_color_fill = 4, edge_extend = 8,
    high_quality_resampling = 32, truncate_kernel = 64, get_temp_buffer_size = 128, no_allocate = 512;
constexpr std::uint64_t maximum_pixels = 1ULL << 28;
constexpr std::string_view libsystem = "/usr/lib/libSystem.B.dylib";

// A buffer's pixels on the host: `channels` values of type T per pixel, rows packed.
template <class T> struct Image {
    std::uint64_t width{}, height{};
    unsigned channels{1};
    std::vector<T> pixels;
    T& at(std::uint64_t x, std::uint64_t y, unsigned c) { return pixels[(y * width + x) * channels + c]; }
    T at(std::uint64_t x, std::uint64_t y, unsigned c) const { return pixels[(y * width + x) * channels + c]; }
};

struct Buffer { Address data{}; std::uint64_t height{}, width{}, row_bytes{}; };
Buffer read_buffer(GuestMemory& m, Address at) { return {m.read64(at), m.read64(at + 8), m.read64(at + 16), m.read64(at + 24)}; }
bool valid(const Buffer& b, unsigned pixel_bytes) {
    return b.data && b.width * b.height <= maximum_pixels && b.row_bytes >= b.width * pixel_bytes;
}

template <class T> Image<T> load(GuestMemory& m, const Buffer& b, unsigned channels, std::uint64_t width, std::uint64_t height) {
    Image<T> image{width, height, channels, std::vector<T>(static_cast<std::size_t>(width * height * channels))};
    const auto row = static_cast<std::size_t>(width * channels * sizeof(T));
    for (std::uint64_t y = 0; y < height && row; ++y)
        m.read_bytes(b.data + y * b.row_bytes, std::span(reinterpret_cast<std::uint8_t*>(image.pixels.data() + y * width * channels), row));
    return image;
}
template <class T> void save(GuestMemory& m, const Buffer& b, const Image<T>& image) {
    const auto row = static_cast<std::size_t>(image.width * image.channels * sizeof(T));
    for (std::uint64_t y = 0; y < image.height && row; ++y)
        m.write_bytes(b.data + y * b.row_bytes, std::span(reinterpret_cast<const std::uint8_t*>(image.pixels.data() + y * image.width * image.channels), row));
}

std::uint8_t clamp8(double value) { return static_cast<std::uint8_t>(std::clamp(std::lround(value), 0L, 255L)); }

double lanczos(double x, double lobes) {
    if (x == 0) return 1;
    if (std::fabs(x) >= lobes) return 0;
    const double px = std::numbers::pi * x;
    return lobes * std::sin(px) * std::sin(px / lobes) / (px * px);
}

// Resamples one axis: `out` samples from `in` samples (stride between successive samples).
template <class T> std::vector<double> resample_weights(std::uint64_t in, std::uint64_t out, double lobes, std::vector<std::int64_t>& first) {
    const double scale = static_cast<double>(in) / static_cast<double>(out);
    const double support = lobes * std::max(1.0, scale);
    const auto taps = static_cast<std::size_t>(std::ceil(support) * 2 + 1);
    std::vector<double> weights(static_cast<std::size_t>(out) * taps, 0.0);
    first.assign(static_cast<std::size_t>(out), 0);
    for (std::uint64_t i = 0; i < out; ++i) {
        const double center = (static_cast<double>(i) + 0.5) * scale - 0.5;
        const auto start = static_cast<std::int64_t>(std::floor(center - support)) + 1;
        first[i] = start;
        double total = 0;
        for (std::size_t k = 0; k < taps; ++k) {
            const double x = (static_cast<double>(start + static_cast<std::int64_t>(k)) - center) / std::max(1.0, scale);
            const double w = lanczos(x, lobes);
            weights[i * taps + k] = w;
            total += w;
        }
        if (total != 0) for (std::size_t k = 0; k < taps; ++k) weights[i * taps + k] /= total;
    }
    return weights;
}

template <class T> Image<T> scale(const Image<T>& src, std::uint64_t width, std::uint64_t height, double lobes, bool integer) {
    const auto pass = [&](const Image<double>& in, std::uint64_t out_w, std::uint64_t out_h, bool horizontal) {
        Image<double> out{out_w, out_h, in.channels, std::vector<double>(static_cast<std::size_t>(out_w * out_h * in.channels))};
        std::vector<std::int64_t> first;
        const auto in_len = horizontal ? in.width : in.height, out_len = horizontal ? out_w : out_h;
        const auto weights = resample_weights<T>(in_len, out_len, lobes, first);
        const auto taps = weights.size() / static_cast<std::size_t>(out_len);
        for (std::uint64_t y = 0; y < out_h; ++y)
            for (std::uint64_t x = 0; x < out_w; ++x) {
                const auto i = horizontal ? x : y;
                for (unsigned c = 0; c < in.channels; ++c) {
                    double total = 0;
                    for (std::size_t k = 0; k < taps; ++k) {
                        const auto j = std::clamp<std::int64_t>(first[i] + static_cast<std::int64_t>(k), 0, static_cast<std::int64_t>(in_len) - 1);
                        total += weights[i * taps + k] * (horizontal ? in.at(static_cast<std::uint64_t>(j), y, c) : in.at(x, static_cast<std::uint64_t>(j), c));
                    }
                    out.at(x, y, c) = total;
                }
            }
        return out;
    };
    Image<double> work{src.width, src.height, src.channels, std::vector<double>(src.pixels.begin(), src.pixels.end())};
    work = pass(work, width, src.height, true);
    work = pass(work, width, height, false);
    Image<T> out{width, height, src.channels, std::vector<T>(work.pixels.size())};
    for (std::size_t i = 0; i < out.pixels.size(); ++i)
        out.pixels[i] = integer ? static_cast<T>(clamp8(work.pixels[i])) : static_cast<T>(work.pixels[i]);
    return out;
}

class VImage final : public Provider {
public:
    using Provider::Provider;
    void install();

private:
    void kernel(const std::string& name, transmute::GuestCallHandler handler, const std::source_location& where = std::source_location::current()) {
        if (has(accelerate, name)) return;
        raw(accelerate, name, std::move(handler), where);
    }
    void install_buffers();
    void install_conversions();
    void install_geometry();
    void install_convolution();
};

void result(CpuState& s, std::int64_t value) { s.x[0] = static_cast<std::uint64_t>(value); }

void VImage::install_buffers() {
    // vImageBuffer_Init(vImage_Buffer *, vImagePixelCount height, width, uint32_t pixelBits, vImage_Flags)
    kernel("_vImageBuffer_Init", [this](CpuState& s, GuestMemory& m) {
        const auto out = s.x[0], height = s.x[1], width = s.x[2];
        const auto bits = static_cast<std::uint32_t>(s.x[3]), flags = static_cast<std::uint32_t>(s.x[4]);
        if (!out) return result(s, null_pointer);
        if (!bits || !width || !height || width * height > maximum_pixels) return result(s, invalid_parameter);
        const auto row_bytes = ((width * bits + 7) / 8 + 15) & ~std::uint64_t{15};
        Address data{};
        if (!(flags & no_allocate)) {
            data = call(libsystem, "_malloc", {row_bytes * height});
            if (!data) return result(s, memory_allocation_error);
        }
        m.write64(out, data);
        m.write64(out + 8, height);
        m.write64(out + 16, width);
        m.write64(out + 24, row_bytes);
        result(s, no_error);
    });
    // vImageCopyBuffer(const vImage_Buffer *src, const vImage_Buffer *dest, size_t pixelSize, vImage_Flags)
    kernel("_vImageCopyBuffer", [](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[1]) return result(s, null_pointer);
        const auto src = read_buffer(m, s.x[0]), dest = read_buffer(m, s.x[1]);
        const auto pixel = s.x[2];
        if (!pixel || !valid(src, static_cast<unsigned>(pixel)) || !valid(dest, static_cast<unsigned>(pixel))) return result(s, invalid_parameter);
        if (src.width < dest.width || src.height < dest.height) return result(s, roi_larger_than_input);
        std::vector<std::uint8_t> row(static_cast<std::size_t>(dest.width * pixel));
        for (std::uint64_t y = 0; y < dest.height; ++y) {
            m.read_bytes(src.data + y * src.row_bytes, row);
            m.write_bytes(dest.data + y * dest.row_bytes, row);
        }
        result(s, no_error);
    });
    // vImageBufferFill_ARGB8888(const vImage_Buffer *dest, const Pixel_8888 color, vImage_Flags)
    kernel("_vImageBufferFill_ARGB8888", [](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[1]) return result(s, null_pointer);
        const auto dest = read_buffer(m, s.x[0]);
        if (!valid(dest, 4)) return result(s, invalid_parameter);
        std::array<std::uint8_t, 4> color{};
        m.read_bytes(s.x[1], color);
        std::vector<std::uint8_t> row(static_cast<std::size_t>(dest.width * 4));
        for (std::size_t i = 0; i < row.size(); ++i) row[i] = color[i % 4];
        for (std::uint64_t y = 0; y < dest.height; ++y) m.write_bytes(dest.data + y * dest.row_bytes, row);
        result(s, no_error);
    });
    // vImageOverwriteChannelsWithScalar_ARGB8888(Pixel_8 scalar, src, dest, uint8_t copyMask, flags):
    // mask 0x8 is the first channel (alpha), 0x1 the last.
    kernel("_vImageOverwriteChannelsWithScalar_ARGB8888", [](CpuState& s, GuestMemory& m) {
        if (!s.x[1] || !s.x[2]) return result(s, null_pointer);
        const auto src = read_buffer(m, s.x[1]), dest = read_buffer(m, s.x[2]);
        if (!valid(src, 4) || !valid(dest, 4)) return result(s, invalid_parameter);
        if (src.width < dest.width || src.height < dest.height) return result(s, roi_larger_than_input);
        auto image = load<std::uint8_t>(m, src, 4, dest.width, dest.height);
        const auto scalar = static_cast<std::uint8_t>(s.x[0]), mask = static_cast<std::uint8_t>(s.x[3]);
        for (std::size_t i = 0; i < image.pixels.size(); ++i)
            if (mask & (0x8 >> (i % 4))) image.pixels[i] = scalar;
        save(m, dest, image);
        result(s, no_error);
    });
    // Histograms: vImageHistogramCalculation_Planar8(src, vImagePixelCount histogram[256], flags);
    // _ARGB8888(src, vImagePixelCount *histogram[4], flags).
    for (const unsigned channels : {1U, 4U})
        kernel(channels == 1 ? "_vImageHistogramCalculation_Planar8" : "_vImageHistogramCalculation_ARGB8888", [channels](CpuState& s, GuestMemory& m) {
            if (!s.x[0] || !s.x[1]) return result(s, null_pointer);
            const auto src = read_buffer(m, s.x[0]);
            if (!valid(src, channels)) return result(s, invalid_parameter);
            const auto image = load<std::uint8_t>(m, src, channels, src.width, src.height);
            std::vector<std::array<std::uint64_t, 256>> counts(channels);
            for (auto& c : counts) c.fill(0);
            for (std::size_t i = 0; i < image.pixels.size(); ++i) ++counts[i % channels][image.pixels[i]];
            for (unsigned c = 0; c < channels; ++c) {
                const auto table = channels == 1 ? s.x[1] : m.read64(s.x[1] + 8ULL * c);
                if (!table) return result(s, null_pointer);
                for (unsigned v = 0; v < 256; ++v) m.write64(table + 8ULL * v, counts[c][v]);
            }
            result(s, no_error);
        });
}

void VImage::install_conversions() {
    const auto real32 = [](const CpuState& s, unsigned i) { return static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(s.v[i][0]))); };
    // vImageConvert_Planar8toPlanarF(src, dest, Pixel_F maxFloat (s0), Pixel_F minFloat (s1), flags)
    kernel("_vImageConvert_Planar8toPlanarF", [real32](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[1]) return result(s, null_pointer);
        const auto src = read_buffer(m, s.x[0]), dest = read_buffer(m, s.x[1]);
        if (!valid(src, 1) || !valid(dest, 4)) return result(s, invalid_parameter);
        if (src.width < dest.width || src.height < dest.height) return result(s, roi_larger_than_input);
        const double high = real32(s, 0), low = real32(s, 1);
        const auto in = load<std::uint8_t>(m, src, 1, dest.width, dest.height);
        Image<float> out{in.width, in.height, 1, std::vector<float>(in.pixels.size())};
        for (std::size_t i = 0; i < in.pixels.size(); ++i) out.pixels[i] = static_cast<float>(low + (high - low) * in.pixels[i] / 255.0);
        save(m, dest, out);
        result(s, no_error);
    });
    // vImageConvert_PlanarFtoPlanar8(src, dest, maxFloat (s0), minFloat (s1), flags): clamped and rounded.
    kernel("_vImageConvert_PlanarFtoPlanar8", [real32](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[1]) return result(s, null_pointer);
        const auto src = read_buffer(m, s.x[0]), dest = read_buffer(m, s.x[1]);
        if (!valid(src, 4) || !valid(dest, 1)) return result(s, invalid_parameter);
        if (src.width < dest.width || src.height < dest.height) return result(s, roi_larger_than_input);
        const double high = real32(s, 0), low = real32(s, 1);
        if (!(high > low)) return result(s, invalid_parameter);
        const auto in = load<float>(m, src, 1, dest.width, dest.height);
        Image<std::uint8_t> out{in.width, in.height, 1, std::vector<std::uint8_t>(in.pixels.size())};
        for (std::size_t i = 0; i < in.pixels.size(); ++i) out.pixels[i] = clamp8((in.pixels[i] - low) * 255.0 / (high - low));
        save(m, dest, out);
        result(s, no_error);
    });
    // vImageConvert_ARGB8888toPlanar8(src, destA, destR, destG, destB, flags) and the reverse
    // vImageConvert_Planar8toARGB8888(srcA, srcR, srcG, srcB, dest, flags).
    kernel("_vImageConvert_ARGB8888toPlanar8", [](CpuState& s, GuestMemory& m) {
        for (unsigned i = 0; i < 5; ++i) if (!s.x[i]) return result(s, null_pointer);
        const auto src = read_buffer(m, s.x[0]);
        std::array<Buffer, 4> planes{};
        for (unsigned c = 0; c < 4; ++c) {
            planes[c] = read_buffer(m, s.x[1 + c]);
            if (!valid(planes[c], 1) || planes[c].width > src.width || planes[c].height > src.height) return result(s, roi_larger_than_input);
        }
        if (!valid(src, 4)) return result(s, invalid_parameter);
        const auto in = load<std::uint8_t>(m, src, 4, planes[0].width, planes[0].height);
        for (unsigned c = 0; c < 4; ++c) {
            Image<std::uint8_t> out{in.width, in.height, 1, std::vector<std::uint8_t>(static_cast<std::size_t>(in.width * in.height))};
            for (std::size_t i = 0; i < out.pixels.size(); ++i) out.pixels[i] = in.pixels[i * 4 + c];
            save(m, planes[c], out);
        }
        result(s, no_error);
    });
    kernel("_vImageConvert_Planar8toARGB8888", [](CpuState& s, GuestMemory& m) {
        for (unsigned i = 0; i < 5; ++i) if (!s.x[i]) return result(s, null_pointer);
        const auto dest = read_buffer(m, s.x[4]);
        if (!valid(dest, 4)) return result(s, invalid_parameter);
        Image<std::uint8_t> out{dest.width, dest.height, 4, std::vector<std::uint8_t>(static_cast<std::size_t>(dest.width * dest.height * 4))};
        for (unsigned c = 0; c < 4; ++c) {
            const auto plane = read_buffer(m, s.x[c]);
            if (!valid(plane, 1) || plane.width < dest.width || plane.height < dest.height) return result(s, roi_larger_than_input);
            const auto in = load<std::uint8_t>(m, plane, 1, dest.width, dest.height);
            for (std::size_t i = 0; i < in.pixels.size(); ++i) out.pixels[i * 4 + c] = in.pixels[i];
        }
        save(m, dest, out);
        result(s, no_error);
    });
    // vImageConvert_RGB888toARGB8888(src, const vImage_Buffer *srcA or NULL, Pixel_8 alpha, dest, bool premultiply, flags)
    kernel("_vImageConvert_RGB888toARGB8888", [](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[3]) return result(s, null_pointer);
        const auto src = read_buffer(m, s.x[0]), dest = read_buffer(m, s.x[3]);
        if (!valid(src, 3) || !valid(dest, 4)) return result(s, invalid_parameter);
        if (src.width < dest.width || src.height < dest.height) return result(s, roi_larger_than_input);
        const auto rgb = load<std::uint8_t>(m, src, 3, dest.width, dest.height);
        Image<std::uint8_t> alpha;
        if (s.x[1]) alpha = load<std::uint8_t>(m, read_buffer(m, s.x[1]), 1, dest.width, dest.height);
        const bool premultiply = (s.x[4] & 0xff) != 0;
        Image<std::uint8_t> out{dest.width, dest.height, 4, std::vector<std::uint8_t>(static_cast<std::size_t>(dest.width * dest.height * 4))};
        for (std::size_t i = 0; i < static_cast<std::size_t>(dest.width * dest.height); ++i) {
            const auto a = s.x[1] ? alpha.pixels[i] : static_cast<std::uint8_t>(s.x[2]);
            out.pixels[i * 4] = a;
            for (unsigned c = 0; c < 3; ++c)
                out.pixels[i * 4 + 1 + c] = premultiply ? static_cast<std::uint8_t>((rgb.pixels[i * 3 + c] * a + 127) / 255) : rgb.pixels[i * 3 + c];
        }
        save(m, dest, out);
        result(s, no_error);
    });
    // vImageConvert_ARGB8888toRGB888(src, dest, flags): alpha dropped.
    kernel("_vImageConvert_ARGB8888toRGB888", [](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[1]) return result(s, null_pointer);
        const auto src = read_buffer(m, s.x[0]), dest = read_buffer(m, s.x[1]);
        if (!valid(src, 4) || !valid(dest, 3)) return result(s, invalid_parameter);
        if (src.width < dest.width || src.height < dest.height) return result(s, roi_larger_than_input);
        const auto in = load<std::uint8_t>(m, src, 4, dest.width, dest.height);
        Image<std::uint8_t> out{dest.width, dest.height, 3, std::vector<std::uint8_t>(static_cast<std::size_t>(dest.width * dest.height * 3))};
        for (std::size_t i = 0; i < static_cast<std::size_t>(dest.width * dest.height); ++i)
            for (unsigned c = 0; c < 3; ++c) out.pixels[i * 3 + c] = in.pixels[i * 4 + 1 + c];
        save(m, dest, out);
        result(s, no_error);
    });
    // vImageConvert_RGB565toARGB8888(Pixel_8 alpha, src, dest, flags) and vImageConvert_ARGB8888toRGB565(src, dest, flags).
    kernel("_vImageConvert_RGB565toARGB8888", [](CpuState& s, GuestMemory& m) {
        if (!s.x[1] || !s.x[2]) return result(s, null_pointer);
        const auto src = read_buffer(m, s.x[1]), dest = read_buffer(m, s.x[2]);
        if (!valid(src, 2) || !valid(dest, 4)) return result(s, invalid_parameter);
        if (src.width < dest.width || src.height < dest.height) return result(s, roi_larger_than_input);
        const auto in = load<std::uint16_t>(m, src, 1, dest.width, dest.height);
        Image<std::uint8_t> out{dest.width, dest.height, 4, std::vector<std::uint8_t>(static_cast<std::size_t>(dest.width * dest.height * 4))};
        for (std::size_t i = 0; i < in.pixels.size(); ++i) {
            const auto p = in.pixels[i];
            out.pixels[i * 4] = static_cast<std::uint8_t>(s.x[0]);
            out.pixels[i * 4 + 1] = static_cast<std::uint8_t>(((p >> 11) & 31) * 255 / 31);
            out.pixels[i * 4 + 2] = static_cast<std::uint8_t>(((p >> 5) & 63) * 255 / 63);
            out.pixels[i * 4 + 3] = static_cast<std::uint8_t>((p & 31) * 255 / 31);
        }
        save(m, dest, out);
        result(s, no_error);
    });
    kernel("_vImageConvert_ARGB8888toRGB565", [](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[1]) return result(s, null_pointer);
        const auto src = read_buffer(m, s.x[0]), dest = read_buffer(m, s.x[1]);
        if (!valid(src, 4) || !valid(dest, 2)) return result(s, invalid_parameter);
        if (src.width < dest.width || src.height < dest.height) return result(s, roi_larger_than_input);
        const auto in = load<std::uint8_t>(m, src, 4, dest.width, dest.height);
        Image<std::uint16_t> out{dest.width, dest.height, 1, std::vector<std::uint16_t>(static_cast<std::size_t>(dest.width * dest.height))};
        for (std::size_t i = 0; i < out.pixels.size(); ++i)
            out.pixels[i] = static_cast<std::uint16_t>(((in.pixels[i * 4 + 1] * 31 + 127) / 255) << 11 | ((in.pixels[i * 4 + 2] * 63 + 127) / 255) << 5 |
                                                       ((in.pixels[i * 4 + 3] * 31 + 127) / 255));
        save(m, dest, out);
        result(s, no_error);
    });
    // vImagePermuteChannels_ARGB8888(src, dest, const uint8_t permuteMap[4], flags): dest[i] = src[map[i]].
    kernel("_vImagePermuteChannels_ARGB8888", [](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[1] || !s.x[2]) return result(s, null_pointer);
        const auto src = read_buffer(m, s.x[0]), dest = read_buffer(m, s.x[1]);
        if (!valid(src, 4) || !valid(dest, 4)) return result(s, invalid_parameter);
        if (src.width < dest.width || src.height < dest.height) return result(s, roi_larger_than_input);
        std::array<std::uint8_t, 4> map{};
        m.read_bytes(s.x[2], map);
        for (const auto c : map) if (c > 3) return result(s, invalid_parameter);
        const auto in = load<std::uint8_t>(m, src, 4, dest.width, dest.height);
        auto out = in;
        for (std::size_t i = 0; i < in.pixels.size(); i += 4)
            for (unsigned c = 0; c < 4; ++c) out.pixels[i + c] = in.pixels[i + map[c]];
        save(m, dest, out);
        result(s, no_error);
    });
    // Premultiplying and unpremultiplying (src, dest, flags): ARGB8888 has alpha first, RGBA8888 last.
    for (const bool alpha_first : {true, false})
        for (const bool premultiply : {true, false}) {
            const std::string name = std::string(premultiply ? "_vImagePremultiplyData_" : "_vImageUnpremultiplyData_") + (alpha_first ? "ARGB8888" : "RGBA8888");
            kernel(name, [alpha_first, premultiply](CpuState& s, GuestMemory& m) {
                if (!s.x[0] || !s.x[1]) return result(s, null_pointer);
                const auto src = read_buffer(m, s.x[0]), dest = read_buffer(m, s.x[1]);
                if (!valid(src, 4) || !valid(dest, 4)) return result(s, invalid_parameter);
                if (src.width < dest.width || src.height < dest.height) return result(s, roi_larger_than_input);
                auto image = load<std::uint8_t>(m, src, 4, dest.width, dest.height);
                const unsigned alpha = alpha_first ? 0 : 3;
                for (std::size_t i = 0; i < image.pixels.size(); i += 4) {
                    const unsigned a = image.pixels[i + alpha];
                    for (unsigned c = 0; c < 4; ++c) {
                        if (c == alpha) continue;
                        auto& v = image.pixels[i + c];
                        v = premultiply ? static_cast<std::uint8_t>((v * a + 127) / 255)
                                        : a ? static_cast<std::uint8_t>(std::min(255U, (v * 255U + a / 2) / a)) : std::uint8_t{0};
                    }
                }
                save(m, dest, image);
                result(s, no_error);
            });
        }
}

void VImage::install_geometry() {
    // Reflections (src, dest, flags) of ARGB8888, Planar8, PlanarF.
    struct Format { const char* suffix; unsigned channels, bytes; };
    constexpr std::array<Format, 3> formats{{{"ARGB8888", 4, 1}, {"Planar8", 1, 1}, {"PlanarF", 1, 4}}};
    for (const auto& format : formats) {
        for (const bool horizontal : {true, false}) {
            const auto f = format;
            kernel(std::string(horizontal ? "_vImageHorizontalReflect_" : "_vImageVerticalReflect_") + f.suffix, [f, horizontal](CpuState& s, GuestMemory& m) {
                if (!s.x[0] || !s.x[1]) return result(s, null_pointer);
                const auto src = read_buffer(m, s.x[0]), dest = read_buffer(m, s.x[1]);
                const auto pixel = f.channels * f.bytes;
                if (!valid(src, pixel) || !valid(dest, pixel)) return result(s, invalid_parameter);
                if (src.width != dest.width || src.height != dest.height) return result(s, roi_larger_than_input);
                const auto in = load<std::uint8_t>(m, src, pixel, src.width, src.height);
                auto out = in;
                for (std::uint64_t y = 0; y < in.height; ++y)
                    for (std::uint64_t x = 0; x < in.width; ++x)
                        for (unsigned b = 0; b < pixel; ++b)
                            out.at(x, y, b) = horizontal ? in.at(in.width - 1 - x, y, b) : in.at(x, in.height - 1 - y, b);
                save(m, dest, out);
                result(s, no_error);
            });
        }
        // vImageRotate90_<format>(src, dest, uint8_t rotationConstant, backColor, flags): quarter turns
        // counterclockwise (0-3); the destination's geometry follows the turn.
        const auto f = format;
        kernel(std::string("_vImageRotate90_") + f.suffix, [f](CpuState& s, GuestMemory& m) {
            if (!s.x[0] || !s.x[1]) return result(s, null_pointer);
            const auto src = read_buffer(m, s.x[0]), dest = read_buffer(m, s.x[1]);
            const auto pixel = f.channels * f.bytes;
            const auto turns = static_cast<unsigned>(s.x[2] & 3);
            if (!valid(src, pixel) || !valid(dest, pixel)) return result(s, invalid_parameter);
            const bool swap = turns & 1;
            if ((swap ? src.height : src.width) != dest.width || (swap ? src.width : src.height) != dest.height) return result(s, invalid_parameter);
            const auto in = load<std::uint8_t>(m, src, pixel, src.width, src.height);
            Image<std::uint8_t> out{dest.width, dest.height, pixel, std::vector<std::uint8_t>(in.pixels.size())};
            for (std::uint64_t y = 0; y < dest.height; ++y)
                for (std::uint64_t x = 0; x < dest.width; ++x) {
                    std::uint64_t sx{}, sy{};
                    switch (turns) {
                    case 0: sx = x; sy = y; break;
                    case 1: sx = src.width - 1 - y; sy = x; break;                 // 90 counterclockwise
                    case 2: sx = src.width - 1 - x; sy = src.height - 1 - y; break;
                    default: sx = y; sy = src.height - 1 - x; break;               // 90 clockwise
                    }
                    for (unsigned b = 0; b < pixel; ++b) out.at(x, y, b) = in.at(sx, sy, b);
                }
            save(m, dest, out);
            result(s, no_error);
        });
    }
    // vImageScale_<format>(src, dest, void *tempBuffer, flags): Lanczos.
    const auto scale_kernel = [this]<class T>(const char* name, unsigned channels, bool integer, T) {
        kernel(name, [channels, integer](CpuState& s, GuestMemory& m) {
            const auto flags = static_cast<std::uint32_t>(s.x[3]);
            if (flags & get_temp_buffer_size) return result(s, 0);
            if (!s.x[0] || !s.x[1]) return result(s, null_pointer);
            const auto src = read_buffer(m, s.x[0]), dest = read_buffer(m, s.x[1]);
            const auto pixel = static_cast<unsigned>(channels * sizeof(T));
            if (!valid(src, pixel) || !valid(dest, pixel) || !src.width || !src.height) return result(s, invalid_parameter);
            const auto in = load<T>(m, src, channels, src.width, src.height);
            save(m, dest, scale(in, dest.width, dest.height, (flags & high_quality_resampling) ? 5.0 : 3.0, integer));
            result(s, no_error);
        });
    };
    scale_kernel("_vImageScale_ARGB8888", 4, true, std::uint8_t{});
    scale_kernel("_vImageScale_Planar8", 1, true, std::uint8_t{});
    scale_kernel("_vImageScale_PlanarF", 1, false, float{});
    scale_kernel("_vImageScale_ARGBFFFF", 4, false, float{});
}

// One convolution: `weights` (kernel_height x kernel_width, row-major) over `src` at the ROI offset,
// per channel; the edge style decides pixels whose kernel leaves the source.
template <class T>
std::int64_t convolve(const Image<T>& src, Image<T>& dest, std::int64_t offset_x, std::int64_t offset_y, std::uint64_t kernel_height,
                      std::uint64_t kernel_width, const std::vector<double>& weights, double divisor, const std::vector<double>& background,
                      std::uint32_t flags, bool integer, std::uint32_t skip_channel_mask = 0) {
    const auto edges = flags & (copy_in_place | background_color_fill | edge_extend | truncate_kernel);
    if (std::popcount(edges) != 1) return invalid_edge_style;
    if (!(kernel_height & 1) || !(kernel_width & 1)) return invalid_kernel_size;
    if (offset_x < 0 || offset_x + static_cast<std::int64_t>(dest.width) > static_cast<std::int64_t>(src.width)) return invalid_offset_x;
    if (offset_y < 0 || offset_y + static_cast<std::int64_t>(dest.height) > static_cast<std::int64_t>(src.height)) return invalid_offset_y;
    const auto half_w = static_cast<std::int64_t>(kernel_width / 2), half_h = static_cast<std::int64_t>(kernel_height / 2);
    const auto sw = static_cast<std::int64_t>(src.width), sh = static_cast<std::int64_t>(src.height);
    double total_weight = 0;
    for (const auto w : weights) total_weight += w;
    for (std::uint64_t y = 0; y < dest.height; ++y)
        for (std::uint64_t x = 0; x < dest.width; ++x) {
            const auto cx = static_cast<std::int64_t>(x) + offset_x, cy = static_cast<std::int64_t>(y) + offset_y;
            const bool inside = cx - half_w >= 0 && cx + half_w < sw && cy - half_h >= 0 && cy + half_h < sh;
            for (unsigned c = 0; c < src.channels; ++c) {
                if (skip_channel_mask & (1U << c)) { dest.at(x, y, c) = src.at(static_cast<std::uint64_t>(cx), static_cast<std::uint64_t>(cy), c); continue; }
                if (!inside && (edges & copy_in_place)) { dest.at(x, y, c) = src.at(static_cast<std::uint64_t>(cx), static_cast<std::uint64_t>(cy), c); continue; }
                double sum = 0, used_weight = 0;
                for (std::int64_t ky = 0; ky < static_cast<std::int64_t>(kernel_height); ++ky)
                    for (std::int64_t kx = 0; kx < static_cast<std::int64_t>(kernel_width); ++kx) {
                        auto sx = cx + kx - half_w, sy = cy + ky - half_h;
                        const double w = weights[static_cast<std::size_t>(ky * static_cast<std::int64_t>(kernel_width) + kx)];
                        double v{};
                        if (sx < 0 || sx >= sw || sy < 0 || sy >= sh) {
                            if (edges & edge_extend) { sx = std::clamp<std::int64_t>(sx, 0, sw - 1); sy = std::clamp<std::int64_t>(sy, 0, sh - 1); }
                            else if (edges & background_color_fill) { sum += w * background[c]; used_weight += w; continue; }
                            else continue;   // truncate: the pixel's weight is left out
                        }
                        v = src.at(static_cast<std::uint64_t>(sx), static_cast<std::uint64_t>(sy), c);
                        sum += w * v;
                        used_weight += w;
                    }
                double value = sum / divisor;
                if ((edges & truncate_kernel) && used_weight != 0 && used_weight != total_weight) value = sum * total_weight / used_weight / divisor;
                dest.at(x, y, c) = integer ? static_cast<T>(clamp8(value)) : static_cast<T>(value);
            }
        }
    return no_error;
}

void VImage::install_convolution() {
    // Box and tent (src, dest, tempBuffer, srcOffsetToROI_X, _Y, kernel_height, kernel_width,
    // background (Pixel_8888 * or Pixel_8), flags (on the stack)).
    for (const bool tent : {false, true})
        for (const unsigned channels : {4U, 1U}) {
            const std::string name = std::string(tent ? "_vImageTentConvolve_" : "_vImageBoxConvolve_") + (channels == 4 ? "ARGB8888" : "Planar8");
            kernel(name, [tent, channels](CpuState& s, GuestMemory& m) {
                const auto flags = m.read32(s.sp);
                if (flags & get_temp_buffer_size) return result(s, 0);
                if (!s.x[0] || !s.x[1]) return result(s, null_pointer);
                const auto src = read_buffer(m, s.x[0]), dest = read_buffer(m, s.x[1]);
                if (!valid(src, channels) || !valid(dest, channels)) return result(s, invalid_parameter);
                const auto kh = s.x[5], kw = s.x[6];
                if (!kh || !kw || kh > 1023 || kw > 1023) return result(s, invalid_kernel_size);
                std::vector<double> weights(static_cast<std::size_t>(kh * kw), 1.0);
                if (tent)
                    for (std::uint64_t y = 0; y < kh; ++y)
                        for (std::uint64_t x = 0; x < kw; ++x)
                            weights[y * kw + x] = double(std::min(x, kw - 1 - x) + 1) * double(std::min(y, kh - 1 - y) + 1);
                double divisor = 0;
                for (const auto w : weights) divisor += w;
                std::vector<double> background(channels, 0.0);
                if (flags & background_color_fill) {
                    if (channels == 4) { if (!s.x[7]) return result(s, null_pointer); for (unsigned c = 0; c < 4; ++c) background[c] = m.read8(s.x[7] + c); }
                    else background[0] = static_cast<double>(s.x[7] & 0xff);
                }
                const auto in = load<std::uint8_t>(m, src, channels, src.width, src.height);
                Image<std::uint8_t> out{dest.width, dest.height, channels, std::vector<std::uint8_t>(static_cast<std::size_t>(dest.width * dest.height * channels))};
                const auto status = convolve(in, out, static_cast<std::int64_t>(s.x[3]), static_cast<std::int64_t>(s.x[4]), kh, kw, weights, divisor,
                                             background, flags, true, (channels == 4 && (flags & leave_alpha_unchanged)) ? 1U : 0U);
                if (status) return result(s, status);
                save(m, dest, out);
                result(s, no_error);
            });
        }
    // vImageConvolve_PlanarF(src, dest, temp, offX, offY, const float *kernel, kernel_height, kernel_width,
    //     Pixel_F background (s0), flags (stack)).
    kernel("_vImageConvolve_PlanarF", [](CpuState& s, GuestMemory& m) {
        const auto flags = m.read32(s.sp);
        if (flags & get_temp_buffer_size) return result(s, 0);
        if (!s.x[0] || !s.x[1] || !s.x[5]) return result(s, null_pointer);
        const auto src = read_buffer(m, s.x[0]), dest = read_buffer(m, s.x[1]);
        if (!valid(src, 4) || !valid(dest, 4)) return result(s, invalid_parameter);
        const auto kh = s.x[6], kw = s.x[7];
        if (!kh || !kw || kh * kw > (1U << 20)) return result(s, invalid_kernel_size);
        std::vector<double> weights(static_cast<std::size_t>(kh * kw));
        for (std::size_t i = 0; i < weights.size(); ++i) weights[i] = std::bit_cast<float>(m.read32(s.x[5] + 4 * i));
        const std::vector<double> background{std::bit_cast<float>(static_cast<std::uint32_t>(s.v[0][0]))};
        const auto in = load<float>(m, src, 1, src.width, src.height);
        Image<float> out{dest.width, dest.height, 1, std::vector<float>(static_cast<std::size_t>(dest.width * dest.height))};
        const auto status = convolve(in, out, static_cast<std::int64_t>(s.x[3]), static_cast<std::int64_t>(s.x[4]), kh, kw, weights, 1.0, background, flags, false);
        if (status) return result(s, status);
        save(m, dest, out);
        result(s, no_error);
    });
    // vImageConvolve_Planar8 / _ARGB8888(src, dest, temp, offX, offY, const int16_t *kernel, kernel_height,
    //     kernel_width, int32_t divisor, background (Pixel_8 or Pixel_8888 *), flags): the last three on the
    //     stack at their natural alignment.
    for (const unsigned channels : {1U, 4U})
        kernel(channels == 1 ? "_vImageConvolve_Planar8" : "_vImageConvolve_ARGB8888", [channels](CpuState& s, GuestMemory& m) {
            const auto divisor = static_cast<std::int32_t>(m.read32(s.sp));
            const auto flags = channels == 1 ? m.read32(s.sp + 8) : m.read32(s.sp + 16);
            if (flags & get_temp_buffer_size) return result(s, 0);
            if (!s.x[0] || !s.x[1] || !s.x[5]) return result(s, null_pointer);
            const auto src = read_buffer(m, s.x[0]), dest = read_buffer(m, s.x[1]);
            if (!valid(src, channels) || !valid(dest, channels)) return result(s, invalid_parameter);
            const auto kh = s.x[6], kw = s.x[7];
            if (!kh || !kw || kh * kw > (1U << 20)) return result(s, invalid_kernel_size);
            std::vector<double> weights(static_cast<std::size_t>(kh * kw));
            for (std::size_t i = 0; i < weights.size(); ++i)
                weights[i] = static_cast<std::int16_t>(m.read8(s.x[5] + 2 * i) | (m.read8(s.x[5] + 2 * i + 1) << 8));
            std::vector<double> background(channels, 0.0);
            if (channels == 1) background[0] = m.read8(s.sp + 4);
            else if (const auto color = m.read64(s.sp + 8)) for (unsigned c = 0; c < 4; ++c) background[c] = m.read8(color + c);
            const auto in = load<std::uint8_t>(m, src, channels, src.width, src.height);
            Image<std::uint8_t> out{dest.width, dest.height, channels, std::vector<std::uint8_t>(static_cast<std::size_t>(dest.width * dest.height * channels))};
            const auto status = convolve(in, out, static_cast<std::int64_t>(s.x[3]), static_cast<std::int64_t>(s.x[4]), kh, kw, weights,
                                         divisor ? static_cast<double>(divisor) : 1.0, background, flags, true,
                                         (channels == 4 && (flags & leave_alpha_unchanged)) ? 1U : 0U);
            if (status) return result(s, status);
            save(m, dest, out);
            result(s, no_error);
        });
}

void VImage::install() {
    install_buffers();
    install_conversions();
    install_geometry();
    install_convolution();
}

} // namespace

void install_vimage(InstallContext& context) { std::make_shared<VImage>(context)->install(); }

} // namespace alchemy::gui::fill
