#include "alchemy/darwin/appkit/drawing.hpp"
#include "alchemy/gui/drawn_text.hpp"
#include "alchemy/host/gpu.hpp"

#include "transmute/a64.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <numbers>
#include <stdexcept>
#include <tuple>


namespace alchemy::gui::appkit::drawing {
namespace {
[[noreturn]] void fail(const std::string& why) { throw std::runtime_error("Alchemy AppKit drawing: " + why); }

std::uint32_t pack(double r, double g, double b, double a) {
    const auto byte = [](double v) { return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255)); };
    return (byte(a) << 24) | (byte(r) << 16) | (byte(g) << 8) | byte(b);
}
// Premultiplied source over premultiplied destination, 8-bit.
std::uint32_t over(std::uint32_t src, std::uint32_t dst) {
    const auto sa = src >> 24;
    if (sa == 255) return src;
    if (sa == 0) return dst;
    const auto inv = 255 - sa;
    std::uint32_t result{};
    for (int shift = 0; shift < 32; shift += 8) {
        const auto s = (src >> shift) & 0xff, d = (dst >> shift) & 0xff;
        const auto value = std::min<std::uint32_t>(255, s + (d * inv + 127) / 255);
        result |= value << shift;
    }
    return result;
}
std::uint32_t premultiplied(Color c, double coverage) {
    const auto a = std::clamp(c.a * coverage, 0.0, 1.0);
    return pack(c.r * a, c.g * a, c.b * a, a);
}
double cross(Point a, Point b, Point c) { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); }
double signed_area(const std::vector<Point>& polygon) {
    double area{};
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const auto& p = polygon[i];
        const auto& q = polygon[(i + 1) % polygon.size()];
        area += p.x * q.y - q.x * p.y;
    }
    return area / 2;
}
void orient(std::vector<Point>& polygon) {
    if (signed_area(polygon) < 0) std::reverse(polygon.begin(), polygon.end());
}
double ctm_scale(const Affine& t) {
    return std::sqrt(std::abs(t.a * t.d - t.b * t.c));
}
} // namespace

// ---- geometry ----------------------------------------------------------------------
std::optional<Affine> Affine::inverted() const {
    const auto det = a * d - b * c;
    if (std::abs(det) < 1e-12) return std::nullopt;
    const Affine inverse{d / det, -b / det, -c / det, a / det, 0, 0};
    const auto origin = inverse.apply({tx, ty});
    return Affine{inverse.a, inverse.b, inverse.c, inverse.d, -origin.x, -origin.y};
}
PixelRect PixelRect::intersect(PixelRect o) const {
    return {std::max(x0, o.x0), std::max(y0, o.y0), std::min(x1, o.x1), std::min(y1, o.y1)};
}
PixelRect PixelRect::unite(PixelRect o) const {
    if (empty()) return o;
    if (o.empty()) return *this;
    return {std::min(x0, o.x0), std::min(y0, o.y0), std::max(x1, o.x1), std::max(y1, o.y1)};
}
Rect bounding_rect(const Affine& t, Rect r) {
    const std::array<Point, 4> corners{t.apply(r.origin), t.apply({r.origin.x + r.size.width, r.origin.y}),
                                       t.apply({r.origin.x, r.origin.y + r.size.height}),
                                       t.apply({r.origin.x + r.size.width, r.origin.y + r.size.height})};
    double x0 = corners[0].x, x1 = x0, y0 = corners[0].y, y1 = y0;
    for (const auto& p : corners) { x0 = std::min(x0, p.x); x1 = std::max(x1, p.x); y0 = std::min(y0, p.y); y1 = std::max(y1, p.y); }
    return {{x0, y0}, {x1 - x0, y1 - y0}};
}
PixelRect device_bounds(const Affine& ctm, Rect r) {
    const auto box = bounding_rect(ctm, r);
    // Snap edges within a hair of a pixel boundary (no bleeding from
    // floating-point noise into the neighbouring pixel).
    const auto low = [](double v) { return static_cast<int>(std::floor(v + 1e-6)); };
    const auto high = [](double v) { return static_cast<int>(std::ceil(v - 1e-6)); };
    return {low(box.origin.x), low(box.origin.y), high(box.origin.x + box.size.width), high(box.origin.y + box.size.height)};
}
Rect intersect(Rect a, Rect b) {
    const auto x0 = std::max(a.origin.x, b.origin.x), y0 = std::max(a.origin.y, b.origin.y);
    const auto x1 = std::min(a.origin.x + a.size.width, b.origin.x + b.size.width);
    const auto y1 = std::min(a.origin.y + a.size.height, b.origin.y + b.size.height);
    if (x1 <= x0 || y1 <= y0) return {{x0, y0}, {0, 0}};
    return {{x0, y0}, {x1 - x0, y1 - y0}};
}
Rect unite(Rect a, Rect b) {
    if (a.size.width <= 0 || a.size.height <= 0) return b;
    if (b.size.width <= 0 || b.size.height <= 0) return a;
    const auto x0 = std::min(a.origin.x, b.origin.x), y0 = std::min(a.origin.y, b.origin.y);
    const auto x1 = std::max(a.origin.x + a.size.width, b.origin.x + b.size.width);
    const auto y1 = std::max(a.origin.y + a.size.height, b.origin.y + b.size.height);
    return {{x0, y0}, {x1 - x0, y1 - y0}};
}

