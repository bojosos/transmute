// CALayer tree composition (layer_tree.hpp, owner: w3): geometry, flipping,
// gravity, clipping, opacity and ordering on host pixels, no guest; (g3)
// contentsRect.
#include "alchemy/darwin/quartzcore/layer_tree.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace t = alchemy::gui::quartzcore::tree;
using alchemy::gui::Point;
using alchemy::gui::Rect;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

using Pixel = std::array<std::uint8_t, 4>;   // B, G, R, A
constexpr Pixel red{0, 0, 255, 255}, green{0, 255, 0, 255}, blue{255, 0, 0, 255}, white{255, 255, 255, 255}, clear{0, 0, 0, 0};

std::string text(const Pixel& p) {
    char buffer[48];
    std::snprintf(buffer, sizeof buffer, "bgra(%u,%u,%u,%u)", p[0], p[1], p[2], p[3]);
    return buffer;
}

struct Canvas {
    std::uint32_t width, height;
    std::vector<std::uint8_t> bytes;
    Canvas(std::uint32_t w, std::uint32_t h) : width(w), height(h), bytes(static_cast<std::size_t>(w) * h * 4, 0) {}
    t::Target target() { return {width, height, static_cast<std::size_t>(width) * 4, bytes.data()}; }
    Pixel at(long x, long y) const {
        const auto* p = bytes.data() + (static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)) * 4;
        return {p[0], p[1], p[2], p[3]};
    }
    void expect(long x, long y, const Pixel& want, const std::string& what) const {
        const auto got = at(x, y);
        require(got == want, what + " at (" + std::to_string(x) + "," + std::to_string(y) + "): " + text(got) + " want " + text(want));
    }
};

// An image whose top half is `top` and bottom half `bottom` (opaque).
struct Picture {
    std::uint32_t width, height;
    std::vector<std::uint8_t> bytes;
    Picture(std::uint32_t w, std::uint32_t h, const Pixel& top, const Pixel& bottom) : width(w), height(h), bytes(static_cast<std::size_t>(w) * h * 4) {
        for (std::uint32_t y = 0; y < h; ++y)
            for (std::uint32_t x = 0; x < w; ++x) {
                const auto& p = y < h / 2 ? top : bottom;
                std::copy(p.begin(), p.end(), bytes.begin() + (static_cast<std::ptrdiff_t>(y) * w + x) * 4);
            }
    }
    t::Image image(bool opaque = true) const { return {width, height, static_cast<std::size_t>(width) * 4, bytes.data(), opaque}; }
};

t::Layer layer(Rect bounds, Point position, Point anchor = {0, 0}) {
    t::Layer value;
    value.bounds = bounds;
    value.position = position;
    value.anchor = anchor;
    return value;
}
std::array<float, 4> rgba(const Pixel& p) { return {p[2] / 255.0F, p[1] / 255.0F, p[0] / 255.0F, p[3] / 255.0F}; }

// Chrome's DisplayCALayerTree: the view's layer, a geometry-flipped layer
// filling it, and an IOSurface layer at its origin with topLeft gravity.
void chrome_tree_shows_the_surface_top_left() {
    const Picture surface(50, 40, red, blue);
    auto root = layer({{0, 0}, {100, 80}}, {0, 0});
    root.background = rgba(white);
    auto flipped = layer({{0, 0}, {100, 80}}, {0, 0});
    flipped.geometry_flipped = true;
    auto content = layer({{0, 0}, {50, 40}}, {0, 0});
    content.contents = surface.image();
    content.gravity = t::Gravity::top_left;
    flipped.sublayers.push_back(content);
    root.sublayers.push_back(flipped);
    Canvas canvas(100, 80);
    auto target = canvas.target();
    const auto stats = t::compose(root, {{{0, 0}, {100, 80}}, 80, 1}, target, {});
    require(stats.images_drawn == 1 && stats.layers_drawn == 3 && stats.unsupported_transforms == 0, "three layers, one image");
    canvas.expect(0, 0, red, "the surface's first row at the window's top-left");
    canvas.expect(49, 19, red, "top half");
    canvas.expect(0, 39, blue, "the surface's last row");
    canvas.expect(50, 0, white, "right of the surface: the root's background");
    canvas.expect(10, 40, white, "below the surface");
}

void unflipped_geometry_puts_the_origin_bottom_left() {
    const Picture surface(20, 10, red, blue);
    auto root = layer({{0, 0}, {40, 30}}, {0, 0});
    auto content = layer({{0, 0}, {20, 10}}, {0, 0});
    content.contents = surface.image();
    root.sublayers.push_back(content);
    Canvas canvas(40, 30);
    auto target = canvas.target();
    t::compose(root, {{{0, 0}, {40, 30}}, 30, 1}, target, {});
    canvas.expect(0, 20, red, "an unflipped sublayer at y 0 sits at the bottom; its image upright");
    canvas.expect(0, 29, blue, "bottom row");
    canvas.expect(0, 19, clear, "above it nothing");
}

