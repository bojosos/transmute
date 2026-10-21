#include "alchemy/darwin/coregraphics/surface.hpp"

#include <algorithm>

// CGPath / CGMutablePath functions.
namespace alchemy::gui::quartz2d {
Address Quartz::make_path(PathData data, bool mutable_path) {
    auto info = std::make_shared<PathInfo>();
    info->data = std::move(data);
    info->mutable_path = mutable_path;
    return create(mutable_path ? mutable_path_class : path_class, info);
}

void Quartz::install_paths() {
    const auto cg = core_graphics_library;
    path_class = declare_class("_AlchemyCGPath");
    mutable_path_class = declare_class("_AlchemyCGMutablePath", "_AlchemyCGPath");
    const auto path = [this](Address object) { return require<PathInfo>(object, "CGPath"); };
    const auto editable = [this](Address object) {
        const auto info = require<PathInfo>(object, "CGPath");
        if (!info->mutable_path) fail("modifying an immutable CGPath");
        return info;
    };
    const auto matrix = [this](Address address) { return address ? read_affine(address) : Affine{}; };

    function(cg, "_CGPathCreateMutable", [this](CpuState& s, GuestMemory&) { s.x[0] = make_path({}, true); });
    function(cg, "_CGPathCreateMutableCopy", [this, path](CpuState& s, GuestMemory&) { s.x[0] = s.x[0] ? make_path(path(s.x[0])->data, true) : 0; });
    function(cg, "_CGPathCreateCopy", [this, path](CpuState& s, GuestMemory&) { s.x[0] = s.x[0] ? make_path(path(s.x[0])->data, false) : 0; });
    function(cg, "_CGPathCreateCopyByTransformingPath", [this, path, matrix](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[0] ? make_path(path(s.x[0])->data.transformed(matrix(s.x[1])), false) : 0;
    });
    function(cg, "_CGPathCreateMutableCopyByTransformingPath", [this, path, matrix](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[0] ? make_path(path(s.x[0])->data.transformed(matrix(s.x[1])), true) : 0;
    });
    function(cg, "_CGPathCreateWithRect", [this, matrix](CpuState& s, GuestMemory&) {
        PathData data;
        data.add_rect(matrix(s.x[0]), rect_arg(s));
        s.x[0] = make_path(std::move(data), false);
    });
    function(cg, "_CGPathCreateWithEllipseInRect", [this, matrix](CpuState& s, GuestMemory&) {
        PathData data;
        data.add_ellipse(matrix(s.x[0]), rect_arg(s));
        s.x[0] = make_path(std::move(data), false);
    });
    function(cg, "_CGPathCreateWithRoundedRect", [this, matrix](CpuState& s, GuestMemory&) {
        PathData data;
        const auto r = standardize(rect_arg(s));
        const auto rx = d(s, 4), ry = d(s, 5);
        if (rx < 0 || ry < 0 || 2 * rx > r.size.width + 1e-9 || 2 * ry > r.size.height + 1e-9)
            fail("CGPathCreateWithRoundedRect: corner size exceeds the rectangle");
        data.add_rounded_rect(matrix(s.x[0]), r, rx, ry);
        s.x[0] = make_path(std::move(data), false);
    });
    function(cg, "_CGPathMoveToPoint", [editable, matrix](CpuState& s, GuestMemory&) {
        editable(s.x[0])->data.move_to(matrix(s.x[1]).apply(point_arg(s)));
    });
    function(cg, "_CGPathAddLineToPoint", [editable, matrix](CpuState& s, GuestMemory&) {
        editable(s.x[0])->data.line_to(matrix(s.x[1]).apply(point_arg(s)));
    });
    function(cg, "_CGPathAddQuadCurveToPoint", [editable, matrix](CpuState& s, GuestMemory&) {
        const auto m = matrix(s.x[1]);
        editable(s.x[0])->data.quad_to(m.apply(point_arg(s)), m.apply(point_arg(s, 2)));
    });
    function(cg, "_CGPathAddCurveToPoint", [editable, matrix](CpuState& s, GuestMemory&) {
        const auto m = matrix(s.x[1]);
        editable(s.x[0])->data.cubic_to(m.apply(point_arg(s)), m.apply(point_arg(s, 2)), m.apply(point_arg(s, 4)));
    });
    function(cg, "_CGPathCloseSubpath", [editable](CpuState& s, GuestMemory&) { editable(s.x[0])->data.close(); });
    function(cg, "_CGPathAddRect", [editable, matrix](CpuState& s, GuestMemory&) { editable(s.x[0])->data.add_rect(matrix(s.x[1]), rect_arg(s)); });
    function(cg, "_CGPathAddRects", [this, editable, matrix](CpuState& s, GuestMemory&) {
        const auto info = editable(s.x[0]);
        const auto m = matrix(s.x[1]);
        for (Address index = 0; index < s.x[3]; ++index) info->data.add_rect(m, read_rect(s.x[2] + index * 32));
    });
    function(cg, "_CGPathAddLines", [this, editable, matrix](CpuState& s, GuestMemory&) {
        const auto info = editable(s.x[0]);
        const auto m = matrix(s.x[1]);
        for (Address index = 0; index < s.x[3]; ++index) {
            const auto p = m.apply(read_point(s.x[2] + index * 16));
            if (index == 0) info->data.move_to(p); else info->data.line_to(p);
        }
    });
    function(cg, "_CGPathAddEllipseInRect", [editable, matrix](CpuState& s, GuestMemory&) { editable(s.x[0])->data.add_ellipse(matrix(s.x[1]), rect_arg(s)); });
    function(cg, "_CGPathAddRoundedRect", [editable, matrix](CpuState& s, GuestMemory&) {
        editable(s.x[0])->data.add_rounded_rect(matrix(s.x[1]), rect_arg(s), d(s, 4), d(s, 5));
    });
    function(cg, "_CGPathAddArc", [editable, matrix](CpuState& s, GuestMemory&) {
        editable(s.x[0])->data.add_arc(matrix(s.x[1]), point_arg(s), d(s, 2), d(s, 3), d(s, 4), bool_arg(s, 2));
    });
    function(cg, "_CGPathAddRelativeArc", [editable, matrix](CpuState& s, GuestMemory&) {
        editable(s.x[0])->data.add_relative_arc(matrix(s.x[1]), point_arg(s), d(s, 2), d(s, 3), d(s, 4));
    });
    function(cg, "_CGPathAddArcToPoint", [editable, matrix](CpuState& s, GuestMemory&) {
        editable(s.x[0])->data.add_arc_to_point(matrix(s.x[1]), point_arg(s), point_arg(s, 2), d(s, 4));
    });
    function(cg, "_CGPathAddPath", [editable, path, matrix](CpuState& s, GuestMemory&) {
        if (!s.x[2]) return;
        const auto source = path(s.x[2])->data;   // copy: a path may be added to itself
        editable(s.x[0])->data.append(source, matrix(s.x[1]));
    });
    function(cg, "_CGPathIsEmpty", [this](CpuState& s, GuestMemory&) {
        const auto info = find<PathInfo>(s.x[0]);
        return_bool(s, !info || info->data.empty());
    });
    function(cg, "_CGPathIsRect", [this, path](CpuState& s, GuestMemory&) {
        const auto rect = s.x[0] ? path(s.x[0])->data.as_rect() : std::nullopt;
        if (rect && s.x[1]) {
            write_double(s.x[1], rect->origin.x); write_double(s.x[1] + 8, rect->origin.y);
            write_double(s.x[1] + 16, rect->size.width); write_double(s.x[1] + 24, rect->size.height);
        }
        return_bool(s, rect.has_value());
    });
    function(cg, "_CGPathGetCurrentPoint", [path](CpuState& s, GuestMemory&) {
        const auto info = s.x[0] ? path(s.x[0]) : nullptr;
        return_point(s, info && info->data.has_current ? info->data.current : Point{});
    });
    function(cg, "_CGPathGetBoundingBox", [path](CpuState& s, GuestMemory&) { return_rect(s, s.x[0] ? path(s.x[0])->data.control_bounds() : null_rect()); });
    function(cg, "_CGPathGetPathBoundingBox", [path](CpuState& s, GuestMemory&) { return_rect(s, s.x[0] ? path(s.x[0])->data.path_bounds() : null_rect()); });
    function(cg, "_CGPathContainsPoint", [path, matrix](CpuState& s, GuestMemory&) {
        if (!s.x[0]) { return_bool(s, false); return; }
        const auto inverse = matrix(s.x[1]).inverse();
        if (!inverse) { return_bool(s, false); return; }
        return_bool(s, path(s.x[0])->data.contains(inverse->apply(point_arg(s)), bool_arg(s, 2)));
    });
    function(cg, "_CGPathEqualToPath", [this](CpuState& s, GuestMemory&) {
        const auto a = find<PathInfo>(s.x[0]), b = find<PathInfo>(s.x[1]);
        return_bool(s, s.x[0] == s.x[1] || (a && b && a->data == b->data));
    });
    function(cg, "_CGPathRetain", [this](CpuState& s, GuestMemory&) { s.x[0] = retain(s.x[0]); });
    function(cg, "_CGPathRelease", [this](CpuState& s, GuestMemory&) { release(s.x[0]); });
    function(cg, "_CGPathGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = 0x100; });

    // CGPathApply(path, info, CGPathApplierFunction(info, const CGPathElement*)).
    const auto apply = [this, path](Address object, const std::function<void(Address element)>& call) {
        if (!object) return;
        const auto elements = path(object)->data.elements;   // the applier may mutate the path
        Scratch buffer(*this, 16 + 3 * 16);
        const auto element = buffer.address(), points = buffer.address() + 16;
        for (const auto& item : elements) {
            memory().write32(element, static_cast<std::uint32_t>(item.op));
            memory().write64(element + 8, points);
            for (unsigned index = 0; index < 3; ++index) {
                write_double(points + index * 16, item.points[index].x);
                write_double(points + index * 16 + 8, item.points[index].y);
            }
            call(element);
        }
    };
    function(cg, "_CGPathApply", [this, apply](CpuState& s, GuestMemory&) {
        const auto info = s.x[1], applier = s.x[2];
        if (!applier) return;
        apply(s.x[0], [&](Address element) { static_cast<void>(call_guest(applier, {info, element})); });
    });
    function(cg, "_CGPathApplyWithBlock", [this, apply](CpuState& s, GuestMemory& m) {
        const auto block = s.x[1];
        if (!block) return;
        const auto invoke = m.read64(block + 16);
        apply(s.x[0], [&](Address element) { static_cast<void>(call_guest(invoke, {block, element})); });
    });

    // Stroked / dashed outlines through the host's geometry widening.
    const auto widen = [](const PathData& source, const Affine& m, double width, std::uint32_t cap, std::uint32_t join, double miter,
                          std::span<const double> dashes, double phase) {
        const auto geometry = make_geometry(source, Affine{}, false, false);
        const auto outline = geometry.widened(width, stroke_style(cap, join, miter, dashes, phase, width), to_host(m));
        if (!outline) fail("widening a path failed (" + std::string(host::name(outline.error())) + ")");
        PathData path;
        append(path, *outline);
        return path;
    };
    function(cg, "_CGPathCreateCopyByStrokingPath", [this, path, matrix, widen](CpuState& s, GuestMemory&) {
        if (!s.x[0]) return;
        const auto source = path(s.x[0])->data;
        s.x[0] = make_path(widen(source, matrix(s.x[1]), d(s, 0), static_cast<std::uint32_t>(s.x[2]), static_cast<std::uint32_t>(s.x[3]),
                                 d(s, 1), {}, 0), false);
    });
    function(cg, "_CGPathCreateCopyByDashingPath", [this, path, matrix](CpuState& s, GuestMemory& m) {
        // (path, transform, phase, lengths, count): the dashed centre line.
        if (!s.x[0]) return;
        const auto source = path(s.x[0])->data;
        std::vector<double> lengths(static_cast<std::size_t>(s.x[3]));
        for (std::size_t index = 0; index < lengths.size(); ++index) lengths[index] = std::bit_cast<double>(m.read64(s.x[2] + index * 8));
        double total{};
        for (const auto value : lengths) total += std::max(0.0, value);
        const auto t = matrix(s.x[1]);
        if (total <= 0) { s.x[0] = make_path(source.transformed(t), false); return; }
        PathData out;
        for (const auto& polygon : source.flatten(0.05)) {
            // Walk each flattened subpath, toggling on/off at the dash lengths.
            double offset = std::fmod(std::max(0.0, d(s, 0)), total);
            std::size_t dash = 0;
            while (offset >= std::max(0.0, lengths[dash])) { offset -= std::max(0.0, lengths[dash]); dash = (dash + 1) % lengths.size(); }
            double remaining = std::max(0.0, lengths[dash]) - offset;
            bool on = dash % 2 == 0;
            if (on) out.move_to(t.apply(polygon.front()));
            for (std::size_t index = 1; index < polygon.size(); ++index) {
                auto a = polygon[index - 1];
                const auto b = polygon[index];
                auto segment = std::hypot(b.x - a.x, b.y - a.y);
                while (segment > remaining) {
                    const auto f = remaining / segment;
                    const Point split{a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f};
                    if (on) out.line_to(t.apply(split)); else out.move_to(t.apply(split));
                    on = !on;
                    segment -= remaining;
                    a = split;
                    dash = (dash + 1) % lengths.size();
                    remaining = std::max(0.0, lengths[dash]);
                }
                remaining -= segment;
                if (on) out.line_to(t.apply(b));
            }
        }
        s.x[0] = make_path(std::move(out), false);
    });
}

} // namespace alchemy::gui::quartz2d