// ---- pixel storage -------------------------------------------------------------------
HostBuffer::HostBuffer(std::uint32_t width, std::uint32_t height, std::uint32_t fill)
    : width_(width), height_(height), pixels_(static_cast<std::size_t>(width) * height, fill) {}
void HostBuffer::read(std::uint32_t x, std::uint32_t y, std::span<std::uint32_t> out) const {
    if (y >= height_ || x + out.size() > width_) fail("host buffer read outside");
    std::memcpy(out.data(), pixels_.data() + static_cast<std::size_t>(y) * width_ + x, out.size_bytes());
}
void HostBuffer::write(std::uint32_t x, std::uint32_t y, std::span<const std::uint32_t> in) {
    if (y >= height_ || x + in.size() > width_) fail("host buffer write outside");
    std::memcpy(pixels_.data() + static_cast<std::size_t>(y) * width_ + x, in.data(), in.size_bytes());
}
void GuestBuffer::read(std::uint32_t x, std::uint32_t y, std::span<std::uint32_t> out) const {
    if (y >= height_ || x + out.size() > width_) fail("backing store read outside");
    memory_.read_bytes(pixels_ + y * stride_ + x * 4ULL,
                       std::span(reinterpret_cast<std::uint8_t*>(out.data()), out.size_bytes()));
    if (rgba_) for (auto& v : out) v = (v & 0xff00ff00u) | ((v & 0xffu) << 16) | ((v >> 16) & 0xffu);   // R and B swap
}
void GuestBuffer::write(std::uint32_t x, std::uint32_t y, std::span<const std::uint32_t> in) {
    if (y >= height_ || x + in.size() > width_) fail("backing store write outside");
    if (rgba_) {
        std::vector<std::uint32_t> swapped(in.begin(), in.end());
        for (auto& v : swapped) v = (v & 0xff00ff00u) | ((v & 0xffu) << 16) | ((v >> 16) & 0xffu);
        memory_.write_bytes(pixels_ + y * stride_ + x * 4ULL,
                            std::span(reinterpret_cast<const std::uint8_t*>(swapped.data()), swapped.size() * 4));
        return;
    }
    memory_.write_bytes(pixels_ + y * stride_ + x * 4ULL,
                        std::span(reinterpret_cast<const std::uint8_t*>(in.data()), in.size_bytes()));
}