void placement_and_backing_scale() {
    auto root = layer({{0, 0}, {10, 10}}, {0, 0});
    root.background = rgba(green);
    Canvas canvas(60, 60);
    auto target = canvas.target();
    // The view at (5, 5) points in a 30-point-high content area at scale 2.
    t::compose(root, {{{5, 5}, {10, 10}}, 30, 2}, target, {});
    canvas.expect(10, 30, green, "top-left of the frame: x 5*2, y (30-15)*2");
    canvas.expect(29, 49, green, "bottom-right pixel");
    canvas.expect(30, 30, clear, "right of the frame");
    canvas.expect(10, 29, clear, "above the frame");
    canvas.expect(10, 50, clear, "below the frame");
}

void gravity_scale_and_resize() {
    const Picture surface(40, 40, red, blue);
    auto root = layer({{0, 0}, {40, 40}}, {0, 0});
    root.geometry_flipped = true;
    // contentsScale 2: the 40-pixel image is 20 points; resize stretches it.
    auto scaled = layer({{0, 0}, {40, 40}}, {0, 0});
    scaled.contents = surface.image();
    scaled.contents_scale = 2;
    scaled.gravity = t::Gravity::top_left;
    root.sublayers.push_back(scaled);
    Canvas canvas(40, 40);
    auto target = canvas.target();
    t::compose(root, {{{0, 0}, {40, 40}}, 40, 1}, target, {});
    // In the flipped space "topLeft" is the layer's max-y corner: the visual
    // bottom-left. A 2x image at scale 1 covers 20 pixels, upright.
    canvas.expect(0, 20, red, "topLeft in a flipped space: the image's top row at y 20");
    canvas.expect(19, 39, blue, "its bottom half");
    canvas.expect(19, 19, clear, "nothing above it");
    canvas.expect(20, 39, clear, "nothing right of it");

    root.sublayers[0].gravity = t::Gravity::resize;
    Canvas stretched(40, 40);
    auto stretched_target = stretched.target();
    t::compose(root, {{{0, 0}, {40, 40}}, 40, 1}, stretched_target, {});
    stretched.expect(39, 0, red, "resize fills the bounds");
    stretched.expect(39, 39, blue, "resize: bottom");

    root.sublayers[0].gravity = t::Gravity::center;
    Canvas centered(40, 40);
    auto centered_target = centered.target();
    t::compose(root, {{{0, 0}, {40, 40}}, 40, 1}, centered_target, {});
    centered.expect(10, 10, red, "center: from 10");
    centered.expect(29, 29, blue, "center: to 30");
    centered.expect(9, 9, clear, "center: outside");
    require(t::gravity_named("resizeAspectFill") == t::Gravity::resize_aspect_fill && !t::gravity_named("upward"), "gravity names");
}

void hidden_opacity_clipping_and_order() {
    auto root = layer({{0, 0}, {30, 30}}, {0, 0});
    root.geometry_flipped = true;
    root.background = rgba(white);
    auto hidden = layer({{0, 0}, {30, 30}}, {0, 0});
    hidden.background = rgba(red);
    hidden.hidden = true;
    auto clipper = layer({{0, 0}, {10, 10}}, {0, 0});
    clipper.masks_to_bounds = true;
    auto overflow = layer({{0, 0}, {20, 20}}, {0, 0});
    overflow.background = rgba(green);
    clipper.sublayers.push_back(overflow);
    // Two overlapping layers: the later one has the lower zPosition.
    auto front = layer({{0, 0}, {10, 10}}, {20, 0});
    front.background = rgba(blue);
    front.z_position = 1;
    auto back = layer({{0, 0}, {10, 10}}, {20, 0});
    back.background = rgba(red);
    // Half-transparent green over white.
    auto faded = layer({{0, 0}, {10, 10}}, {0, 20});
    faded.background = rgba(green);
    faded.opacity = 0.5F;
    root.sublayers = {hidden, clipper, front, back, faded};
    Canvas canvas(30, 30);
    auto target = canvas.target();
    t::compose(root, {{{0, 0}, {30, 30}}, 30, 1}, target, {});
    canvas.expect(15, 15, white, "a hidden layer draws nothing");
    canvas.expect(5, 5, green, "inside the clipping layer");
    canvas.expect(15, 5, white, "masksToBounds clips the overflow");
    canvas.expect(25, 5, blue, "the higher zPosition is in front");
    const auto blended = canvas.at(5, 25);
    require(blended[3] == 255 && blended[1] == 255 && blended[0] > 120 && blended[0] < 135, "opacity 0.5 blends: " + text(blended));
    // Without the root itself (AppKit drew the view).
    Canvas sublayers_only(30, 30);
    auto sublayers_target = sublayers_only.target();
    t::compose(root, {{{0, 0}, {30, 30}}, 30, 1}, sublayers_target, {}, false);
    sublayers_only.expect(15, 15, clear, "root_self false leaves the root's background out");
    sublayers_only.expect(5, 5, green, "but draws its sublayers");
}

