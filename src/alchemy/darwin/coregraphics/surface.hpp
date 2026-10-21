#pragma once

// The drawing surface of the Quartz 2D layer, over the host's 2D surfaces
// (host::gpu::Surface; Direct2D on win32), and the pixel-rectangle and
// geometry helpers of the drawing pipeline (context.cpp).
//
// A Surface's pixels are plain memory between draws, so a bitmap context
// copies the guest's pixels in before a draw and out after it (context.cpp,
// "flush semantics").

#include "alchemy/darwin/coregraphics/host_gpu.hpp"
#include "alchemy/darwin/coregraphics/q2d_internal.hpp"

#include <memory>
#include <string_view>

namespace alchemy::gui::quartz2d {

// Integer pixel rectangle [x0, x1) x [y0, y1) in device space.
using PixelRect = host::gpu::PixelRect;
[[nodiscard]] PixelRect pixel_bounds(const Rect& device, std::uint32_t width, std::uint32_t height);
[[nodiscard]] PixelRect intersect(const PixelRect& a, const PixelRect& b);
[[nodiscard]] PixelRect unite(const PixelRect& a, const PixelRect& b);

// Throws "Alchemy Quartz: <what> failed (<host error>)" for a failed host call.
void check(const host::Status& status, std::string_view what);

class Surface {
public:
    Surface(std::uint32_t width, std::uint32_t height);
    [[nodiscard]] std::uint32_t width() const { return surface_->width(); }
    [[nodiscard]] std::uint32_t height() const { return surface_->height(); }
    // The drawing interface between begin and end.
    [[nodiscard]] host::gpu::Surface& canvas() { return *surface_; }
    // Whether the copy and plus-lighter blends draw directly (a device context).
    [[nodiscard]] bool blend_modes() const { return surface_->blend_modes(); }
    void begin();
    void end();
    // Pixel access between draws.
    using Lock = host::gpu::Surface::Lock;
    [[nodiscard]] Lock lock(const PixelRect& rect, bool write);
    void clear(const PixelRect& rect);   // transparent black
    [[nodiscard]] Pbgra read(const PixelRect& rect);
    void write(const PixelRect& rect, const Pbgra& pixels);

private:
    std::unique_ptr<host::gpu::Surface> surface_;
};

[[nodiscard]] inline host::gpu::BgraView view(const Pbgra& pixels) { return {pixels.width, pixels.height, pixels.pixels.data()}; }
[[nodiscard]] inline host::gpu::Box to_box(const Rect& r) {
    return {r.origin.x, r.origin.y, r.origin.x + r.size.width, r.origin.y + r.size.height};
}
[[nodiscard]] inline host::gpu::Box to_box(const PixelRect& r) {
    return {static_cast<double>(r.x0), static_cast<double>(r.y0), static_cast<double>(r.x1), static_cast<double>(r.y1)};
}
[[nodiscard]] inline host::gpu::Color to_color(const Rgba& c, double opacity = 1) { return {c.r, c.g, c.b, c.a * opacity}; }

// Path geometry in the coordinates of `path` after `transform`.
[[nodiscard]] host::gpu::Geometry make_geometry(const PathData& path, const Affine& transform, bool even_odd, bool filled = true);
// The host stroke style for CGLineCap / CGLineJoin and a miter limit, with
// `dashes` (user lengths) for a stroke of `width`.
[[nodiscard]] host::gpu::StrokeStyle stroke_style(std::uint32_t cap, std::uint32_t join, double miter_limit,
                                                  std::span<const double> dashes = {}, double dash_phase = 0, double width = 1);

} // namespace alchemy::gui::quartz2d