// ---- paths ------------------------------------------------------------------------------
void Path::move_to(Point p) { elements_.push_back({Op::move, {p}}); current_ = start_ = p; }
void Path::line_to(Point p) {
    if (!current_) { move_to(p); return; }
    elements_.push_back({Op::line, {p}});
    current_ = p;
}
void Path::curve_to(Point c1, Point c2, Point end) {
    if (!current_) move_to(c1);
    elements_.push_back({Op::curve, {c1, c2, end}});
    current_ = end;
}
void Path::close() {
    if (!current_) return;
    elements_.push_back({Op::close, {}});
    current_ = start_;
}
void Path::add_rect(Rect r) {
    move_to(r.origin);
    line_to({r.origin.x + r.size.width, r.origin.y});
    line_to({r.origin.x + r.size.width, r.origin.y + r.size.height});
    line_to({r.origin.x, r.origin.y + r.size.height});
    close();
}
void Path::add_rounded_rect(Rect r, double rx, double ry) {
    rx = std::clamp(rx, 0.0, r.size.width / 2);
    ry = std::clamp(ry, 0.0, r.size.height / 2);
    if (rx <= 0 || ry <= 0) { add_rect(r); return; }
    constexpr double k = 0.5522847498;   // cubic circle approximation
    const auto x0 = r.origin.x, y0 = r.origin.y, x1 = x0 + r.size.width, y1 = y0 + r.size.height;
    move_to({x0 + rx, y0});
    line_to({x1 - rx, y0});
    curve_to({x1 - rx + rx * k, y0}, {x1, y0 + ry - ry * k}, {x1, y0 + ry});
    line_to({x1, y1 - ry});
    curve_to({x1, y1 - ry + ry * k}, {x1 - rx + rx * k, y1}, {x1 - rx, y1});
    line_to({x0 + rx, y1});
    curve_to({x0 + rx - rx * k, y1}, {x0, y1 - ry + ry * k}, {x0, y1 - ry});
    line_to({x0, y0 + ry});
    curve_to({x0, y0 + ry - ry * k}, {x0 + rx - rx * k, y0}, {x0 + rx, y0});
    close();
}
void Path::add_oval(Rect r) { add_rounded_rect(r, r.size.width / 2, r.size.height / 2); }
void Path::add_arc(Point center, double radius, double start, double end, bool clockwise) {
    auto from = start * std::numbers::pi / 180, to = end * std::numbers::pi / 180;
    if (clockwise) { while (to > from) to -= 2 * std::numbers::pi; }
    else { while (to < from) to += 2 * std::numbers::pi; }
    const int steps = std::max(1, static_cast<int>(std::ceil(std::abs(to - from) / (std::numbers::pi / 2))));
    const auto step = (to - from) / steps;
    const Point first{center.x + radius * std::cos(from), center.y + radius * std::sin(from)};
    if (current_) line_to(first); else move_to(first);
    for (int i = 0; i < steps; ++i) {
        const auto a0 = from + step * i, a1 = a0 + step;
        const auto alpha = 4.0 / 3.0 * std::tan(step / 4);
        const Point p0{center.x + radius * std::cos(a0), center.y + radius * std::sin(a0)};
        const Point p3{center.x + radius * std::cos(a1), center.y + radius * std::sin(a1)};
        curve_to({p0.x - alpha * radius * std::sin(a0), p0.y + alpha * radius * std::cos(a0)},
                 {p3.x + alpha * radius * std::sin(a1), p3.y - alpha * radius * std::cos(a1)}, p3);
    }
}
void Path::append(const Path& other) {
    elements_.insert(elements_.end(), other.elements_.begin(), other.elements_.end());
    if (other.current_) { current_ = other.current_; start_ = other.start_; }
}
Rect Path::bounds() const {
    bool any{};
    double x0{}, y0{}, x1{}, y1{};
    for (const auto& element : elements_) {
        const int count = element.op == Op::curve ? 3 : element.op == Op::close ? 0 : 1;
        for (int i = 0; i < count; ++i) {
            const auto& p = element.points[i];
            if (!any) { x0 = x1 = p.x; y0 = y1 = p.y; any = true; continue; }
            x0 = std::min(x0, p.x); x1 = std::max(x1, p.x); y0 = std::min(y0, p.y); y1 = std::max(y1, p.y);
        }
    }
    return {{x0, y0}, {x1 - x0, y1 - y0}};
}
void Path::transform(const Affine& t) {
    for (auto& element : elements_) for (auto& p : element.points) p = t.apply(p);
    if (current_) current_ = t.apply(*current_);
    if (start_) start_ = t.apply(*start_);
}
std::vector<Path::Contour> Path::flatten(const Affine& ctm) const {
    std::vector<Contour> contours;
    Point last{};
    for (const auto& element : elements_) {
        switch (element.op) {
        case Op::move:
            contours.push_back({{ctm.apply(element.points[0])}, false});
            last = element.points[0];
            break;
        case Op::line:
            if (contours.empty()) contours.push_back({{ctm.apply(last)}, false});
            contours.back().points.push_back(ctm.apply(element.points[0]));
            last = element.points[0];
            break;
        case Op::curve: {
            if (contours.empty()) contours.push_back({{ctm.apply(last)}, false});
            const auto p0 = ctm.apply(last), p1 = ctm.apply(element.points[0]), p2 = ctm.apply(element.points[1]),
                       p3 = ctm.apply(element.points[2]);
            const auto length = std::hypot(p1.x - p0.x, p1.y - p0.y) + std::hypot(p2.x - p1.x, p2.y - p1.y) +
                                std::hypot(p3.x - p2.x, p3.y - p2.y);
            const int steps = std::clamp(static_cast<int>(std::ceil(std::sqrt(length) * 2)), 2, 256);
            for (int i = 1; i <= steps; ++i) {
                const double t = static_cast<double>(i) / steps, u = 1 - t;
                const auto a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
                contours.back().points.push_back({a * p0.x + b * p1.x + c * p2.x + d * p3.x, a * p0.y + b * p1.y + c * p2.y + d * p3.y});
            }
            last = element.points[2];
            break;
        }
        case Op::close:
            if (!contours.empty()) {
                contours.back().closed = true;
                const auto start = contours.back().points.front();
                // A subpath after close starts at the closed subpath's start.
                const auto inverse = ctm.inverted();
                if (inverse) last = inverse->apply(start);
            }
            break;
        }
    }
    return contours;
}