void clip_and_unsupported_transforms() {
    auto root = layer({{0, 0}, {20, 20}}, {0, 0});
    root.background = rgba(red);
    Canvas canvas(20, 20);
    auto target = canvas.target();
    t::compose(root, {{{0, 0}, {20, 20}}, 20, 1}, target, {0, 0, 10, 10});
    canvas.expect(5, 5, red, "inside the clip");
    canvas.expect(15, 15, clear, "outside the clip nothing changes");

    auto rotated = layer({{0, 0}, {10, 10}}, {10, 10}, {0.5, 0.5});
    rotated.transform = {0, 1, -1, 0, 0, 0};
    rotated.background = rgba(blue);
    auto scaled = layer({{0, 0}, {5, 5}}, {0, 0});
    scaled.transform = {2, 0, 0, 2, 0, 0};   // about the anchor (0, 0): 10x10
    scaled.background = rgba(green);
    root.sublayers = {rotated, scaled};
    Canvas second(20, 20);
    auto second_target = second.target();
    const auto stats = t::compose(root, {{{0, 0}, {20, 20}}, 20, 1}, second_target, {});
    require(stats.unsupported_transforms == 1, "a rotation is skipped and counted");
    second.expect(9, 10, green, "a scale transform is drawn (bottom-left 10x10)");
    second.expect(10, 9, red, "beyond the scaled layer");
}

} // namespace

// g3: contentsRect selects part of the contents. Its y axis is the layer's:
// in a flipped layer (Chrome's tree) y = 0 is the image's top, in an
// unflipped one its bottom. Its size is what gravity places; outside the
// unit square the edge pixels repeat; an empty rectangle draws nothing.
void contents_rect() {
    const Picture surface(40, 40, red, blue);   // red top half, blue bottom half
    const auto compose = [&](bool flipped, Rect rect, t::Gravity gravity) {
        auto root = layer({{0, 0}, {40, 40}}, {0, 0});
        root.background = rgba(white);
        root.geometry_flipped = flipped;
        auto content = layer({{0, 0}, {40, 40}}, {0, 0});
        content.contents = surface.image();
        content.gravity = gravity;
        content.contents_rect = rect;
        root.sublayers.push_back(content);
        Canvas canvas(40, 40);
        auto target = canvas.target();
        t::compose(root, {{{0, 0}, {40, 40}}, 40, 1}, target, {});
        return canvas;
    };
    auto canvas = compose(true, {{0, 0}, {1, 1}}, t::Gravity::resize);
    canvas.expect(0, 0, red, "the unit rectangle: the whole image (top)");
    canvas.expect(0, 39, blue, "the unit rectangle: the whole image (bottom)");
    canvas = compose(true, {{0, 0.5}, {1, 0.5}}, t::Gravity::resize);
    canvas.expect(0, 0, blue, "flipped: y 0.5..1 is the image's bottom half, stretched");
    canvas.expect(39, 39, blue, "flipped: all of it");
    canvas = compose(false, {{0, 0}, {1, 0.5}}, t::Gravity::resize);
    canvas.expect(0, 0, blue, "unflipped: y 0..0.5 is the image's bottom half");
    canvas.expect(39, 39, blue, "unflipped: all of it");
    canvas = compose(false, {{0, 0.5}, {1, 0.5}}, t::Gravity::resize);
    canvas.expect(20, 20, red, "unflipped: y 0.5..1 is the top half");
    // The sub-rectangle's size is what gravity places: 40x20 pixels at the
    // top (unflipped: "top" is the visual top; y 0.5..1 is the top half).
    canvas = compose(false, {{0, 0.5}, {1, 0.5}}, t::Gravity::top_left);
    canvas.expect(0, 0, red, "topLeft: the top half at the top");
    canvas.expect(39, 19, red, "topLeft: 20 rows of it");
    canvas.expect(0, 20, white, "topLeft: below it the background");
    // A quarter of the image (its top-left 20x20 pixels) stretched over 40x40.
    canvas = compose(true, {{0.5, 0.5}, {0.5, 0.5}}, t::Gravity::resize);
    canvas.expect(0, 0, blue, "flipped: x 0.5.., y 0.5.. is part of the bottom half");
    // Outside the unit square the edge rows repeat: y -0.5..0.5 is all red.
    canvas = compose(true, {{0, -0.5}, {1, 1}}, t::Gravity::resize);
    canvas.expect(0, 0, red, "above the image: its first row repeated");
    canvas.expect(39, 39, red, "y up to 0.5: still the top half");
    // An empty rectangle draws nothing.
    canvas = compose(true, {{0, 0}, {0, 0.5}}, t::Gravity::resize);
    canvas.expect(0, 0, white, "an empty contentsRect draws nothing");
    std::cout << "contents rect ok\n";
}

