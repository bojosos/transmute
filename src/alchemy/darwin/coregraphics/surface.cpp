#include "alchemy/darwin/coregraphics/surface.hpp"

#include <algorithm>
#include <cstring>

// The Quartz 2D surface over host::gpu::Surface, pixel rectangles and the
// path -> geometry and stroke-style conversions.
namespace alchemy::gui::quartz2d {

void check(const host::Status& status, std::string_view what) {
    if (status) return;
    fail(std::string(what) + " failed (" + std::string(host::name(status.error())) + ")");
}

PixelRect pixel_bounds(const Rect& device, std::uint32_t width, std::uint32_t height) {
    if (rect_is_null(device)) return {};
    const auto s = standardize(device);
    const auto clamp = [](double value, double hi) { return static_cast<std::int32_t>(std::clamp(value, 0.0, hi)); };
    PixelRect r{clamp(std::floor(s.origin.x), width), clamp(std::floor(s.origin.y), height),
                clamp(std::ceil(s.origin.x + s.size.width), width), clamp(std::ceil(s.origin.y + s.size.height), height)};
    if (r.empty()) return {};
    return r;
}
PixelRect intersect(const PixelRect& a, const PixelRect& b) {
    PixelRect r{std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
    return r.empty() ? PixelRect{} : r;
}
PixelRect unite(const PixelRect& a, const PixelRect& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    return {std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
}

// ---- Surface --------------------------------------------------------------------------------------
Surface::Surface(std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0) fail("empty drawing surface");
    auto created = host::gpu::Surface::create(width, height);
    if (!created) fail("creating a drawing surface failed (" + std::string(host::name(created.error())) + ")");
    surface_ = std::move(*created);
}

void Surface::begin() {
    const auto begun = surface_->begin();
    if (!begun && begun.error() == host::Error::busy) fail("nested drawing on one surface");
    check(begun, "beginning a drawing");
}
void Surface::end() { check(surface_->end(), "Direct2D drawing"); }

Surface::Lock Surface::lock(const PixelRect& rect, bool write) {
    auto locked = surface_->lock(rect, write);
    if (!locked && locked.error() == host::Error::busy) fail("surface pixels accessed while drawing");
    check(locked ? host::Status{} : host::Status{locked.error()}, "locking surface pixels");
    return std::move(*locked);
}
void Surface::clear(const PixelRect& rect) {
    if (rect.empty()) return;
    const auto locked = lock(rect, true);
    for (auto y = rect.y0; y < rect.y1; ++y) std::memset(locked.at(rect.x0, y, rect), 0, static_cast<std::size_t>(rect.width()) * 4);
}
Pbgra Surface::read(const PixelRect& rect) {
    Pbgra out;
    out.width = rect.width();
    out.height = rect.height();
    out.pixels.resize(static_cast<std::size_t>(out.width) * out.height * 4);
    if (rect.empty()) return out;
    const auto locked = lock(rect, false);
    for (auto y = rect.y0; y < rect.y1; ++y)
        std::memcpy(out.row(static_cast<std::uint32_t>(y - rect.y0)), locked.at(rect.x0, y, rect), static_cast<std::size_t>(out.width) * 4);
    return out;
}
void Surface::write(const PixelRect& rect, const Pbgra& pixels) {
    if (rect.empty()) return;
    const auto locked = lock(rect, true);
    for (auto y = rect.y0; y < rect.y1; ++y)
        std::memcpy(locked.at(rect.x0, y, rect), pixels.row(static_cast<std::uint32_t>(y - rect.y0)), static_cast<std::size_t>(rect.width()) * 4);
}

// ---- geometry ------------------------------------------------------------------------------------------
host::gpu::Geometry make_geometry(const PathData& path, const Affine& transform, bool even_odd, bool filled) {
    auto geometry = host::gpu::Geometry::path(to_host(path, transform), even_odd, filled);
    if (!geometry) fail("creating a path geometry failed (" + std::string(host::name(geometry.error())) + ")");
    return std::move(*geometry);
}

host::gpu::StrokeStyle stroke_style(std::uint32_t cap, std::uint32_t join, double miter_limit, std::span<const double> dashes, double dash_phase,
                                    double width) {
    using Style = host::gpu::StrokeStyle;
    Style style;
    style.cap = cap == 1 ? Style::Cap::round : cap == 2 ? Style::Cap::square : Style::Cap::flat;
    style.join = join == 1 ? Style::Join::round : join == 2 ? Style::Join::bevel : Style::Join::miter_or_bevel;
    // CoreGraphics' limit is the miter length per stroke width; Direct2D's per half width.
    style.miter_limit = static_cast<float>(std::max(1.0, miter_limit * 2));
    if (!dashes.empty()) {
        double total{};
        for (const auto value : dashes) total += std::max(0.0, value);
        if (total > 0) {
            const auto unit = width > 0 ? width : 1.0;
            for (const auto value : dashes) style.dashes.push_back(static_cast<float>(std::max(0.0, value) / unit));
            if (style.dashes.size() % 2) { const auto copy = style.dashes; style.dashes.insert(style.dashes.end(), copy.begin(), copy.end()); }
            style.dash_offset = static_cast<float>(dash_phase / unit);
        }
    }
    return style;
}

} // namespace alchemy::gui::quartz2d