// ---- rasterization ---------------------------------------------------------------------
void rasterize(const std::vector<Path::Contour>& contours, Winding winding, PixelRect clip,
               const std::function<void(int, int, std::span<const float>)>& span) {
    struct Edge { double x0, y0, x1, y1; int direction; };
    std::vector<Edge> edges;
    double min_y = 1e300, max_y = -1e300, min_x = 1e300, max_x = -1e300;
    for (const auto& contour : contours) {
        const auto& points = contour.points;
        if (points.size() < 2) continue;
        for (std::size_t i = 0; i < points.size(); ++i) {
            const auto& p = points[i];
            const auto& q = points[(i + 1) % points.size()];   // fills close every contour
            min_x = std::min(min_x, p.x); max_x = std::max(max_x, p.x);
            min_y = std::min(min_y, p.y); max_y = std::max(max_y, p.y);
            if (p.y == q.y) continue;
            if (p.y < q.y) edges.push_back({p.x, p.y, q.x, q.y, 1});
            else edges.push_back({q.x, q.y, p.x, p.y, -1});
        }
    }
    if (edges.empty()) return;
    const PixelRect box{static_cast<int>(std::floor(min_x)), static_cast<int>(std::floor(min_y)),
                        static_cast<int>(std::ceil(max_x)) + 1, static_cast<int>(std::ceil(max_y)) + 1};
    const auto area = box.intersect(clip);
    if (area.empty()) return;
    std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) { return a.y0 < b.y0; });
    constexpr int samples = 5;
    const auto width = static_cast<std::size_t>(area.x1 - area.x0);
    std::vector<float> coverage(width);
    std::vector<std::pair<double, int>> crossings;
    for (int y = area.y0; y < area.y1; ++y) {
        std::fill(coverage.begin(), coverage.end(), 0.0f);
        bool any{};
        for (int s = 0; s < samples; ++s) {
            const double sy = y + (s + 0.5) / samples;
            crossings.clear();
            for (const auto& edge : edges) {
                if (edge.y0 > sy) break;
                if (sy >= edge.y1) continue;
                const auto x = edge.x0 + (sy - edge.y0) * (edge.x1 - edge.x0) / (edge.y1 - edge.y0);
                crossings.emplace_back(x, edge.direction);
            }
            if (crossings.size() < 2) continue;
            std::sort(crossings.begin(), crossings.end());
            int count = 0;
            for (std::size_t i = 0; i + 1 < crossings.size(); ++i) {
                count += crossings[i].second;
                const bool inside = winding == Winding::non_zero ? count != 0 : (std::abs(count) & 1) != 0;
                if (!inside) continue;
                auto xa = std::max(crossings[i].first, static_cast<double>(area.x0));
                auto xb = std::min(crossings[i + 1].first, static_cast<double>(area.x1));
                if (xb <= xa) continue;
                any = true;
                const auto weight = 1.0f / samples;
                const int ia = static_cast<int>(std::floor(xa)), ib = static_cast<int>(std::floor(xb));
                if (ia == ib) { coverage[ia - area.x0] += static_cast<float>((xb - xa) * weight); continue; }
                coverage[ia - area.x0] += static_cast<float>((ia + 1 - xa) * weight);
                for (int x = ia + 1; x < ib; ++x) coverage[x - area.x0] += weight;
                if (ib < area.x1) coverage[ib - area.x0] += static_cast<float>((xb - ib) * weight);
            }
        }
        if (!any) continue;
        // Emit runs of covered pixels.
        std::size_t start = 0;
        while (start < width) {
            while (start < width && coverage[start] <= 0.0f) ++start;
            if (start >= width) break;
            auto end = start;
            while (end < width && coverage[end] > 0.0f) ++end;
            span(y, area.x0 + static_cast<int>(start), std::span<const float>(coverage.data() + start, end - start));
            start = end;
        }
    }
}

// ---- canvas ---------------------------------------------------------------------------------
Canvas::Canvas(PixelBuffer& target) : target_(&target) {
    state.clip = {0, 0, static_cast<int>(target.width()), static_cast<int>(target.height())};
}
void Canvas::restore() {
    if (stack_.empty()) fail("graphics state restore without a save");
    state = stack_.back();
    stack_.pop_back();
}
void Canvas::clip_to_rect(Rect user) { state.clip = state.clip.intersect(device_bounds(state.ctm, user)); }

