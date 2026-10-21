#include "alchemy/darwin/coregraphics/q2d_internal.hpp"

#include <algorithm>
#include <numbers>

// Geometry (CGRect / CGAffineTransform semantics) and the CGPath model:
// element lists with CoreGraphics' arc, ellipse and rounded-rectangle
// constructions, bounds, flattening and hit testing.
namespace alchemy::gui::quartz2d {

// ---- rectangles ----------------------------------------------------------------------------
Rect standardize(Rect r) {
    if (rect_is_null(r)) return r;
    if (r.size.width < 0) { r.origin.x += r.size.width; r.size.width = -r.size.width; }
    if (r.size.height < 0) { r.origin.y += r.size.height; r.size.height = -r.size.height; }
    return r;
}
bool rect_is_empty(const Rect& r) { return rect_is_null(r) || r.size.width == 0 || r.size.height == 0; }
Rect rect_union(const Rect& a, const Rect& b) {
    if (rect_is_null(a)) return standardize(b);
    if (rect_is_null(b)) return standardize(a);
    const auto x0 = std::min(min_x(a), min_x(b)), y0 = std::min(min_y(a), min_y(b));
    const auto x1 = std::max(max_x(a), max_x(b)), y1 = std::max(max_y(a), max_y(b));
    return {{x0, y0}, {x1 - x0, y1 - y0}};
}
Rect rect_intersection(const Rect& a, const Rect& b) {
    if (rect_is_null(a) || rect_is_null(b)) return null_rect();
    const auto x0 = std::max(min_x(a), min_x(b)), y0 = std::max(min_y(a), min_y(b));
    const auto x1 = std::min(max_x(a), max_x(b)), y1 = std::min(max_y(a), max_y(b));
    if (x1 < x0 || y1 < y0) return null_rect();
    return {{x0, y0}, {x1 - x0, y1 - y0}};
}
Rect rect_integral(const Rect& r) {
    if (rect_is_null(r)) return r;
    const auto s = standardize(r);
    if (s.size.width == 0 && s.size.height == 0) return {{std::floor(s.origin.x), std::floor(s.origin.y)}, {0, 0}};
    const auto x0 = std::floor(s.origin.x), y0 = std::floor(s.origin.y);
    const auto x1 = std::ceil(s.origin.x + s.size.width), y1 = std::ceil(s.origin.y + s.size.height);
    return {{x0, y0}, {x1 - x0, y1 - y0}};
}
Rect rect_inset(const Rect& r, double dx, double dy) {
    if (rect_is_null(r)) return r;
    auto s = standardize(r);
    s.origin.x += dx; s.origin.y += dy; s.size.width -= 2 * dx; s.size.height -= 2 * dy;
    if (s.size.width < 0 || s.size.height < 0) return null_rect();
    return s;
}
bool rect_contains_point(const Rect& r, Point p) {
    if (rect_is_null(r)) return false;
    const auto s = standardize(r);
    return p.x >= s.origin.x && p.x < s.origin.x + s.size.width && p.y >= s.origin.y && p.y < s.origin.y + s.size.height;
}
Rect rect_from_points(Point a, Point b) {
    return {{std::min(a.x, b.x), std::min(a.y, b.y)}, {std::abs(b.x - a.x), std::abs(b.y - a.y)}};
}

std::optional<Affine> Affine::inverse() const {
    const auto det = determinant();
    if (det == 0 || !std::isfinite(det)) return std::nullopt;
    return Affine{d / det, -b / det, -c / det, a / det, (c * ty - d * tx) / det, (b * tx - a * ty) / det};
}

Rect transform_rect(const Affine& t, const Rect& r) {
    if (rect_is_null(r) || rect_is_infinite(r)) return r;
    const auto s = standardize(r);
    const std::array<Point, 4> corners{t.apply(s.origin), t.apply({s.origin.x + s.size.width, s.origin.y}),
                                       t.apply({s.origin.x, s.origin.y + s.size.height}),
                                       t.apply({s.origin.x + s.size.width, s.origin.y + s.size.height})};
    double x0 = corners[0].x, x1 = x0, y0 = corners[0].y, y1 = y0;
    for (const auto& p : corners) { x0 = std::min(x0, p.x); x1 = std::max(x1, p.x); y0 = std::min(y0, p.y); y1 = std::max(y1, p.y); }
    return {{x0, y0}, {x1 - x0, y1 - y0}};
}

// ---- path construction -----------------------------------------------------------------------
void PathData::move_to(Point p) {
    // Consecutive move-tos replace each other, as in CoreGraphics.
    if (!elements.empty() && elements.back().op == PathOp::move) elements.back().points[0] = p;
    else elements.push_back({PathOp::move, {p}});
    current = start = p;
    has_current = true;
}
void PathData::line_to(Point p) {
    if (!has_current) { move_to(p); return; }   // CG logs and starts a subpath; keep the point
    elements.push_back({PathOp::line, {p}});
    current = p;
}
void PathData::quad_to(Point c, Point p) {
    if (!has_current) move_to(c);
    elements.push_back({PathOp::quad, {c, p}});
    current = p;
}
void PathData::cubic_to(Point c1, Point c2, Point p) {
    if (!has_current) move_to(c1);
    elements.push_back({PathOp::cubic, {c1, c2, p}});
    current = p;
}
void PathData::close() {
    if (!has_current || elements.empty() || elements.back().op == PathOp::close) return;
    elements.push_back({PathOp::close, {}});
    current = start;
}
void PathData::add_rect(const Affine& m, const Rect& r) {
    const auto s = r;
    move_to(m.apply(s.origin));
    line_to(m.apply({s.origin.x + s.size.width, s.origin.y}));
    line_to(m.apply({s.origin.x + s.size.width, s.origin.y + s.size.height}));
    line_to(m.apply({s.origin.x, s.origin.y + s.size.height}));
    close();
}
namespace {
constexpr double kappa = 0.5522847498307936;   // 4/3 (sqrt(2) - 1)
}
void PathData::add_ellipse(const Affine& m, const Rect& r) {
    const auto s = standardize(r);
    const auto cx = s.origin.x + s.size.width / 2, cy = s.origin.y + s.size.height / 2;
    const auto rx = s.size.width / 2, ry = s.size.height / 2;
    const auto kx = rx * kappa, ky = ry * kappa;
    // Counter-clockwise (y up) from the maximum-x point.
    move_to(m.apply({cx + rx, cy}));
    cubic_to(m.apply({cx + rx, cy + ky}), m.apply({cx + kx, cy + ry}), m.apply({cx, cy + ry}));
    cubic_to(m.apply({cx - kx, cy + ry}), m.apply({cx - rx, cy + ky}), m.apply({cx - rx, cy}));
    cubic_to(m.apply({cx - rx, cy - ky}), m.apply({cx - kx, cy - ry}), m.apply({cx, cy - ry}));
    cubic_to(m.apply({cx + kx, cy - ry}), m.apply({cx + rx, cy - ky}), m.apply({cx + rx, cy}));
    close();
}
void PathData::add_rounded_rect(const Affine& m, const Rect& r, double rx, double ry) {
    const auto s = standardize(r);
    rx = std::clamp(rx, 0.0, s.size.width / 2);
    ry = std::clamp(ry, 0.0, s.size.height / 2);
    if (rx == 0 || ry == 0) { add_rect(m, s); return; }
    const auto x0 = s.origin.x, y0 = s.origin.y, x1 = x0 + s.size.width, y1 = y0 + s.size.height;
    const auto kx = rx * kappa, ky = ry * kappa;
    move_to(m.apply({x0 + rx, y0}));
    line_to(m.apply({x1 - rx, y0}));
    cubic_to(m.apply({x1 - rx + kx, y0}), m.apply({x1, y0 + ry - ky}), m.apply({x1, y0 + ry}));
    line_to(m.apply({x1, y1 - ry}));
    cubic_to(m.apply({x1, y1 - ry + ky}), m.apply({x1 - rx + kx, y1}), m.apply({x1 - rx, y1}));
    line_to(m.apply({x0 + rx, y1}));
    cubic_to(m.apply({x0 + rx - kx, y1}), m.apply({x0, y1 - ry + ky}), m.apply({x0, y1 - ry}));
    line_to(m.apply({x0, y0 + ry}));
    cubic_to(m.apply({x0, y0 + ry - ky}), m.apply({x0 + rx - kx, y0}), m.apply({x0 + rx, y0}));
    close();
}

namespace {
// Appends cubic segments approximating the arc of `radius` around `center`
// from `start` sweeping `delta` radians (at most pi/2 per segment).
void append_arc_segments(PathData& path, const Affine& m, Point center, double radius, double start, double delta) {
    const auto segments = std::max(1, static_cast<int>(std::ceil(std::abs(delta) / (std::numbers::pi / 2) - 1e-9)));
    const auto step = delta / segments;
    const auto k = 4.0 / 3.0 * std::tan(step / 4);
    auto angle = start;
    for (int index = 0; index < segments; ++index) {
        const auto a0 = angle, a1 = angle + step;
        const Point p0{center.x + radius * std::cos(a0), center.y + radius * std::sin(a0)};
        const Point p3{center.x + radius * std::cos(a1), center.y + radius * std::sin(a1)};
        const Point p1{p0.x - k * radius * std::sin(a0), p0.y + k * radius * std::cos(a0)};
        const Point p2{p3.x + k * radius * std::sin(a1), p3.y - k * radius * std::cos(a1)};
        path.cubic_to(m.apply(p1), m.apply(p2), m.apply(p3));
        angle = a1;
    }
}
} // namespace

void PathData::add_arc(const Affine& m, Point center, double radius, double start, double end, bool clockwise) {
    constexpr auto two_pi = 2 * std::numbers::pi;
    auto delta = end - start;
    if (!clockwise) {
        if (delta >= two_pi) delta = two_pi;
        else { delta = std::fmod(delta, two_pi); if (delta < 0) delta += two_pi; }
    } else {
        if (delta <= -two_pi) delta = -two_pi;
        else { delta = std::fmod(delta, two_pi); if (delta > 0) delta -= two_pi; }
    }
    add_relative_arc(m, center, radius, start, delta);
}
void PathData::add_relative_arc(const Affine& m, Point center, double radius, double start, double delta) {
    radius = std::abs(radius);
    const auto first = m.apply({center.x + radius * std::cos(start), center.y + radius * std::sin(start)});
    if (has_current) line_to(first);
    else move_to(first);
    if (delta == 0 || radius == 0) return;
    append_arc_segments(*this, m, center, radius, start, delta);
}
void PathData::add_arc_to_point(const Affine& m, Point p1, Point p2, double radius) {
    // The current point is stored transformed; arcs are built in the
    // untransformed space, so map it back.
    if (!has_current) { move_to(m.apply(p1)); return; }
    const auto inverse = m.inverse();
    const auto p0 = inverse ? inverse->apply(current) : current;
    const Point v1{p0.x - p1.x, p0.y - p1.y}, v2{p2.x - p1.x, p2.y - p1.y};
    const auto l1 = std::hypot(v1.x, v1.y), l2 = std::hypot(v2.x, v2.y);
    const auto cross = v1.x * v2.y - v1.y * v2.x;
    if (radius <= 0 || l1 == 0 || l2 == 0 || std::abs(cross) < 1e-12) { line_to(m.apply(p1)); return; }
    const auto cosine = std::clamp((v1.x * v2.x + v1.y * v2.y) / (l1 * l2), -1.0, 1.0);
    const auto angle = std::acos(cosine);              // angle at p1 between the two lines
    const auto distance = radius / std::tan(angle / 2);   // tangent distance from p1
    const Point t1{p1.x + v1.x / l1 * distance, p1.y + v1.y / l1 * distance};
    const Point t2{p1.x + v2.x / l2 * distance, p1.y + v2.y / l2 * distance};
    // Center: from p1 along the bisector.
    const Point bisector{v1.x / l1 + v2.x / l2, v1.y / l1 + v2.y / l2};
    const auto bl = std::hypot(bisector.x, bisector.y);
    const auto center_distance = radius / std::sin(angle / 2);
    const Point center{p1.x + bisector.x / bl * center_distance, p1.y + bisector.y / bl * center_distance};
    const auto a0 = std::atan2(t1.y - center.y, t1.x - center.x);
    const auto a1 = std::atan2(t2.y - center.y, t2.x - center.x);
    auto delta = a1 - a0;
    // Sweep the short way (less than pi), direction given by the turn.
    while (delta > std::numbers::pi) delta -= 2 * std::numbers::pi;
    while (delta < -std::numbers::pi) delta += 2 * std::numbers::pi;
    line_to(m.apply(t1));
    append_arc_segments(*this, m, center, radius, a0, delta);
}
void PathData::append(const PathData& other, const Affine& m) {
    for (const auto& element : other.elements) {
        switch (element.op) {
        case PathOp::move: move_to(m.apply(element.points[0])); break;
        case PathOp::line: line_to(m.apply(element.points[0])); break;
        case PathOp::quad: quad_to(m.apply(element.points[0]), m.apply(element.points[1])); break;
        case PathOp::cubic: cubic_to(m.apply(element.points[0]), m.apply(element.points[1]), m.apply(element.points[2])); break;
        case PathOp::close: close(); break;
        }
    }
}
PathData PathData::transformed(const Affine& m) const {
    PathData result;
    result.append(*this, m);
    return result;
}

// ---- measurement -------------------------------------------------------------------------------
Rect PathData::control_bounds() const {
    bool any{};
    double x0{}, y0{}, x1{}, y1{};
    for (const auto& element : elements)
        for (unsigned index = 0; index < element.count(); ++index) {
            const auto& p = element.points[index];
            if (!any) { x0 = x1 = p.x; y0 = y1 = p.y; any = true; }
            x0 = std::min(x0, p.x); x1 = std::max(x1, p.x); y0 = std::min(y0, p.y); y1 = std::max(y1, p.y);
        }
    if (!any) return null_rect();
    return {{x0, y0}, {x1 - x0, y1 - y0}};
}

namespace {
void extend(double& lo, double& hi, double value) { lo = std::min(lo, value); hi = std::max(hi, value); }
// Extrema of a cubic Bezier coordinate in (0, 1).
void cubic_extrema(double p0, double p1, double p2, double p3, double& lo, double& hi) {
    const auto a = -p0 + 3 * p1 - 3 * p2 + p3, b = 2 * (p0 - 2 * p1 + p2), c = p1 - p0;
    const auto at = [&](double t) {
        const auto u = 1 - t;
        return u * u * u * p0 + 3 * u * u * t * p1 + 3 * u * t * t * p2 + t * t * t * p3;
    };
    if (std::abs(a) < 1e-12) {
        if (std::abs(b) > 1e-12) { const auto t = -c / b; if (t > 0 && t < 1) extend(lo, hi, at(t)); }
        return;
    }
    const auto discriminant = b * b - 4 * a * c;
    if (discriminant < 0) return;
    const auto root = std::sqrt(discriminant);
    for (const auto t : {(-b + root) / (2 * a), (-b - root) / (2 * a)})
        if (t > 0 && t < 1) extend(lo, hi, at(t));
}
void quad_extrema(double p0, double p1, double p2, double& lo, double& hi) {
    const auto denominator = p0 - 2 * p1 + p2;
    if (std::abs(denominator) < 1e-12) return;
    const auto t = (p0 - p1) / denominator;
    if (t > 0 && t < 1) {
        const auto u = 1 - t;
        extend(lo, hi, u * u * p0 + 2 * u * t * p1 + t * t * p2);
    }
}
} // namespace

Rect PathData::path_bounds() const {
    bool any{};
    double x0{}, y0{}, x1{}, y1{};
    Point last{};
    const auto add = [&](Point p) {
        if (!any) { x0 = x1 = p.x; y0 = y1 = p.y; any = true; }
        extend(x0, x1, p.x); extend(y0, y1, p.y);
    };
    Point subpath{};
    for (const auto& element : elements) {
        switch (element.op) {
        case PathOp::move: add(element.points[0]); last = subpath = element.points[0]; break;
        case PathOp::line: add(element.points[0]); last = element.points[0]; break;
        case PathOp::quad:
            add(element.points[1]);
            quad_extrema(last.x, element.points[0].x, element.points[1].x, x0, x1);
            quad_extrema(last.y, element.points[0].y, element.points[1].y, y0, y1);
            last = element.points[1];
            break;
        case PathOp::cubic:
            add(element.points[2]);
            cubic_extrema(last.x, element.points[0].x, element.points[1].x, element.points[2].x, x0, x1);
            cubic_extrema(last.y, element.points[0].y, element.points[1].y, element.points[2].y, y0, y1);
            last = element.points[2];
            break;
        case PathOp::close: last = subpath; break;
        }
    }
    if (!any) return null_rect();
    return {{x0, y0}, {x1 - x0, y1 - y0}};
}

std::vector<std::vector<Point>> PathData::flatten(double tolerance) const {
    std::vector<std::vector<Point>> result;
    tolerance = std::max(tolerance, 1e-6);
    Point last{};
    const auto segments_for = [&](double length) {
        return std::clamp(static_cast<int>(std::ceil(std::sqrt(length / tolerance))), 1, 256);
    };
    for (const auto& element : elements) {
        switch (element.op) {
        case PathOp::move:
            result.push_back({element.points[0]});
            last = element.points[0];
            break;
        case PathOp::line:
            if (result.empty()) result.push_back({last});
            result.back().push_back(element.points[0]);
            last = element.points[0];
            break;
        case PathOp::quad: {
            if (result.empty()) result.push_back({last});
            const auto& c = element.points[0];
            const auto& p = element.points[1];
            const auto n = segments_for(std::hypot(c.x - last.x, c.y - last.y) + std::hypot(p.x - c.x, p.y - c.y));
            for (int index = 1; index <= n; ++index) {
                const auto t = static_cast<double>(index) / n, u = 1 - t;
                result.back().push_back({u * u * last.x + 2 * u * t * c.x + t * t * p.x, u * u * last.y + 2 * u * t * c.y + t * t * p.y});
            }
            last = p;
            break;
        }
        case PathOp::cubic: {
            if (result.empty()) result.push_back({last});
            const auto& c1 = element.points[0];
            const auto& c2 = element.points[1];
            const auto& p = element.points[2];
            const auto n = segments_for(std::hypot(c1.x - last.x, c1.y - last.y) + std::hypot(c2.x - c1.x, c2.y - c1.y) +
                                        std::hypot(p.x - c2.x, p.y - c2.y));
            for (int index = 1; index <= n; ++index) {
                const auto t = static_cast<double>(index) / n, u = 1 - t;
                result.back().push_back({u * u * u * last.x + 3 * u * u * t * c1.x + 3 * u * t * t * c2.x + t * t * t * p.x,
                                         u * u * u * last.y + 3 * u * u * t * c1.y + 3 * u * t * t * c2.y + t * t * t * p.y});
            }
            last = p;
            break;
        }
        case PathOp::close:
            if (!result.empty() && !result.back().empty()) last = result.back().front();
            break;
        }
    }
    return result;
}

bool PathData::contains(Point p, bool even_odd) const {
    int winding = 0;
    for (const auto& polygon : flatten(0.01)) {
        const auto n = polygon.size();
        if (n < 2) continue;
        for (std::size_t index = 0; index < n; ++index) {
            const auto& a = polygon[index];
            const auto& b = polygon[(index + 1) % n];
            if (a.y <= p.y) {
                if (b.y > p.y && (b.x - a.x) * (p.y - a.y) - (p.x - a.x) * (b.y - a.y) > 0) ++winding;
            } else if (b.y <= p.y && (b.x - a.x) * (p.y - a.y) - (p.x - a.x) * (b.y - a.y) < 0) {
                --winding;
            }
        }
    }
    return even_odd ? (winding & 1) != 0 : winding != 0;
}

std::optional<Rect> PathData::as_rect() const {
    // move + 3 or 4 lines (+ close), axis aligned.
    if (elements.size() < 4 || elements[0].op != PathOp::move) return std::nullopt;
    std::vector<Point> points{elements[0].points[0]};
    std::size_t index = 1;
    for (; index < elements.size() && elements[index].op == PathOp::line; ++index) points.push_back(elements[index].points[0]);
    if (index < elements.size() && elements[index].op == PathOp::close) ++index;
    if (index != elements.size()) return std::nullopt;
    if (points.size() == 5 && points[4].x == points[0].x && points[4].y == points[0].y) points.pop_back();
    if (points.size() != 4) return std::nullopt;
    for (std::size_t corner = 0; corner < 4; ++corner) {
        const auto& a = points[corner];
        const auto& b = points[(corner + 1) % 4];
        if (a.x != b.x && a.y != b.y) return std::nullopt;
    }
    const auto r = rect_from_points(points[0], points[2]);
    if (r.size.width == 0 || r.size.height == 0) return std::nullopt;
    return r;
}

bool PathData::operator==(const PathData& other) const {
    if (elements.size() != other.elements.size()) return false;
    for (std::size_t index = 0; index < elements.size(); ++index) {
        const auto& a = elements[index];
        const auto& b = other.elements[index];
        if (a.op != b.op) return false;
        for (unsigned point = 0; point < a.count(); ++point)
            if (a.points[point].x != b.points[point].x || a.points[point].y != b.points[point].y) return false;
    }
    return true;
}

} // namespace alchemy::gui::quartz2d
