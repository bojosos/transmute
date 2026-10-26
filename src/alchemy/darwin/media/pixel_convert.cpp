#include "alchemy/darwin/media/pixel_convert.hpp"

#include <algorithm>
#include <cmath>

namespace alchemy::gui::completion_kit::media::pixels {
namespace {

constexpr std::uint32_t four(const char (&code)[5]) {
    return (std::uint32_t(std::uint8_t(code[0])) << 24) | (std::uint32_t(std::uint8_t(code[1])) << 16) |
           (std::uint32_t(std::uint8_t(code[2])) << 8) | std::uint32_t(std::uint8_t(code[3]));
}

constexpr std::uint32_t argb = 0x00000020, rgb24 = 0x00000018;

enum class Kind { rgb, gray, packed422, nv12, p010, i420, unknown };
struct Layout {
    Kind kind{Kind::unknown};
    bool full_range{};
    int order[4]{};                    // RGB formats: the byte of R, G, B, A (-1: none)
    unsigned bytes{};                  // RGB formats: bytes per pixel
    bool cb_first{};                   // 422: Cb Y0 Cr Y1 (2vuy) or Y0 Cb Y1 Cr (yuvs)
};

Layout layout(std::uint32_t format) {
    if (format == four("BGRA")) return {Kind::rgb, true, {2, 1, 0, 3}, 4};
    if (format == argb) return {Kind::rgb, true, {1, 2, 3, 0}, 4};
    if (format == four("RGBA")) return {Kind::rgb, true, {0, 1, 2, 3}, 4};
    if (format == four("ABGR")) return {Kind::rgb, true, {3, 2, 1, 0}, 4};
    if (format == rgb24) return {Kind::rgb, true, {0, 1, 2, -1}, 3};
    if (format == four("24BG")) return {Kind::rgb, true, {2, 1, 0, -1}, 3};
    if (format == four("L008")) return {Kind::gray, true, {}, 1};
    if (format == four("2vuy")) return {Kind::packed422, false, {}, 0, true};
    if (format == four("yuvs")) return {Kind::packed422, false, {}, 0, false};
    if (format == four("420v")) return {Kind::nv12, false};
    if (format == four("420f")) return {Kind::nv12, true};
    if (format == four("x420")) return {Kind::p010, false};
    if (format == four("xf20")) return {Kind::p010, true};
    if (format == four("y420")) return {Kind::i420, false};
    if (format == four("f420")) return {Kind::i420, true};
    return {};
}

struct Coefficients { double kr, kb; };
Coefficients coefficients(Matrix m) {
    switch (m) {
    case Matrix::bt601: return {0.299, 0.114};
    case Matrix::bt2020: return {0.2627, 0.0593};
    default: return {0.2126, 0.0722};
    }
}

// Normalized Y' in [0, 1] and Cb, Cr in [-0.5, 0.5] from code values of `bits` bits.
double luma_in(double code, unsigned bits, bool full) {
    const double scale = (1 << (bits - 8));
    return full ? code / ((1 << bits) - 1) : (code - 16 * scale) / (219 * scale);
}
double chroma_in(double code, unsigned bits, bool full) {
    const double scale = (1 << (bits - 8));
    return full ? (code - 128 * scale) / ((1 << bits) - 1) : (code - 128 * scale) / (224 * scale);
}
double luma_out(double y, unsigned bits, bool full) {
    const double scale = (1 << (bits - 8));
    const double code = full ? y * ((1 << bits) - 1) : 16 * scale + y * 219 * scale;
    return std::clamp(std::round(code), 0.0, static_cast<double>((1 << bits) - 1));
}
double chroma_out(double c, unsigned bits, bool full) {
    const double scale = (1 << (bits - 8));
    const double code = 128 * scale + c * (full ? (1 << bits) - 1 : 224 * scale);
    return std::clamp(std::round(code), 0.0, static_cast<double>((1 << bits) - 1));
}

void to_rgb(Matrix m, double y, double cb, double cr, float* out) {
    const auto [kr, kb] = coefficients(m);
    const double kg = 1 - kr - kb;
    const double r = y + 2 * (1 - kr) * cr, b = y + 2 * (1 - kb) * cb;
    const double g = (y - kr * r - kb * b) / kg;
    out[0] = static_cast<float>(std::clamp(r, 0.0, 1.0));
    out[1] = static_cast<float>(std::clamp(g, 0.0, 1.0));
    out[2] = static_cast<float>(std::clamp(b, 0.0, 1.0));
    out[3] = 1;
}
void to_ycc(Matrix m, const float* rgb, double& y, double& cb, double& cr) {
    const auto [kr, kb] = coefficients(m);
    const double kg = 1 - kr - kb;
    y = kr * rgb[0] + kg * rgb[1] + kb * rgb[2];
    cb = (rgb[2] - y) / (2 * (1 - kb));
    cr = (rgb[0] - y) / (2 * (1 - kr));
}

std::uint16_t read16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
void write16(std::uint8_t* p, std::uint16_t v) { p[0] = static_cast<std::uint8_t>(v); p[1] = static_cast<std::uint8_t>(v >> 8); }

} // namespace

unsigned plane_count(std::uint32_t format) {
    switch (layout(format).kind) {
    case Kind::rgb: case Kind::gray: case Kind::packed422: return 1;
    case Kind::nv12: case Kind::p010: return 2;
    case Kind::i420: return 3;
    default: return 0;
    }
}

Matrix default_matrix(std::uint32_t, std::uint32_t height) { return height <= 576 ? Matrix::bt601 : Matrix::bt709; }

std::vector<float> to_rgba(const Image& image) {
    const auto l = layout(image.format);
    const auto w = image.width, h = image.height;
    std::vector<float> out(static_cast<std::size_t>(w) * h * 4);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            float* px = out.data() + (static_cast<std::size_t>(y) * w + x) * 4;
            const auto row = [&](unsigned plane, std::uint32_t r) { return image.planes[plane].data + r * image.planes[plane].bytes_per_row; };
            switch (l.kind) {
            case Kind::rgb: {
                const auto* p = row(0, y) + x * l.bytes;
                for (int c = 0; c < 3; ++c) px[c] = p[l.order[c]] / 255.0f;
                px[3] = l.order[3] >= 0 ? p[l.order[3]] / 255.0f : 1.0f;
                break;
            }
            case Kind::gray: {
                const float v = row(0, y)[x] / 255.0f;
                px[0] = px[1] = px[2] = v;
                px[3] = 1;
                break;
            }
            case Kind::packed422: {
                const auto* p = row(0, y) + (x / 2) * 4;
                const int cb = l.cb_first ? p[0] : p[1], cr = l.cb_first ? p[2] : p[3];
                const int luma = l.cb_first ? p[(x & 1) ? 3 : 1] : p[(x & 1) ? 2 : 0];
                to_rgb(image.matrix, luma_in(luma, 8, l.full_range), chroma_in(cb, 8, l.full_range), chroma_in(cr, 8, l.full_range), px);
                break;
            }
            case Kind::nv12: {
                const auto* c = row(1, y / 2) + (x / 2) * 2;
                to_rgb(image.matrix, luma_in(row(0, y)[x], 8, l.full_range), chroma_in(c[0], 8, l.full_range), chroma_in(c[1], 8, l.full_range), px);
                break;
            }
            case Kind::p010: {
                const auto* c = row(1, y / 2) + (x / 2) * 4;
                to_rgb(image.matrix, luma_in(read16(row(0, y) + x * 2) >> 6, 10, l.full_range), chroma_in(read16(c) >> 6, 10, l.full_range),
                       chroma_in(read16(c + 2) >> 6, 10, l.full_range), px);
                break;
            }
            case Kind::i420:
                to_rgb(image.matrix, luma_in(row(0, y)[x], 8, l.full_range), chroma_in(row(1, y / 2)[x / 2], 8, l.full_range),
                       chroma_in(row(2, y / 2)[x / 2], 8, l.full_range), px);
                break;
            default:
                break;
            }
        }
    return out;
}

