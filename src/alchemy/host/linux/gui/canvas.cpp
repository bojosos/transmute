// Linux host backend, area "gpu": 2D drawing for the Quartz 2D pipeline over Cairo
// (include/alchemy/host/gpu.hpp). A Surface is a Cairo image surface of premultiplied BGRA
// (CAIRO_FORMAT_ARGB32 on a little-endian host); between begin and end a cairo_t draws on it,
// otherwise its pixels are plain memory. Geometries keep their figures and replay them into
// Cairo; widened() is this file's own polygon stroker (Cairo has no public path widening).
#include "alchemy/host/linux/gui/fonts.hpp"

#include <cairo-ft.h>
#include <cairo.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <numbers>

namespace alchemy::host::gpu {

struct Geometry::Impl {
    struct Figure {
        Point start;
        std::vector<PathElement> segments;   // line, quad and cubic
        bool closed{};
    };
    std::vector<Figure> figures;
    bool even_odd{}, filled{true};
};

struct Surface::Impl {
    std::uint32_t width{}, height{};
    cairo_surface_t* surface{};
    cairo_t* cr{};
    cairo_matrix_t matrix{1, 0, 0, 1, 0, 0};
    bool antialias{true}, text_antialias{true};
    cairo_operator_t blend{CAIRO_OPERATOR_OVER};
    ~Impl() {
        if (cr) cairo_destroy(cr);
        if (surface) cairo_surface_destroy(surface);
    }
};

namespace {

// ---- paths ----

void replay(cairo_t* cr, const Geometry::Impl& geometry, bool fill) {
    cairo_new_path(cr);
    for (const auto& figure : geometry.figures) {
        if (fill && !geometry.filled) continue;   // hollow figures are only stroked
        cairo_move_to(cr, figure.start.x, figure.start.y);
        Point current = figure.start;
        for (const auto& s : figure.segments) {
            switch (s.op) {
            case PathOp::line:
                cairo_line_to(cr, s.points[0].x, s.points[0].y);
                current = s.points[0];
                break;
            case PathOp::quad: {
                const auto& q = s.points[0];
                const auto& e = s.points[1];
                cairo_curve_to(cr, current.x + 2.0 / 3.0 * (q.x - current.x), current.y + 2.0 / 3.0 * (q.y - current.y),
                               e.x + 2.0 / 3.0 * (q.x - e.x), e.y + 2.0 / 3.0 * (q.y - e.y), e.x, e.y);
                current = e;
                break;
            }
            case PathOp::cubic:
                cairo_curve_to(cr, s.points[0].x, s.points[0].y, s.points[1].x, s.points[1].y, s.points[2].x, s.points[2].y);
                current = s.points[2];
                break;
            default:
                break;
            }
        }
        if (figure.closed) cairo_close_path(cr);
    }
}

cairo_line_cap_t cap(StrokeStyle::Cap value) {
    return value == StrokeStyle::Cap::round ? CAIRO_LINE_CAP_ROUND : value == StrokeStyle::Cap::square ? CAIRO_LINE_CAP_SQUARE : CAIRO_LINE_CAP_BUTT;
}

void set_stroke(cairo_t* cr, double width, const StrokeStyle& style) {
    cairo_set_line_width(cr, width);
    cairo_set_line_cap(cr, cap(style.cap));
    cairo_set_line_join(cr, style.join == StrokeStyle::Join::round ? CAIRO_LINE_JOIN_ROUND
                            : style.join == StrokeStyle::Join::bevel ? CAIRO_LINE_JOIN_BEVEL
                                                                     : CAIRO_LINE_JOIN_MITER);
    // Direct2D's limit is miter length per half stroke width (at least 1); Cairo's per stroke width.
    cairo_set_miter_limit(cr, std::max(1.0, style.miter_limit) / 2.0);
    std::vector<double> dashes;
    for (const auto value : style.dashes) dashes.push_back(value * width);
    cairo_set_dash(cr, dashes.empty() ? nullptr : dashes.data(), static_cast<int>(dashes.size()), style.dash_offset * width);
}

// ---- the stroker behind widened() ----

struct Vec {
    double x{}, y{};
};
Vec operator+(Vec a, Vec b) { return {a.x + b.x, a.y + b.y}; }
Vec operator-(Vec a, Vec b) { return {a.x - b.x, a.y - b.y}; }
Vec operator*(Vec a, double s) { return {a.x * s, a.y * s}; }
double length(Vec a) { return std::hypot(a.x, a.y); }
double cross(Vec a, Vec b) { return a.x * b.y - a.y * b.x; }

struct Polyline {
    std::vector<Vec> points;
    bool closed{};
};

// Figures flattened to polylines within `tolerance`.
std::vector<Polyline> flatten(const Geometry::Impl& geometry, double tolerance) {
    std::vector<Polyline> lines;
    for (const auto& figure : geometry.figures) {
        Polyline line;
        line.closed = figure.closed;
        Vec current{figure.start.x, figure.start.y};
        line.points.push_back(current);
        const auto add_cubic = [&](Vec p1, Vec p2, Vec p3) {
            const auto p0 = current;
            const double d = std::max(length(p0 - p1 * 2 + p2), length(p1 - p2 * 2 + p3));
            const int steps = std::clamp(static_cast<int>(std::ceil(std::sqrt(0.75 * d / tolerance))), 1, 1024);
            for (int i = 1; i <= steps; ++i) {
                const double t = static_cast<double>(i) / steps, u = 1 - t;
                line.points.push_back(p0 * (u * u * u) + p1 * (3 * u * u * t) + p2 * (3 * u * t * t) + p3 * (t * t * t));
            }
            current = p3;
        };
        for (const auto& s : figure.segments) {
            const Vec a{s.points[0].x, s.points[0].y}, b{s.points[1].x, s.points[1].y}, c{s.points[2].x, s.points[2].y};
            if (s.op == PathOp::line) {
                line.points.push_back(a);
                current = a;
            } else if (s.op == PathOp::quad) {
                add_cubic(current + (a - current) * (2.0 / 3.0), b + (a - b) * (2.0 / 3.0), b);
            } else if (s.op == PathOp::cubic) {
                add_cubic(a, b, c);
            }
        }
        // Drop repeated points (zero-length pieces have no direction).
        std::vector<Vec> unique;
        for (const auto& p : line.points)
            if (unique.empty() || length(p - unique.back()) > 1e-9) unique.push_back(p);
        if (line.closed && unique.size() > 1 && length(unique.front() - unique.back()) <= 1e-9) unique.pop_back();
        line.points = std::move(unique);
        lines.push_back(std::move(line));
    }
    return lines;
}

// The dashes of a polyline, as open polylines.
std::vector<Polyline> dash(const Polyline& line, const std::vector<double>& pattern, double offset) {
    double total = 0;
    for (const auto value : pattern) total += std::max(0.0, value);
    if (pattern.empty() || total <= 0) return {line};
    auto points = line.points;
    if (line.closed && !points.empty()) points.push_back(points.front());
    std::vector<Polyline> pieces;
    std::size_t index = 0;
    double remaining = 0;
    double phase = std::fmod(offset, total);
    if (phase < 0) phase += total;
    while (phase >= std::max(0.0, pattern[index])) {
        phase -= std::max(0.0, pattern[index]);
        index = (index + 1) % pattern.size();
    }
    remaining = std::max(0.0, pattern[index]) - phase;
    bool on = index % 2 == 0;
    Polyline piece;
    if (on && !points.empty()) piece.points.push_back(points.front());
    for (std::size_t i = 1; i < points.size(); ++i) {
        Vec a = points[i - 1];
        const Vec b = points[i];
        double left = length(b - a);
        while (left > remaining) {
            const Vec at = a + (b - a) * (remaining / left);
            if (on) {
                piece.points.push_back(at);
                pieces.push_back(std::move(piece));
                piece = {};
            } else {
                piece.points = {at};
            }
            left -= remaining;
            a = at;
            index = (index + 1) % pattern.size();
            remaining = std::max(0.0, pattern[index]);
            on = !on;
        }
        remaining -= left;
        if (on) piece.points.push_back(b);
    }
    if (on && !piece.points.empty()) pieces.push_back(std::move(piece));
    return pieces;
}

// Polygons whose non-zero union is the stroke (each turned the same way).
struct Stroker {
    double half{};
    const StrokeStyle& style;
    std::vector<std::vector<Vec>> polygons;

