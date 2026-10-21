#pragma once

// CALayer tree composition (owner: w3 compositing). Pure C++ over a snapshot
// of a layer tree; no Objective-C, no guest memory. layer_contents.cpp takes
// the snapshot of a view's backing layer and its sublayers on CATransaction
// commit and presents the result in the view's window.
//
// Model (Core Animation's render tree, flattened):
// - A layer's bounds space maps to its superlayer's space through
//   position + transform * (point - (bounds.origin + anchorPoint * size)).
//   A superlayer with geometryFlipped interprets its sublayers' geometry
//   flipped vertically within its bounds; sublayerTransform applies about the
//   superlayer's anchor point. The root's bounds space maps to its frame in
//   the target (Placement).
// - Each layer draws, back to front: backgroundColor over its bounds, then
//   its contents placed by contentsGravity (contentsScale pixels per point),
//   then its sublayers in zPosition order (stable). Contents are drawn
//   upright (the image's first row at the visual top: geometryFlipped never
//   flips the image); the vertical gravities name the layer's own axis, so
//   "top" is the visual bottom where the layer's space is flipped
//   (contentsAreFlipped), as QuartzCore documents. contentsRect (unit
//   coordinates of the contents, {0,0,1,1} by default) selects the part of
//   the image that is drawn: its size is what gravity and contentsScale
//   place; its y axis is the layer's, so y = 0 is the image's visual top
//   where the layer's space is flipped and its bottom otherwise *(design:
//   Chrome's CALayerTreeCoordinator gives top-left texture rectangles to
//   sublayers of a geometryFlipped layer)*; outside the image the edge
//   pixels repeat; an empty rectangle draws nothing.
//   masksToBounds clips contents and sublayers to the bounds. Hidden layers
//   draw nothing; opacity multiplies down the tree (per layer, not as a
//   group). Pixels are premultiplied BGRA composited source-over; opaque
//   images and colours replace.
// - Only axis-aligned transforms (scale and translation, including flips)
//   are drawn; a layer whose accumulated transform rotates or shears is
//   skipped with its sublayers and counted in Stats::unsupported_transforms.
//   Sampling is nearest-neighbour.

#include "alchemy/gui/window_server.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace alchemy::gui::quartzcore::tree {

// x' = a x + c y + tx, y' = b x + d y + ty (CGAffineTransform's layout).
struct Affine {
    double a{1}, b{0}, c{0}, d{1}, tx{0}, ty{0};
    [[nodiscard]] Point apply(Point p) const { return {a * p.x + c * p.y + tx, b * p.x + d * p.y + ty}; }
    // this after `inner`: p -> this(inner(p)).
    [[nodiscard]] Affine after(const Affine& inner) const {
        return {a * inner.a + c * inner.b, b * inner.a + d * inner.b, a * inner.c + c * inner.d, b * inner.c + d * inner.d,
                a * inner.tx + c * inner.ty + tx, b * inner.tx + d * inner.ty + ty};
    }
    [[nodiscard]] static Affine translation(double x, double y) { return {1, 0, 0, 1, x, y}; }
    [[nodiscard]] bool identity() const { return a == 1 && b == 0 && c == 0 && d == 1 && tx == 0 && ty == 0; }
};

// kCAGravity* (the string constants' values: "center", "top", ...).
enum class Gravity { center, top, bottom, left, right, top_left, top_right, bottom_left, bottom_right, resize, resize_aspect,
                     resize_aspect_fill };
[[nodiscard]] std::optional<Gravity> gravity_named(std::string_view name);

// Premultiplied BGRA8 pixels, first row on top.
struct Image {
    std::uint32_t width{}, height{};
    std::size_t stride{};
    const std::uint8_t* bgra{};
    bool opaque{};   // every alpha is 255 (copied without blending)
};

struct Layer {
    Rect bounds;
    Point position, anchor{0.5, 0.5};
    Affine transform, sublayer_transform;   // the 2D parts of the CATransform3Ds
    double z_position{}, contents_scale{1};
    float opacity{1};
    bool hidden{}, geometry_flipped{}, masks_to_bounds{};
    std::optional<std::array<float, 4>> background;   // straight RGBA, 0..1 (sRGB)
    std::optional<Image> contents;
    Gravity gravity{Gravity::resize};
    Rect contents_rect{{0, 0}, {1, 1}};   // unit coordinates (see the model above)
    std::uint64_t contents_object{};   // the guest contents object (callers resolve it to `contents`)
    std::vector<Layer> sublayers;
};

// Where the root layer is shown: its bounds fill `frame` (points, bottom-left
// origin, in a target `content_height` points high) at `scale` pixels per point.
struct Placement {
    Rect frame;
    double content_height{};
    double scale{1};
};

// The target pixels (premultiplied BGRA, rows top to bottom) and the part of
// them that may change (pixels; empty: all).
struct Target {
    std::uint32_t width{}, height{};
    std::size_t stride{};
    std::uint8_t* bgra{};
};
struct PixelRect {
    long x0{}, y0{}, x1{}, y1{};
    [[nodiscard]] bool empty() const { return x1 <= x0 || y1 <= y0; }
    [[nodiscard]] PixelRect intersect(const PixelRect& other) const;
};

struct Stats {
    std::uint32_t layers_drawn{}, images_drawn{}, unsupported_transforms{};
};

// The root's bounds space -> target pixels (y down).
[[nodiscard]] Affine root_transform(const Layer& root, const Placement& placement);

// Composites `root` and its sublayers into `target` within `clip`. With
// `root_self` false the root's own background and contents are left out (a
// view whose drawing AppKit already put in the target) and only its sublayers
// are drawn.
Stats compose(const Layer& root, const Placement& placement, Target& target, PixelRect clip, bool root_self = true);

} // namespace alchemy::gui::quartzcore::tree