void Canvas::blend_span(int y, int x0, std::span<const float> coverage, Color color) {
    if (y < state.clip.y0 || y >= state.clip.y1) return;
    auto first = std::max(x0, state.clip.x0);
    const auto last = std::min(x0 + static_cast<int>(coverage.size()), state.clip.x1);
    if (last <= first) return;
    std::vector<std::uint32_t> row(static_cast<std::size_t>(last - first));
    target_->read(static_cast<std::uint32_t>(first), static_cast<std::uint32_t>(y), row);
    const auto effective = color.with_alpha(state.alpha);
    for (int x = first; x < last; ++x) {
        auto c = coverage[static_cast<std::size_t>(x - x0)];
        if (!state.antialias) c = c >= 0.5f ? 1.0f : 0.0f;
        if (c <= 0) continue;
        c = std::min(c, 1.0f);
        auto& pixel = row[static_cast<std::size_t>(x - first)];
        switch (state.blend) {
        case Blend::source_over: pixel = over(premultiplied(effective, c), pixel); break;
        case Blend::copy: {
            // Coverage-weighted replacement.
            const auto source = premultiplied(effective, 1);
            if (c >= 1) { pixel = source; break; }
            std::uint32_t mixed{};
            for (int shift = 0; shift < 32; shift += 8) {
                const auto s = (source >> shift) & 0xff, d = (pixel >> shift) & 0xff;
                mixed |= static_cast<std::uint32_t>(std::lround(s * c + d * (1 - c))) << shift;
            }
            pixel = mixed;
            break;
        }
        case Blend::clear: {
            std::uint32_t kept{};
            for (int shift = 0; shift < 32; shift += 8)
                kept |= static_cast<std::uint32_t>(std::lround(((pixel >> shift) & 0xff) * (1 - c))) << shift;
            pixel = kept;
            break;
        }
        }
    }
    target_->write(static_cast<std::uint32_t>(first), static_cast<std::uint32_t>(y), row);
}

void Canvas::blend_coverage(PixelRect area, std::span<const float> coverage, Color color) {
    const auto width = static_cast<std::size_t>(area.x1 - area.x0);
    for (int y = area.y0; y < area.y1; ++y)
        blend_span(y, area.x0, coverage.subspan(static_cast<std::size_t>(y - area.y0) * width, width), color);
}

void Canvas::fill_rect(Rect user, Color color) {
    if (user.size.width <= 0 || user.size.height <= 0) return;
    if (!state.ctm.axis_aligned()) { Path path; path.add_rect(user); fill_path(path, color); return; }
    // Exact area coverage of an axis-aligned rectangle.
    const auto box = bounding_rect(state.ctm, user);
    const auto x0 = box.origin.x, y0 = box.origin.y, x1 = x0 + box.size.width, y1 = y0 + box.size.height;
    const auto area = device_bounds(state.ctm, user).intersect(state.clip);
    if (area.empty()) return;
    const auto overlap = [](double a0, double a1, int p) { return std::clamp(std::min(a1, p + 1.0) - std::max(a0, static_cast<double>(p)), 0.0, 1.0); };
    std::vector<float> row(static_cast<std::size_t>(area.x1 - area.x0));
    for (int y = area.y0; y < area.y1; ++y) {
        const auto vertical = overlap(y0, y1, y);
        if (vertical <= 0) continue;
        for (int x = area.x0; x < area.x1; ++x) row[static_cast<std::size_t>(x - area.x0)] = static_cast<float>(vertical * overlap(x0, x1, x));
        blend_span(y, area.x0, row, color);
    }
}

void Canvas::fill_path(const Path& path, Color color, Winding winding) {
    if (path.empty()) return;
    rasterize(path.flatten(state.ctm), winding, state.clip,
              [&](int y, int x0, std::span<const float> coverage) { blend_span(y, x0, coverage, color); });
}

void Canvas::stroke_path(const Path& path, Color color, double width) {
    if (path.empty() || width <= 0) return;
    const auto half = std::max(width * ctm_scale(state.ctm), 0.35) / 2;   // hairlines stay visible
    std::vector<Path::Contour> polygons;
    const auto add_polygon = [&](std::vector<Point> polygon) {
        if (polygon.size() < 3) return;
        orient(polygon);
        polygons.push_back({std::move(polygon), true});
    };
    const auto joint = [&](Point center) {
        std::vector<Point> circle;
        constexpr int steps = 12;
        for (int i = 0; i < steps; ++i) {
            const auto angle = 2 * std::numbers::pi * i / steps;
            circle.push_back({center.x + half * std::cos(angle), center.y + half * std::sin(angle)});
        }
        add_polygon(std::move(circle));
    };
    for (const auto& contour : path.flatten(state.ctm)) {
        const auto& points = contour.points;
        const auto count = points.size();
        if (count < 2) continue;
        const auto segments = contour.closed ? count : count - 1;
        for (std::size_t i = 0; i < segments; ++i) {
            const auto p = points[i], q = points[(i + 1) % count];
            const auto length = std::hypot(q.x - p.x, q.y - p.y);
            if (length < 1e-9) continue;
            const Point normal{-(q.y - p.y) / length * half, (q.x - p.x) / length * half};
            add_polygon({{p.x + normal.x, p.y + normal.y}, {q.x + normal.x, q.y + normal.y},
                         {q.x - normal.x, q.y - normal.y}, {p.x - normal.x, p.y - normal.y}});
        }
        // Round joins at interior vertices (and every vertex of a closed contour).
        for (std::size_t i = contour.closed ? 0 : 1; i < (contour.closed ? count : count - 1); ++i) {
            const auto& previous = points[(i + count - 1) % count];
            const auto& next = points[(i + 1) % count];
            if (std::abs(cross(previous, points[i], next)) < 1e-9 &&
                (next.x - points[i].x) * (points[i].x - previous.x) + (next.y - points[i].y) * (points[i].y - previous.y) >= 0)
                continue;   // straight continuation
            joint(points[i]);
        }
    }
    rasterize(polygons, Winding::non_zero, state.clip,
              [&](int y, int x0, std::span<const float> coverage) { blend_span(y, x0, coverage, color); });
}