    void add(std::vector<Vec> polygon) {
        if (polygon.size() < 3) return;
        double area = 0;
        for (std::size_t i = 0; i < polygon.size(); ++i) area += cross(polygon[i], polygon[(i + 1) % polygon.size()]);
        if (std::abs(area) < 1e-12) return;
        if (area < 0) std::reverse(polygon.begin(), polygon.end());
        polygons.push_back(std::move(polygon));
    }
    std::vector<Vec> circle(Vec centre) const {
        const int steps = std::clamp(static_cast<int>(std::ceil(std::numbers::pi / std::acos(std::max(-1.0, 1 - 0.05 / std::max(half, 0.05))))), 8, 256);
        std::vector<Vec> points;
        for (int i = 0; i < steps; ++i) {
            const double angle = 2 * std::numbers::pi * i / steps;
            points.push_back(centre + Vec{std::cos(angle), std::sin(angle)} * half);
        }
        return points;
    }
    void segment(Vec a, Vec b) {
        const Vec d = (b - a) * (1 / length(b - a));
        const Vec n{-d.y * half, d.x * half};
        add({a + n, b + n, b - n, a - n});
    }
    void join(Vec p, Vec d1, Vec d2) {
        const double turn = cross(d1, d2);
        if (std::abs(turn) < 1e-12 && d1.x * d2.x + d1.y * d2.y > 0) return;   // straight on
        if (style.join == StrokeStyle::Join::round) {
            add(circle(p));
            return;
        }
        const Vec n1{-d1.y, d1.x}, n2{-d2.y, d2.x};
        const double side = turn > 0 ? -1 : 1;   // the outer side of the turn
        const Vec a = p + n1 * (side * half), b = p + n2 * (side * half);
        const Vec m = n1 + n2;
        const double m2 = m.x * m.x + m.y * m.y;
        if (style.join == StrokeStyle::Join::miter_or_bevel && m2 > 1e-12 && 2 / std::sqrt(m2) <= std::max(1.0, style.miter_limit)) {
            add({p, a, p + m * (side * 2 * half / m2), b});
            return;
        }
        add({p, a, b});
    }
    void end_cap(Vec p, Vec d) {   // d points out of the line
        if (style.cap == StrokeStyle::Cap::round) add(circle(p));
        else if (style.cap == StrokeStyle::Cap::square) {
            const Vec n{-d.y * half, d.x * half};
            add({p + n, p + n + d * half, p - n + d * half, p - n});
        }
    }
    void stroke(const Polyline& line) {
        const auto& p = line.points;
        if (p.empty()) return;
        if (p.size() == 1) {   // a dot: only caps draw it
            if (style.cap == StrokeStyle::Cap::round) add(circle(p[0]));
            else if (style.cap == StrokeStyle::Cap::square) add({p[0] + Vec{-half, -half}, p[0] + Vec{half, -half}, p[0] + Vec{half, half}, p[0] + Vec{-half, half}});
            return;
        }
        const std::size_t count = line.closed ? p.size() : p.size() - 1;
        const auto direction = [&](std::size_t i) { const Vec d = p[(i + 1) % p.size()] - p[i]; return d * (1 / length(d)); };
        for (std::size_t i = 0; i < count; ++i) segment(p[i], p[(i + 1) % p.size()]);
        for (std::size_t i = 1; i < count; ++i) join(p[i], direction(i - 1), direction(i));
        if (line.closed) join(p[0], direction(count - 1), direction(0));
        else {
            end_cap(p.front(), direction(0) * -1);
            end_cap(p.back(), direction(count - 1));
        }
    }
};

// ---- brushes ----

struct Source {
    cairo_pattern_t* pattern{};
    cairo_surface_t* image{};
    double alpha{1};
    ~Source() {
        if (pattern) cairo_pattern_destroy(pattern);
        if (image) cairo_surface_destroy(image);
    }
};

cairo_surface_t* wrap(const BgraView& pixels) {
    if (!pixels.pixels || pixels.width == 0 || pixels.height == 0) return nullptr;
    auto* surface = cairo_image_surface_create_for_data(const_cast<std::uint8_t*>(pixels.pixels), CAIRO_FORMAT_ARGB32, static_cast<int>(pixels.width),
                                                        static_cast<int>(pixels.height), static_cast<int>(pixels.width * 4));
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return nullptr;
    }
    return surface;
}

Status make_source(const Brush& brush, Source& source) {
    switch (brush.kind) {
    case Brush::Kind::solid:
        source.pattern = cairo_pattern_create_rgba(brush.color.r, brush.color.g, brush.color.b, brush.color.a);
        return {};
    case Brush::Kind::tiles: {
        source.image = wrap(brush.tiles);
        if (!source.image) return Error::invalid_argument;
        source.pattern = cairo_pattern_create_for_surface(source.image);
        cairo_pattern_set_extend(source.pattern, CAIRO_EXTEND_REPEAT);
        cairo_pattern_set_filter(source.pattern, CAIRO_FILTER_BILINEAR);
        const auto& t = brush.tiles_transform;
        cairo_matrix_t matrix{t.a, t.b, t.c, t.d, t.tx, t.ty};
        if (cairo_matrix_invert(&matrix) != CAIRO_STATUS_SUCCESS) return Error::invalid_argument;
        cairo_pattern_set_matrix(source.pattern, &matrix);
        source.alpha = brush.opacity;
        return {};
    }
    case Brush::Kind::linear_gradient:
        if (brush.stops.empty()) return Error::invalid_argument;
        source.pattern = cairo_pattern_create_linear(brush.start.x, brush.start.y, brush.end.x, brush.end.y);
        for (const auto& stop : brush.stops)
            cairo_pattern_add_color_stop_rgba(source.pattern, std::clamp(stop.position, 0.0, 1.0), stop.color.r, stop.color.g, stop.color.b, stop.color.a);
        cairo_pattern_set_extend(source.pattern, CAIRO_EXTEND_PAD);
        source.alpha = brush.opacity;
        return {};
    }
    return Error::invalid_argument;
}

// Draws with `brush` as the source; an opacity below one goes through a group.
Status paint(cairo_t* cr, const Brush& brush, const std::function<void()>& draw) {
    if (!cr) return Error::busy;
    Source source;
    if (const auto made = make_source(brush, source); !made) return made;
    if (source.alpha >= 1) {
        cairo_set_source(cr, source.pattern);
        draw();
    } else {
        cairo_push_group(cr);
        cairo_set_source(cr, source.pattern);
        draw();
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, std::max(0.0, source.alpha));
    }
    return cairo_status(cr) == CAIRO_STATUS_SUCCESS ? Status{} : Status{Error::io};
}

