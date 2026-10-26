#include "alchemy/darwin/media/video_frames.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace alchemy::gui::completion_kit::media::video_frames {

namespace {

constexpr std::uint32_t four(const char (&code)[5]) {
    return (std::uint32_t(std::uint8_t(code[0])) << 24) | (std::uint32_t(std::uint8_t(code[1])) << 16) |
           (std::uint32_t(std::uint8_t(code[2])) << 8) | std::uint32_t(std::uint8_t(code[3]));
}

struct Layout {
    bool ten{};          // 10 bits in the high bits of 16
    bool full{};         // full range
    bool triplanar{};    // Cb and Cr planes (else interleaved CbCr)
    std::uint32_t shift_x{}, shift_y{};
};

bool layout_of(std::uint32_t format, Layout& out) {
    switch (format) {
    case four("420v"): out = {false, false, false, 1, 1}; return true;
    case four("420f"): out = {false, true, false, 1, 1}; return true;
    case four("x420"): out = {true, false, false, 1, 1}; return true;
    case four("xf20"): out = {true, true, false, 1, 1}; return true;
    case four("y420"): out = {false, false, true, 1, 1}; return true;
    case four("f420"): out = {false, true, true, 1, 1}; return true;
    case four("422v"): out = {false, false, false, 1, 0}; return true;
    case four("422f"): out = {false, true, false, 1, 0}; return true;
    case four("444v"): out = {false, false, false, 0, 0}; return true;
    case four("444f"): out = {false, true, false, 0, 0}; return true;
    default: return false;
    }
}

// A sample of a plane in [0, 1] code units (relative to the frame's bit depth).
double sample(const codecs::VideoFrame& f, int plane, std::uint32_t x, std::uint32_t y) {
    const auto& p = f.planes[plane];
    x = std::min(x, p.width - 1);
    y = std::min(y, p.height - 1);
    const auto* row = p.data.data() + std::size_t(y) * p.stride;
    const double code = f.bit_depth > 8 ? double(row[2 * x] | (row[2 * x + 1] << 8)) : double(row[x]);
    return code / double((1u << f.bit_depth) - 1) * 255.0;   // in 8-bit code units, fractional
}

// The chroma sample at chroma position (cx, cy) of an output subsampled by (sx, sy): the source's
// samples covering that area, averaged (or the nearest one when the source is coarser).
double chroma(const codecs::VideoFrame& f, int plane, std::uint32_t cx, std::uint32_t cy, std::uint32_t sx, std::uint32_t sy) {
    if (f.monochrome) return 128.0;
    // Luma-resolution area of the output sample.
    const std::uint32_t x0 = cx << sx, y0 = cy << sy, w = 1u << sx, h = 1u << sy;
    const std::uint32_t fx = f.chroma_shift_x, fy = f.chroma_shift_y;
    double sum = 0;
    int n = 0;
    for (std::uint32_t y = y0; y < y0 + h; y += 1u << fy)
        for (std::uint32_t x = x0; x < x0 + w; x += 1u << fx) {
            sum += sample(f, plane, x >> fx, y >> fy);
            ++n;
        }
    return sum / n;
}

// Range conversion of 8-bit-unit code values.
double luma_to(double y, bool from_full, bool to_full) {
    if (from_full == to_full) return y;
    return to_full ? (y - 16.0) * 255.0 / 219.0 : 16.0 + y * 219.0 / 255.0;
}
double chroma_to(double c, bool from_full, bool to_full) {
    if (from_full == to_full) return c;
    return to_full ? 128.0 + (c - 128.0) * 255.0 / 224.0 : 128.0 + (c - 128.0) * 224.0 / 255.0;
}

void store(std::uint8_t* at, double value, bool ten) {
    if (ten) {
        const auto code = static_cast<std::uint32_t>(std::clamp(std::lround(value * 1023.0 / 255.0), 0L, 1023L)) << 6;
        at[0] = static_cast<std::uint8_t>(code);
        at[1] = static_cast<std::uint8_t>(code >> 8);
    } else {
        at[0] = static_cast<std::uint8_t>(std::clamp(std::lround(value), 0L, 255L));
    }
}

// Exact copies where the layouts agree (the common case), else the general path.
bool write_ycbcr(const codecs::VideoFrame& f, const Layout& l, std::span<const pixels::Plane> planes) {
    if (planes.size() < (l.triplanar ? 3u : 2u)) return false;
    const std::uint32_t bytes = l.ten ? 2 : 1;
    const bool same_depth = (f.bit_depth > 8) == l.ten && (f.bit_depth == 8 || f.bit_depth == 10);
    // Luma.
    for (std::uint32_t y = 0; y < f.height; ++y) {
        auto* out = planes[0].data + std::size_t(y) * planes[0].bytes_per_row;
        if (same_depth && f.full_range == l.full && !l.ten) {
            std::memcpy(out, f.planes[0].data.data() + std::size_t(y) * f.planes[0].stride, f.width);
            continue;
        }
        for (std::uint32_t x = 0; x < f.width; ++x) store(out + std::size_t(x) * bytes, luma_to(sample(f, 0, x, y), f.full_range, l.full), l.ten);
    }
    // Chroma.
    const std::uint32_t cw = (f.width + (1u << l.shift_x) - 1) >> l.shift_x, ch = (f.height + (1u << l.shift_y) - 1) >> l.shift_y;
    const bool same_chroma = !f.monochrome && f.chroma_shift_x == l.shift_x && f.chroma_shift_y == l.shift_y;
    for (std::uint32_t y = 0; y < ch; ++y) {
        auto* cb = planes[1].data + std::size_t(y) * planes[1].bytes_per_row;
        auto* cr = l.triplanar ? planes[2].data + std::size_t(y) * planes[2].bytes_per_row : nullptr;
        if (same_chroma && same_depth && f.full_range == l.full && !l.ten && l.triplanar) {
            std::memcpy(cb, f.planes[1].data.data() + std::size_t(y) * f.planes[1].stride, cw);
            std::memcpy(cr, f.planes[2].data.data() + std::size_t(y) * f.planes[2].stride, cw);
            continue;
        }
        for (std::uint32_t x = 0; x < cw; ++x) {
            const double u = chroma_to(same_chroma ? sample(f, 1, x, y) : chroma(f, 1, x, y, l.shift_x, l.shift_y), f.full_range, l.full);
            const double v = chroma_to(same_chroma ? sample(f, 2, x, y) : chroma(f, 2, x, y, l.shift_x, l.shift_y), f.full_range, l.full);
            if (l.triplanar) {
                store(cb + x, u, false);
                store(cr + x, v, false);
            } else {
                store(cb + std::size_t(x) * 2 * bytes, u, l.ten);
                store(cb + std::size_t(x) * 2 * bytes + bytes, v, l.ten);
            }
        }
    }
    return true;
}

} // namespace