void Canvas::stroke_rect(Rect r, Color color, double width) {
    Path path;
    path.add_rect(r);
    stroke_path(path, color, width);
}
void Canvas::stroke_line(Point a, Point b, Color color, double width) {
    Path path;
    path.move_to(a);
    path.line_to(b);
    stroke_path(path, color, width);
}

void Canvas::draw_image(const Image& image, Rect user, double alpha, bool flip) {
    if (image.width == 0 || image.height == 0 || user.size.width <= 0 || user.size.height <= 0) return;
    if (image.rgba.size() < static_cast<std::size_t>(image.width) * image.height * 4) fail("image pixels are short");
    const auto area = device_bounds(state.ctm, user).intersect(state.clip);
    if (area.empty()) return;
    const auto inverse = state.ctm.inverted();
    if (!inverse) return;
    const auto sample = [&](int x, int y, int channel) {
        x = std::clamp(x, 0, static_cast<int>(image.width) - 1);
        y = std::clamp(y, 0, static_cast<int>(image.height) - 1);
        return image.rgba[(static_cast<std::size_t>(y) * image.width + x) * 4 + channel] / 255.0;
    };
    const auto scale_x = image.width / user.size.width, scale_y = image.height / user.size.height;
    const bool magnify = scale_x < 1 || scale_y < 1;
    std::vector<std::uint32_t> row(static_cast<std::size_t>(area.x1 - area.x0));
    for (int y = area.y0; y < area.y1; ++y) {
        target_->read(static_cast<std::uint32_t>(area.x0), static_cast<std::uint32_t>(y), row);
        bool changed{};
        for (int x = area.x0; x < area.x1; ++x) {
            const auto p = inverse->apply({x + 0.5, y + 0.5});
            if (p.x < user.origin.x || p.y < user.origin.y || p.x >= user.origin.x + user.size.width ||
                p.y >= user.origin.y + user.size.height) continue;
            // Image coordinates (rows from the top): user y grows upward
            // unless the image is drawn flipped.
            const auto u = (p.x - user.origin.x) * scale_x;
            const auto from_bottom = (p.y - user.origin.y) * scale_y;
            const auto v = flip ? from_bottom : image.height - from_bottom;
            std::array<double, 4> rgba{};
            if (magnify) {
                const auto fx = u - 0.5, fy = v - 0.5;
                const int ix = static_cast<int>(std::floor(fx)), iy = static_cast<int>(std::floor(fy));
                const auto wx = fx - ix, wy = fy - iy;
                for (int c = 0; c < 4; ++c) {
                    // Interpolate premultiplied values (no dark fringes).
                    const auto value = [&](int sx, int sy) { return c == 3 ? sample(sx, sy, 3) : sample(sx, sy, c) * sample(sx, sy, 3); };
                    rgba[c] = (value(ix, iy) * (1 - wx) + value(ix + 1, iy) * wx) * (1 - wy) +
                              (value(ix, iy + 1) * (1 - wx) + value(ix + 1, iy + 1) * wx) * wy;
                }
            } else {
                const int ix = static_cast<int>(u), iy = static_cast<int>(v);
                const auto a = sample(ix, iy, 3);
                rgba = {sample(ix, iy, 0) * a, sample(ix, iy, 1) * a, sample(ix, iy, 2) * a, a};
            }
            const auto factor = alpha * state.alpha;
            const auto source = pack(rgba[0] * factor, rgba[1] * factor, rgba[2] * factor, rgba[3] * factor);
            auto& pixel = row[static_cast<std::size_t>(x - area.x0)];
            pixel = state.blend == Blend::copy ? source : over(source, pixel);
            changed = true;
        }
        if (changed) target_->write(static_cast<std::uint32_t>(area.x0), static_cast<std::uint32_t>(y), row);
    }
}