// ---- glyphs ----

const cairo_user_data_key_t face_key{};

struct CairoFace {
    FT_Face face{};
    std::shared_ptr<const std::vector<std::uint8_t>> bytes;
};

cairo_font_face_t* canvas_face(const Font& font) {
    auto* impl = font.impl();
    std::lock_guard lock(freetype::freetype_mutex());
    if (impl->canvas_face) return static_cast<cairo_font_face_t*>(impl->canvas_face.get());
    cairo_font_face_t* face{};
    if (!impl->bytes) {
        // Cairo opens its own FreeType face for the file.
        FcPattern* pattern = FcPatternCreate();
        FcPatternAddString(pattern, FC_FILE, reinterpret_cast<const FcChar8*>(impl->path.c_str()));
        FcPatternAddInteger(pattern, FC_INDEX, impl->index);
        face = cairo_ft_font_face_create_for_pattern(pattern);
        FcPatternDestroy(pattern);
    } else {
        // An in-memory face: a FreeType face of Cairo's own over the same bytes.
        auto holder = std::make_unique<CairoFace>();
        holder->bytes = impl->bytes;
        if (FT_New_Memory_Face(freetype::library(), impl->bytes->data(), static_cast<FT_Long>(impl->bytes->size()), impl->index, &holder->face) != 0)
            return nullptr;
        face = cairo_ft_font_face_create_for_ft_face(holder->face, 0);
        if (cairo_font_face_set_user_data(face, &face_key, holder.get(), [](void* data) {
                std::unique_ptr<CairoFace> owned(static_cast<CairoFace*>(data));
                std::lock_guard inner(freetype::freetype_mutex());
                FT_Done_Face(owned->face);
            }) != CAIRO_STATUS_SUCCESS) {
            cairo_font_face_destroy(face);
            FT_Done_Face(holder->face);
            return nullptr;
        }
        static_cast<void>(holder.release());
    }
    if (cairo_font_face_status(face) != CAIRO_STATUS_SUCCESS) {
        cairo_font_face_destroy(face);
        return nullptr;
    }
    impl->canvas_face = std::shared_ptr<void>(face, [](void* f) { cairo_font_face_destroy(static_cast<cairo_font_face_t*>(f)); });
    return face;
}

} // namespace