// Rounded backgrounds, borders inside the rounded bounds, rounded clipping
// of sublayers, shadows and contents frames (frameworks wave U2).
void corners_borders_shadows() {
    // A 20 x 20 red box with 8-point corners and a 2-point blue border at (10, 10).
    auto root = layer({{0, 0}, {40, 40}}, {0, 0});
    auto box = layer({{0, 0}, {20, 20}}, {10, 10});
    box.background = rgba(red);
    box.corner_radius = 8;
    box.border_width = 2;
    box.border = rgba(blue);
    root.sublayers.push_back(box);
    Canvas canvas(40, 40);
    auto target = canvas.target();
    t::compose(root, {{{0, 0}, {40, 40}}, 40, 1}, target, {});
    canvas.expect(10, 10, clear, "the rounded corner is left out");
    canvas.expect(20, 20, red, "the inside is the background");
    canvas.expect(20, 10, blue, "the border along the top edge");
    canvas.expect(20, 29, blue, "the border along the bottom edge");
    canvas.expect(13, 20, red, "inside the border");

    // Only the bottom-left corner of the layer (kCALayerMinXMinYCorner) rounded.
    box.masked_corners = 1;
    box.border.reset();
    root.sublayers = {box};
    Canvas one(40, 40);
    auto one_target = one.target();
    t::compose(root, {{{0, 0}, {40, 40}}, 40, 1}, one_target, {});
    one.expect(10, 29, clear, "min-x min-y is the visual bottom-left of an unflipped layer");
    one.expect(10, 10, red, "the other corners stay square");

    // masksToBounds with a corner radius clips the sublayers to the rounded bounds.
    auto clip = layer({{0, 0}, {20, 20}}, {10, 10});
    clip.masks_to_bounds = true;
    clip.corner_radius = 8;
    auto fill = layer({{0, 0}, {20, 20}}, {0, 0});
    fill.background = rgba(green);
    clip.sublayers.push_back(fill);
    root.sublayers = {clip};
    Canvas clipped(40, 40);
    auto clipped_target = clipped.target();
    t::compose(root, {{{0, 0}, {40, 40}}, 40, 1}, clipped_target, {});
    clipped.expect(10, 10, clear, "the sublayer is clipped at the rounded corner");
    clipped.expect(20, 20, green, "and shown inside");

    // A crisp shadow (no blur) offset right and down.
    auto lit = layer({{0, 0}, {10, 10}}, {5, 25});
    lit.background = rgba(red);
    lit.shadow = t::Shadow{{0, 0, 0, 1}, 1.0F, {5, -5}, 0, std::nullopt};
    root.sublayers = {lit};
    Canvas shadowed(40, 40);
    auto shadowed_target = shadowed.target();
    t::compose(root, {{{0, 0}, {40, 40}}, 40, 1}, shadowed_target, {});
    shadowed.expect(7, 7, red, "the layer over its shadow");
    shadowed.expect(17, 17, {0, 0, 0, 255}, "the shadow right of and below the layer");
    shadowed.expect(30, 30, clear, "nothing beyond the shadow");

    // Owned contents drawn at their frame, beyond the bounds.
    const Picture picture(4, 4, green, green);
    auto shape = layer({{0, 0}, {10, 10}}, {10, 10});
    shape.contents = picture.image(false);
    shape.contents_frame = Rect{{-5, -5}, {20, 20}};
    root.sublayers = {shape};
    Canvas framed(40, 40);
    auto framed_target = framed.target();
    t::compose(root, {{{0, 0}, {40, 40}}, 40, 1}, framed_target, {});
    framed.expect(6, 33, green, "contents outside the bounds at their frame");
}

int main() {
    try {
        chrome_tree_shows_the_surface_top_left();
        unflipped_geometry_puts_the_origin_bottom_left();
        placement_and_backing_scale();
        gravity_scale_and_resize();
        hidden_opacity_clipping_and_order();
        clip_and_unsupported_transforms();
        contents_rect();
        corners_borders_shadows();
    } catch (const std::exception& error) {
        std::cerr << "layer tree test failed: " << error.what() << "\n";
        return 1;
    }
    std::cout << "Alchemy GUI QuartzCore layer tree passed\n";
    return 0;
}