// ---- text ----------------------------------------------------------------------------------
std::u16string utf16(std::string_view text) {
    std::u16string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        std::uint32_t code{};
        int extra{};
        if (c < 0x80) { code = c; }
        else if ((c >> 5) == 6) { code = c & 0x1f; extra = 1; }
        else if ((c >> 4) == 14) { code = c & 0x0f; extra = 2; }
        else if ((c >> 3) == 30) { code = c & 0x07; extra = 3; }
        else { code = 0xfffd; }
        ++i;
        for (int k = 0; k < extra && i < text.size(); ++k, ++i) code = (code << 6) | (static_cast<unsigned char>(text[i]) & 0x3f);
        if (code >= 0x10000) {
            code -= 0x10000;
            out.push_back(static_cast<char16_t>(0xd800 + (code >> 10)));
            out.push_back(static_cast<char16_t>(0xdc00 + (code & 0x3ff)));
        } else {
            out.push_back(static_cast<char16_t>(code));
        }
    }
    return out;
}
std::string utf8(std::u16string_view text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        std::uint32_t code = text[i];
        if (code >= 0xd800 && code < 0xdc00 && i + 1 < text.size() && text[i + 1] >= 0xdc00 && text[i + 1] < 0xe000)
            code = 0x10000 + ((code - 0xd800) << 10) + (text[++i] - 0xdc00);
        if (code < 0x80) out += static_cast<char>(code);
        else if (code < 0x800) { out += static_cast<char>(0xc0 | (code >> 6)); out += static_cast<char>(0x80 | (code & 0x3f)); }
        else if (code < 0x10000) {
            out += static_cast<char>(0xe0 | (code >> 12)); out += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (code & 0x3f));
        } else {
            out += static_cast<char>(0xf0 | (code >> 18)); out += static_cast<char>(0x80 | ((code >> 12) & 0x3f));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3f)); out += static_cast<char>(0x80 | (code & 0x3f));
        }
    }
    return out;
}

std::wstring host_family(const FontSpec& font) {
    std::string name = font.name;
    std::string lower;
    for (const auto c : name) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const auto has = [&](std::string_view part) { return lower.find(part) != std::string::npos; };
    if (font.monospace || has("mono") || has("menlo") || has("monaco") || has("courier") || has("consol"))
        return L"Consolas";
    if (lower.empty() || lower[0] == '.' || has("system") || has("sf pro") || has("sfns") || has("lucida grande") ||
        has("san francisco"))
        return L"Segoe UI";
    if (has("helvetica") || has("arial")) return L"Arial";
    if (has("times")) return L"Times New Roman";
    if (has("georgia")) return L"Georgia";
    if (has("verdana")) return L"Verdana";
    // A PostScript name: the family part before the style suffix.
    if (const auto dash = name.find('-'); dash != std::string::npos) name.resize(dash);
    const auto wide = utf16(name);
    return std::wstring(wide.begin(), wide.end());
}

namespace {
bool bold_name(const FontSpec& font) {
    std::string lower;
    for (const auto c : font.name) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return font.bold || lower.find("bold") != std::string::npos || lower.find("heavy") != std::string::npos ||
           lower.find("black") != std::string::npos || lower.find("semibold") != std::string::npos;
}
bool italic_name(const FontSpec& font) {
    std::string lower;
    for (const auto c : font.name) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return font.italic || lower.find("italic") != std::string::npos || lower.find("oblique") != std::string::npos;
}

// The host's raster font for `spec` at an em height of `pixels` (quarter
// pixels for the host's font cache).
host::gpu::RasterFont raster_font(const FontSpec& spec, double pixels) {
    const auto family = host_family(spec);
    host::gpu::RasterFont font;
    font.family = utf8(std::u16string(family.begin(), family.end()));
    font.quarter_pixels = std::max(4, static_cast<int>(std::lround(pixels * 4)));
    font.bold = bold_name(spec);
    font.italic = italic_name(spec);
    return font;
}
constexpr double reference_pixels = 256;   // metrics are measured at this em size and scaled

[[noreturn]] void host_failed(std::string_view what, host::Error error) {
    fail(std::string(what) + ": the host's text rasterizer failed (" + std::string(host::name(error)) + ")");
}
} // namespace

// Without a host text rasterizer (host::gpu answers unsupported) the
// metrics and advances are fixed proportions of the size, and text is not drawn.
FontMetrics font_metrics(const FontSpec& font) {
    const auto metrics = host::gpu::raster_metrics(raster_font(font, reference_pixels));
    if (!metrics) {
        if (metrics.error() != host::Error::unsupported) host_failed("font metrics", metrics.error());
        return {font.size * 0.93, font.size * 0.23, 0, font.size * 0.7, font.size * 0.5, font.size * 0.5, font.size};
    }
    const auto k = font.size / reference_pixels;
    FontMetrics result;
    result.ascent = metrics->ascent * k;
    result.descent = metrics->descent * k;
    result.leading = metrics->leading * k;
    result.average_width = metrics->average_width * k;
    result.max_width = metrics->max_width * k;
    result.cap_height = metrics->cap_height ? *metrics->cap_height * k : result.ascent * 0.7;
    result.x_height = metrics->x_height ? *metrics->x_height * k : result.ascent * 0.5;
    return result;
}
double text_width(const FontSpec& font, std::u16string_view text) {
    if (text.empty()) return 0;
    const auto width = host::gpu::raster_width(raster_font(font, reference_pixels), text);
    if (!width) {
        if (width.error() != host::Error::unsupported) host_failed("text width", width.error());
        return text.size() * font.size * 0.5;
    }
    return *width * font.size / reference_pixels;
}
std::vector<double> prefix_widths(const FontSpec& font, std::u16string_view text) {
    std::vector<double> result(text.size());
    if (text.empty()) return result;
    const auto extents = host::gpu::raster_prefix_widths(raster_font(font, reference_pixels), text);
    if (!extents) {
        if (extents.error() != host::Error::unsupported) host_failed("text widths", extents.error());
        for (std::size_t i = 0; i < text.size(); ++i) result[i] = (i + 1) * font.size * 0.5;
        return result;
    }
    for (std::size_t i = 0; i < text.size(); ++i) result[i] = (*extents)[i] * font.size / reference_pixels;
    return result;
}