// ---- Geometry ----------------------------------------------------------------------------------
Result<Geometry> Geometry::path(std::span<const PathElement> elements, bool even_odd, bool filled) {
    auto impl = std::make_shared<Impl>();
    impl->even_odd = even_odd;
    impl->filled = filled;
    bool open = false;
    Point start{}, last{};
    const auto begin_figure = [&](Point p) {
        impl->figures.push_back({p, {}, false});
        open = true;
        start = last = p;
    };
    for (const auto& element : elements) {
        switch (element.op) {
        case PathOp::move:
            open = false;
            begin_figure(element.points[0]);
            break;
        case PathOp::line:
            if (!open) begin_figure(last);
            impl->figures.back().segments.push_back(element);
            last = element.points[0];
            break;
        case PathOp::quad:
            if (!open) begin_figure(last);
            impl->figures.back().segments.push_back(element);
            last = element.points[1];
            break;
        case PathOp::cubic:
            if (!open) begin_figure(last);
            impl->figures.back().segments.push_back(element);
            last = element.points[2];
            break;
        case PathOp::close:
            if (open) impl->figures.back().closed = true;
            open = false;
            last = start;
            break;
        default:
            return Error::invalid_argument;
        }
    }
    return Geometry(std::move(impl));
}

Result<std::vector<PathElement>> Geometry::widened(double width, const StrokeStyle& style, const Matrix& transform) const {
    if (!impl_) return Error::bad_handle;
    if (!(width > 0)) return std::vector<PathElement>{};
    Stroker stroker{width / 2, style, {}};
    std::vector<double> pattern;
    for (const auto value : style.dashes) pattern.push_back(value * width);
    for (const auto& line : flatten(*impl_, 0.05))
        for (const auto& piece : dash(line, pattern, style.dash_offset * width)) stroker.stroke(piece);
    std::vector<PathElement> path;
    const auto map = [&](Vec p) {
        return Point{transform.a * p.x + transform.c * p.y + transform.tx, transform.b * p.x + transform.d * p.y + transform.ty};
    };
    for (const auto& polygon : stroker.polygons) {
        path.push_back({PathOp::move, {map(polygon.front())}});
        for (std::size_t i = 1; i < polygon.size(); ++i) path.push_back({PathOp::line, {map(polygon[i])}});
        path.push_back({PathOp::close, {}});
    }
    return path;
}

