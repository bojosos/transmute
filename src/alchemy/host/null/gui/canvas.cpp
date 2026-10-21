// Null host backend, area "gpu": 2D drawing. Every call fails with
// Error::unsupported (include/alchemy/host/host.hpp); no Surface or
// Geometry can be created.
#include "alchemy/host/gpu.hpp"

namespace alchemy::host::gpu {

struct Geometry::Impl {};
struct Surface::Impl {};

Result<Geometry> Geometry::path(std::span<const PathElement>, bool, bool) { return Error::unsupported; }
Result<std::vector<PathElement>> Geometry::widened(double, const StrokeStyle&, const Matrix&) const { return Error::unsupported; }
Result<bool> Geometry::stroke_contains(Point, double) const { return Error::unsupported; }

Result<std::unique_ptr<Surface>> Surface::create(std::uint32_t, std::uint32_t) { return Error::unsupported; }
Surface::Surface(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Surface::~Surface() = default;
std::uint32_t Surface::width() const noexcept { return 0; }
std::uint32_t Surface::height() const noexcept { return 0; }
bool Surface::blend_modes() const noexcept { return false; }
Status Surface::begin() { return Error::unsupported; }
Status Surface::end() { return Error::unsupported; }
Result<Surface::Lock> Surface::lock(const PixelRect&, bool) { return Error::unsupported; }
void Surface::set_transform(const Matrix&) {}
void Surface::set_antialias(bool) {}
void Surface::set_text_antialias(bool) {}
void Surface::set_blend(Blend) {}
void Surface::push_axis_clip(const Box&, bool) {}
void Surface::pop_axis_clip() {}
Status Surface::push_clip(const Geometry&, bool) { return Error::unsupported; }
void Surface::pop_clip() {}
Status Surface::fill_rect(const Box&, const Brush&) { return Error::unsupported; }
Status Surface::fill(const Geometry&, const Brush&) { return Error::unsupported; }
Status Surface::stroke(const Geometry&, const Brush&, double, const StrokeStyle&) { return Error::unsupported; }
Status Surface::draw_bitmap(const BgraView&, const Box&, double, Interpolation) { return Error::unsupported; }
Status Surface::draw_glyphs(const Font&, double, std::span<const std::uint16_t>, std::span<const Point>, const Brush&) {
    return Error::unsupported;
}

} // namespace alchemy::host::gpu

namespace alchemy::host::gpu {

Result<RasterMetrics> raster_metrics(const RasterFont&) { return Error::unsupported; }
Result<double> raster_width(const RasterFont&, std::u16string_view) { return Error::unsupported; }
Result<std::vector<double>> raster_prefix_widths(const RasterFont&, std::u16string_view) { return Error::unsupported; }
Result<std::vector<float>> raster_coverage(const RasterFont&, std::u16string_view, int, int, int, int) { return Error::unsupported; }

} // namespace alchemy::host::gpu