void from_rgba(std::span<const float> rgba, Image& image) {
    const auto l = layout(image.format);
    const auto w = image.width, h = image.height;
    const auto pixel = [&](std::uint32_t x, std::uint32_t y) { return rgba.data() + (static_cast<std::size_t>(y) * w + x) * 4; };
    const auto row = [&](unsigned plane, std::uint32_t r) { return image.planes[plane].data + r * image.planes[plane].bytes_per_row; };
    const auto byte = [](float v) { return static_cast<std::uint8_t>(std::clamp(std::lround(v * 255.0f), 0L, 255L)); };
    // The average Cb/Cr of the pixels (x0..x0+dx, y0..y0+dy) inside the image.
    const auto chroma = [&](std::uint32_t x0, std::uint32_t y0, std::uint32_t dx, std::uint32_t dy, double& cb, double& cr) {
        cb = cr = 0;
        int n = 0;
        for (std::uint32_t y = y0; y < std::min(y0 + dy, h); ++y)
            for (std::uint32_t x = x0; x < std::min(x0 + dx, w); ++x) {
                double luma, b, r;
                to_ycc(image.matrix, pixel(x, y), luma, b, r);
                cb += b;
                cr += r;
                ++n;
            }
        if (n) { cb /= n; cr /= n; }
    };
    const auto luma_of = [&](std::uint32_t x, std::uint32_t y) {
        double luma, b, r;
        to_ycc(image.matrix, pixel(x, y), luma, b, r);
        return luma;
    };
    switch (l.kind) {
    case Kind::rgb:
        for (std::uint32_t y = 0; y < h; ++y)
            for (std::uint32_t x = 0; x < w; ++x) {
                auto* p = row(0, y) + x * l.bytes;
                const auto* px = pixel(x, y);
                for (int c = 0; c < 4; ++c)
                    if (l.order[c] >= 0) p[l.order[c]] = byte(px[c]);
            }
        break;
    case Kind::gray:
        for (std::uint32_t y = 0; y < h; ++y)
            for (std::uint32_t x = 0; x < w; ++x) row(0, y)[x] = byte(static_cast<float>(luma_of(x, y)));
        break;
    case Kind::packed422:
        for (std::uint32_t y = 0; y < h; ++y)
            for (std::uint32_t x = 0; x < w; x += 2) {
                double cb, cr;
                chroma(x, y, 2, 1, cb, cr);
                auto* p = row(0, y) + (x / 2) * 4;
                const auto y0 = static_cast<std::uint8_t>(luma_out(luma_of(x, y), 8, l.full_range));
                const auto y1 = static_cast<std::uint8_t>(luma_out(luma_of(std::min(x + 1, w - 1), y), 8, l.full_range));
                const auto b = static_cast<std::uint8_t>(chroma_out(cb, 8, l.full_range)), r = static_cast<std::uint8_t>(chroma_out(cr, 8, l.full_range));
                if (l.cb_first) { p[0] = b; p[1] = y0; p[2] = r; p[3] = y1; }
                else { p[0] = y0; p[1] = b; p[2] = y1; p[3] = r; }
            }
        break;
    case Kind::nv12: case Kind::p010: case Kind::i420: {
        const bool ten = l.kind == Kind::p010;
        for (std::uint32_t y = 0; y < h; ++y)
            for (std::uint32_t x = 0; x < w; ++x) {
                const auto code = luma_out(luma_of(x, y), ten ? 10 : 8, l.full_range);
                if (ten) write16(row(0, y) + x * 2, static_cast<std::uint16_t>(static_cast<unsigned>(code) << 6));
                else row(0, y)[x] = static_cast<std::uint8_t>(code);
            }
        for (std::uint32_t y = 0; y < (h + 1) / 2; ++y)
            for (std::uint32_t x = 0; x < (w + 1) / 2; ++x) {
                double cb, cr;
                chroma(x * 2, y * 2, 2, 2, cb, cr);
                const auto b = chroma_out(cb, ten ? 10 : 8, l.full_range), r = chroma_out(cr, ten ? 10 : 8, l.full_range);
                if (l.kind == Kind::nv12) {
                    auto* c = row(1, y) + x * 2;
                    c[0] = static_cast<std::uint8_t>(b);
                    c[1] = static_cast<std::uint8_t>(r);
                } else if (ten) {
                    auto* c = row(1, y) + x * 4;
                    write16(c, static_cast<std::uint16_t>(static_cast<unsigned>(b) << 6));
                    write16(c + 2, static_cast<std::uint16_t>(static_cast<unsigned>(r) << 6));
                } else {
                    row(1, y)[x] = static_cast<std::uint8_t>(b);
                    row(2, y)[x] = static_cast<std::uint8_t>(r);
                }
            }
        break;
    }
    default:
        break;
    }
}