std::uint32_t native_format(const codecs::VideoFrame& f) {
    if (f.bit_depth > 8) return f.full_range ? four("xf20") : four("x420");
    if (!f.monochrome && f.chroma_shift_x == 0 && f.chroma_shift_y == 0) return f.full_range ? four("444f") : four("444v");
    if (!f.monochrome && f.chroma_shift_x == 1 && f.chroma_shift_y == 0) return f.full_range ? four("422f") : four("422v");
    return f.full_range ? four("420f") : four("420v");
}

bool writable(std::uint32_t format) {
    Layout l;
    return layout_of(format, l) || pixels::plane_count(format) != 0;
}

pixels::Matrix matrix_of(const codecs::VideoFrame& f) {
    switch (f.matrix) {
    case 1: return pixels::Matrix::bt709;
    case 5: case 6: case 7: return pixels::Matrix::bt601;
    case 9: case 10: return pixels::Matrix::bt2020;
    default: return pixels::default_matrix(f.width, f.height);
    }
}

bool write(const codecs::VideoFrame& f, std::uint32_t format, std::span<const pixels::Plane> planes) {
    if (!f.width || !f.height || planes.empty()) return false;
    Layout l;
    if (layout_of(format, l)) return write_ycbcr(f, l, planes);
    if (!pixels::plane_count(format)) return false;
    // RGB and the other formats pixel_convert writes: the picture is staged as tri-planar 8-bit
    // 4:2:0 (y420/f420; 4:2:2 and 4:4:4 chroma averaged down) and converted from there.
    pixels::Image target;
    target.format = format;
    target.width = f.width;
    target.height = f.height;
    target.planes.assign(planes.begin(), planes.end());
    target.matrix = matrix_of(f);
    const std::uint32_t cw = (f.width + 1) / 2, ch = (f.height + 1) / 2;
    std::vector<std::uint8_t> y(std::size_t(f.width) * f.height), cb(std::size_t(cw) * ch), cr(std::size_t(cw) * ch);
    const pixels::Plane yp{y.data(), f.width}, up{cb.data(), cw}, vp{cr.data(), cw};
    const Layout source = {false, f.full_range, true, 1, 1};
    const std::array<pixels::Plane, 3> staged{yp, up, vp};
    write_ycbcr(f, source, staged);
    pixels::Image staged_image;
    staged_image.format = f.full_range ? four("f420") : four("y420");
    staged_image.width = f.width;
    staged_image.height = f.height;
    staged_image.planes.assign(staged.begin(), staged.end());
    staged_image.matrix = target.matrix;
    const auto rgba = pixels::to_rgba(staged_image);
    pixels::from_rgba(rgba, target);
    return true;
}

} // namespace alchemy::gui::completion_kit::media::video_frames