double Canvas::draw_text(std::u16string_view text, const FontSpec& font, Point origin, Color color) {
    const auto advance = text_width(font, text);
    if (text.empty()) return advance;
    alchemy::gui::report_drawn_text(text);   // autopress labels (diagnostic, drawn_text.hpp)
    const auto& t = state.ctm;
    if (!t.axis_aligned()) fail("text drawing under a rotated or skewed transform is not supported");
    const auto scale_x = std::abs(t.a), scale_y = std::abs(t.d);
    const auto pixels = font.size * scale_y;
    if (pixels < 1 || pixels > 2000) return advance;
    // The device baseline origin; the line runs toward +x in device space
    // when the ctm keeps x (a > 0).
    const auto device = t.apply(origin);
    const auto metrics = font_metrics(font);
    const int pad = 2;
    const int ascent = static_cast<int>(std::ceil(metrics.ascent * scale_y)) + pad;
    const int descent = static_cast<int>(std::ceil(metrics.descent * scale_y)) + pad;
    const int width = static_cast<int>(std::ceil(advance * scale_x)) + 2 * pad + static_cast<int>(pixels / 4);
    const int height = ascent + descent;
    const int left = static_cast<int>(std::floor(device.x)) - pad;
    const int baseline = static_cast<int>(std::lround(device.y));
    const PixelRect area{left, baseline - ascent, left + width, baseline + descent};
    if (area.intersect(state.clip).empty()) return advance;
    const double fraction = device.x - std::floor(device.x);
    const auto coverage = host::gpu::raster_coverage(raster_font(font, pixels), text, width, height,
                                                     pad + static_cast<int>(std::lround(fraction)), ascent);
    if (!coverage) {
        if (coverage.error() == host::Error::unsupported) fail("text drawing needs the host's text rasterizer");
        host_failed("text drawing", coverage.error());
    }
    blend_coverage(area, *coverage, color);
    return advance;
}

std::vector<TextLine> layout_lines(const FontSpec& font, std::u16string_view text, double width) {
    std::vector<TextLine> lines;
    std::size_t start = 0;
    while (true) {
        auto end = text.find(u'\n', start);
        const bool last = end == std::u16string_view::npos;
        if (last) end = text.size();
        auto paragraph = text.substr(start, end - start);
        if (!paragraph.empty() && paragraph.back() == u'\r') paragraph.remove_suffix(1);
        const auto widths = prefix_widths(font, paragraph);
        const auto span_width = [&](std::size_t from, std::size_t to) {   // width of paragraph[from, to)
            if (to <= from) return 0.0;
            return widths[to - 1] - (from ? widths[from - 1] : 0.0);
        };
        if (width <= 0 || span_width(0, paragraph.size()) <= width) {
            lines.push_back({start, paragraph.size(), span_width(0, paragraph.size())});
        } else {
            std::size_t line_start = 0;
            while (line_start < paragraph.size()) {
                // The longest prefix that fits, preferably ending after a space.
                std::size_t fit = line_start, best_break = std::u16string_view::npos;
                while (fit < paragraph.size() && span_width(line_start, fit + 1) <= width) {
                    if (paragraph[fit] == u' ') best_break = fit + 1;
                    ++fit;
                }
                std::size_t line_end;
                if (fit >= paragraph.size()) line_end = paragraph.size();
                else if (paragraph[fit] == u' ') line_end = fit + 1;   // the overflowing space ends the line
                else if (best_break != std::u16string_view::npos && best_break > line_start) line_end = best_break;
                else line_end = std::max(fit, line_start + 1);
                auto shown_end = line_end;
                while (shown_end > line_start && paragraph[shown_end - 1] == u' ') --shown_end;
                lines.push_back({start + line_start, line_end - line_start, span_width(line_start, shown_end)});
                line_start = line_end;
            }
        }
        if (last) break;
        start = end + 1;
    }
    return lines;
}

} // namespace alchemy::gui::appkit::drawing