std::vector<float> scale(std::span<const float> rgba, std::uint32_t width, std::uint32_t height, std::uint32_t to_width, std::uint32_t to_height) {
    if (width == to_width && height == to_height) return {rgba.begin(), rgba.end()};
    std::vector<float> out(static_cast<std::size_t>(to_width) * to_height * 4);
    const double sx = static_cast<double>(width) / to_width, sy = static_cast<double>(height) / to_height;
    for (std::uint32_t y = 0; y < to_height; ++y)
        for (std::uint32_t x = 0; x < to_width; ++x) {
            float* px = out.data() + (static_cast<std::size_t>(y) * to_width + x) * 4;
            if (sx > 2 || sy > 2) {
                // Area average over the source rectangle the destination pixel covers.
                const auto x0 = static_cast<std::uint32_t>(x * sx), x1 = std::max(x0 + 1, static_cast<std::uint32_t>((x + 1) * sx));
                const auto y0 = static_cast<std::uint32_t>(y * sy), y1 = std::max(y0 + 1, static_cast<std::uint32_t>((y + 1) * sy));
                double sum[4]{};
                int n = 0;
                for (auto yy = y0; yy < std::min(y1, height); ++yy)
                    for (auto xx = x0; xx < std::min(x1, width); ++xx) {
                        const float* s = rgba.data() + (static_cast<std::size_t>(yy) * width + xx) * 4;
                        for (int c = 0; c < 4; ++c) sum[c] += s[c];
                        ++n;
                    }
                for (int c = 0; c < 4; ++c) px[c] = n ? static_cast<float>(sum[c] / n) : 0.0f;
                continue;
            }
            const double fx = std::clamp((x + 0.5) * sx - 0.5, 0.0, width - 1.0), fy = std::clamp((y + 0.5) * sy - 0.5, 0.0, height - 1.0);
            const auto x0 = static_cast<std::uint32_t>(fx), y0 = static_cast<std::uint32_t>(fy);
            const auto x1 = std::min(x0 + 1, width - 1), y1 = std::min(y0 + 1, height - 1);
            const double ax = fx - x0, ay = fy - y0;
            for (int c = 0; c < 4; ++c) {
                const auto at = [&](std::uint32_t xx, std::uint32_t yy) { return rgba[(static_cast<std::size_t>(yy) * width + xx) * 4 + c]; };
                const double top = at(x0, y0) * (1 - ax) + at(x1, y0) * ax, bottom = at(x0, y1) * (1 - ax) + at(x1, y1) * ax;
                px[c] = static_cast<float>(top * (1 - ay) + bottom * ay);
            }
        }
    return out;
}

} // namespace alchemy::gui::completion_kit::media::pixels