Result<bool> Geometry::stroke_contains(Point point, double width) const {
    if (!impl_) return Error::bad_handle;
    auto* scratch = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
    auto* cr = cairo_create(scratch);
    set_stroke(cr, width, StrokeStyle{});
    replay(cr, *impl_, false);
    const bool hit = cairo_in_stroke(cr, point.x, point.y) != 0;
    const bool ok = cairo_status(cr) == CAIRO_STATUS_SUCCESS;
    cairo_destroy(cr);
    cairo_surface_destroy(scratch);
    if (!ok) return Error::invalid_argument;
    return hit;
}

// ---- Surface -----------------------------------------------------------------------------------
Result<std::unique_ptr<Surface>> Surface::create(std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0) return Error::invalid_argument;
    if (width > 32767 || height > 32767) return Error::no_memory;   // Cairo's image size limit
    auto impl = std::make_unique<Impl>();
    impl->width = width;
    impl->height = height;
    impl->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, static_cast<int>(width), static_cast<int>(height));
    if (cairo_surface_status(impl->surface) != CAIRO_STATUS_SUCCESS) return Error::no_memory;
    return std::make_unique<Surface>(std::move(impl));
}

Surface::Surface(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Surface::~Surface() = default;
std::uint32_t Surface::width() const noexcept { return impl_->width; }
std::uint32_t Surface::height() const noexcept { return impl_->height; }
bool Surface::blend_modes() const noexcept { return true; }

namespace {
// The drawing state Cairo's save/restore (the clip stack) would otherwise take back.
void apply_state(Surface::Impl& impl) {
    cairo_set_matrix(impl.cr, &impl.matrix);
    cairo_set_antialias(impl.cr, impl.antialias ? CAIRO_ANTIALIAS_DEFAULT : CAIRO_ANTIALIAS_NONE);
    cairo_set_operator(impl.cr, impl.blend);
}
} // namespace

Status Surface::begin() {
    if (impl_->cr) return Error::busy;
    impl_->cr = cairo_create(impl_->surface);
    if (cairo_status(impl_->cr) != CAIRO_STATUS_SUCCESS) {
        cairo_destroy(impl_->cr);
        impl_->cr = nullptr;
        return Error::no_memory;
    }
    impl_->matrix = {1, 0, 0, 1, 0, 0};
    apply_state(*impl_);
    return {};
}

Status Surface::end() {
    if (!impl_->cr) return {};
    const auto status = cairo_status(impl_->cr);
    cairo_destroy(impl_->cr);
    impl_->cr = nullptr;
    cairo_surface_flush(impl_->surface);
    return status == CAIRO_STATUS_SUCCESS ? Status{} : Status{Error::io};
}

Result<Surface::Lock> Surface::lock(const PixelRect& rect, bool write) {
    if (impl_->cr) return Error::busy;
    if (rect.empty() || rect.x0 < 0 || rect.y0 < 0 || static_cast<std::uint32_t>(rect.x1) > impl_->width ||
        static_cast<std::uint32_t>(rect.y1) > impl_->height)
        return Error::invalid_argument;
    cairo_surface_flush(impl_->surface);
    Lock result;
    result.stride = static_cast<std::uint32_t>(cairo_image_surface_get_stride(impl_->surface));
    result.data = cairo_image_surface_get_data(impl_->surface) + static_cast<std::size_t>(rect.y0) * result.stride + static_cast<std::size_t>(rect.x0) * 4;
    auto* surface = cairo_surface_reference(impl_->surface);
    result.hold = std::shared_ptr<void>(surface, [write, rect](void* held) {
        auto* s = static_cast<cairo_surface_t*>(held);
        if (write) cairo_surface_mark_dirty_rectangle(s, rect.x0, rect.y0, static_cast<int>(rect.width()), static_cast<int>(rect.height()));
        cairo_surface_destroy(s);
    });
    return result;
}

void Surface::set_transform(const Matrix& transform) {
    impl_->matrix = {transform.a, transform.b, transform.c, transform.d, transform.tx, transform.ty};
    if (impl_->cr) cairo_set_matrix(impl_->cr, &impl_->matrix);
}
void Surface::set_antialias(bool antialias) {
    impl_->antialias = antialias;
    if (impl_->cr) cairo_set_antialias(impl_->cr, antialias ? CAIRO_ANTIALIAS_DEFAULT : CAIRO_ANTIALIAS_NONE);
}
void Surface::set_text_antialias(bool grayscale) { impl_->text_antialias = grayscale; }
void Surface::set_blend(Blend blend) {
    impl_->blend = blend == Blend::copy ? CAIRO_OPERATOR_SOURCE : blend == Blend::add ? CAIRO_OPERATOR_ADD : CAIRO_OPERATOR_OVER;
    if (impl_->cr) cairo_set_operator(impl_->cr, impl_->blend);
}
void Surface::push_axis_clip(const Box& box, bool antialias) {
    auto* cr = impl_->cr;
    if (!cr) return;
    cairo_save(cr);
    cairo_set_antialias(cr, antialias ? CAIRO_ANTIALIAS_DEFAULT : CAIRO_ANTIALIAS_NONE);
    cairo_new_path(cr);
    cairo_rectangle(cr, box.left, box.top, box.right - box.left, box.bottom - box.top);
    cairo_clip(cr);
    apply_state(*impl_);
}
void Surface::pop_axis_clip() {
    if (!impl_->cr) return;
    cairo_restore(impl_->cr);
    apply_state(*impl_);
}
Status Surface::push_clip(const Geometry& geometry, bool antialias) {
    if (!geometry) return Error::bad_handle;
    auto* cr = impl_->cr;
    if (!cr) return Error::busy;
    cairo_save(cr);
    cairo_set_antialias(cr, antialias ? CAIRO_ANTIALIAS_DEFAULT : CAIRO_ANTIALIAS_NONE);
    replay(cr, *geometry.impl(), true);
    cairo_set_fill_rule(cr, geometry.impl()->even_odd ? CAIRO_FILL_RULE_EVEN_ODD : CAIRO_FILL_RULE_WINDING);
    cairo_clip(cr);
    apply_state(*impl_);
    return {};
}
void Surface::pop_clip() { pop_axis_clip(); }

Status Surface::fill_rect(const Box& box, const Brush& brush) {
    auto* cr = impl_->cr;
    return paint(cr, brush, [&] {
        cairo_new_path(cr);
        cairo_rectangle(cr, box.left, box.top, box.right - box.left, box.bottom - box.top);
        cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
        cairo_fill(cr);
    });
}
Status Surface::fill(const Geometry& geometry, const Brush& brush) {
    if (!geometry) return Error::bad_handle;
    auto* cr = impl_->cr;
    return paint(cr, brush, [&] {
        replay(cr, *geometry.impl(), true);
        cairo_set_fill_rule(cr, geometry.impl()->even_odd ? CAIRO_FILL_RULE_EVEN_ODD : CAIRO_FILL_RULE_WINDING);
        cairo_fill(cr);
    });
}
Status Surface::stroke(const Geometry& geometry, const Brush& brush, double width, const StrokeStyle& style) {
    if (!geometry) return Error::bad_handle;
    auto* cr = impl_->cr;
    return paint(cr, brush, [&] {
        set_stroke(cr, width, style);
        replay(cr, *geometry.impl(), false);
        cairo_stroke(cr);
    });
}
Status Surface::draw_bitmap(const BgraView& pixels, const Box& destination, double opacity, Interpolation interpolation) {
    auto* cr = impl_->cr;
    if (!cr) return Error::busy;
    const double w = destination.right - destination.left, h = destination.bottom - destination.top;
    if (!(w > 0) || !(h > 0)) return {};
    auto* image = wrap(pixels);
    if (!image) return Error::invalid_argument;
    auto* pattern = cairo_pattern_create_for_surface(image);
    cairo_matrix_t matrix;
    cairo_matrix_init_scale(&matrix, pixels.width / w, pixels.height / h);
    cairo_matrix_translate(&matrix, -destination.left, -destination.top);
    cairo_pattern_set_matrix(pattern, &matrix);
    cairo_pattern_set_extend(pattern, CAIRO_EXTEND_PAD);
    cairo_pattern_set_filter(pattern, interpolation == Interpolation::nearest ? CAIRO_FILTER_NEAREST
                                      : interpolation == Interpolation::high_quality ? CAIRO_FILTER_BEST
                                                                                     : CAIRO_FILTER_BILINEAR);
    cairo_save(cr);
    cairo_new_path(cr);
    cairo_rectangle(cr, destination.left, destination.top, w, h);
    cairo_clip(cr);
    cairo_set_source(cr, pattern);
    cairo_paint_with_alpha(cr, std::clamp(opacity, 0.0, 1.0));
    cairo_restore(cr);
    apply_state(*impl_);
    cairo_pattern_destroy(pattern);
    cairo_surface_destroy(image);
    return cairo_status(cr) == CAIRO_STATUS_SUCCESS ? Status{} : Status{Error::io};
}
Status Surface::draw_glyphs(const Font& font, double size, std::span<const std::uint16_t> glyphs, std::span<const Point> offsets,
                            const Brush& brush) {
    if (!font) return Error::bad_handle;
    auto* cr = impl_->cr;
    if (!cr) return Error::busy;
    auto* face = canvas_face(font);
    if (!face) return Error::io;
    std::vector<cairo_glyph_t> placed(glyphs.size());
    for (std::size_t index = 0; index < glyphs.size(); ++index) {
        const auto p = index < offsets.size() ? offsets[index] : Point{};
        placed[index] = {glyphs[index], p.x, -p.y};   // y up from the baseline
    }
    // As DirectWrite's natural mode draws them: snapped vertically only (stems
    // and bars on whole rows), unhinted advances, grayscale or aliased.
    auto* options = cairo_font_options_create();
    cairo_font_options_set_hint_style(options, CAIRO_HINT_STYLE_SLIGHT);
    cairo_font_options_set_hint_metrics(options, CAIRO_HINT_METRICS_OFF);
    cairo_font_options_set_antialias(options, impl_->text_antialias ? CAIRO_ANTIALIAS_GRAY : CAIRO_ANTIALIAS_NONE);
    cairo_set_font_face(cr, face);
    cairo_set_font_size(cr, size);
    cairo_set_font_options(cr, options);
    cairo_font_options_destroy(options);
    return paint(cr, brush, [&] { cairo_show_glyphs(cr, placed.data(), static_cast<int>(placed.size())); });
}

} // namespace alchemy::host::gpu
