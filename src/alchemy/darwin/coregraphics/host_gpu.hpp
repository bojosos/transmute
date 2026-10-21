#pragma once

// Quartz 2D values as host::gpu values and back (include/alchemy/host/gpu.hpp):
// affine transforms, points and paths.

#include "alchemy/darwin/coregraphics/q2d_internal.hpp"
#include "alchemy/host/gpu.hpp"

#include <span>
#include <vector>

namespace alchemy::gui::quartz2d {

[[nodiscard]] inline host::gpu::Matrix to_host(const Affine& m) { return {m.a, m.b, m.c, m.d, m.tx, m.ty}; }
[[nodiscard]] inline host::gpu::Point to_host(Point p) { return {p.x, p.y}; }

// The elements of `path`, mapped through `transform`.
[[nodiscard]] inline std::vector<host::gpu::PathElement> to_host(const PathData& path, const Affine& transform) {
    std::vector<host::gpu::PathElement> elements;
    elements.reserve(path.elements.size());
    for (const auto& element : path.elements) {
        host::gpu::PathElement out;
        out.op = static_cast<host::gpu::PathOp>(element.op);
        for (unsigned index = 0; index < element.count(); ++index) {
            const auto p = transform.apply(element.points[index]);
            out.points[index] = {p.x, p.y};
        }
        elements.push_back(out);
    }
    return elements;
}

// Appends host path elements (moves, lines, quads, cubics, closes) to `path`.
inline void append(PathData& path, std::span<const host::gpu::PathElement> elements) {
    const auto at = [](const host::gpu::Point& p) { return Point{p.x, p.y}; };
    for (const auto& element : elements) {
        switch (element.op) {
        case host::gpu::PathOp::move: path.move_to(at(element.points[0])); break;
        case host::gpu::PathOp::line: path.line_to(at(element.points[0])); break;
        case host::gpu::PathOp::quad: path.quad_to(at(element.points[0]), at(element.points[1])); break;
        case host::gpu::PathOp::cubic: path.cubic_to(at(element.points[0]), at(element.points[1]), at(element.points[2])); break;
        case host::gpu::PathOp::close: path.close(); break;
        }
    }
}

} // namespace alchemy::gui::quartz2d
