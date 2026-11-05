// CALayer tree composition (layer_tree.hpp, owner: w3): geometry, flipping,
// gravity, clipping, opacity and ordering on host pixels, no guest; (g3)
// contentsRect.
#include "alchemy/darwin/quartzcore/layer_tree.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <memory>
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

bool near(const Pixel& got, const Pixel& want, int tolerance = 8) {
    for (int i = 0; i < 4; ++i) if (std::abs(static_cast<int>(got[i]) - static_cast<int>(want[i])) > tolerance) return false;
    return true;
}

// Rotated layers are drawn as groups, resampled bilinearly; masks multiply
// by the mask layer's alpha; scaled contents follow the layer's filters.
void rotations_masks_and_filters() {
    const double r = std::sqrt(0.5);
    // A 10x10 blue square turned 45 degrees about its centre (10, 10) over red: a diamond.
    auto root = layer({{0, 0}, {20, 20}}, {0, 0});
    root.background = rgba(red);
    auto diamond = layer({{0, 0}, {10, 10}}, {10, 10}, {0.5, 0.5});
    diamond.transform = {r, r, -r, r, 0, 0};
    diamond.background = rgba(blue);
    root.sublayers = {diamond};
    Canvas canvas(20, 20);
    auto target = canvas.target();
    const auto stats = t::compose(root, {{{0, 0}, {20, 20}}, 20, 1}, target, {});
    require(stats.unsupported_transforms == 0, "a rotated layer is drawn");
    canvas.expect(10, 10, blue, "the diamond's centre");
    canvas.expect(10, 4, blue, "its top point region (6 above the centre, inside the 7.07 half-diagonal)");
    canvas.expect(15, 10, blue, "its right point region");
    canvas.expect(5, 5, red, "an unrotated square's corner is outside the diamond");
    canvas.expect(15, 15, red, "the other corner");
    const auto edge = canvas.at(13, 13);   // on the diamond's edge: antialiased
    require(edge[0] > 0 && edge[0] < 255 && edge[2] > 0 && edge[3] == 255, "the rotated edge is antialiased: " + text(edge));

    // A rotation inside a rotation: the inner layer turned back is axis-aligned again.
    auto outer = layer({{0, 0}, {20, 20}}, {10, 10}, {0.5, 0.5});
    outer.transform = {r, r, -r, r, 0, 0};
    auto inner = layer({{0, 0}, {6, 6}}, {10, 10}, {0.5, 0.5});
    inner.transform = {r, -r, r, r, 0, 0};
    inner.background = rgba(green);
    outer.sublayers = {inner};
    root.sublayers = {outer};
    Canvas nested(20, 20);
    auto nested_target = nested.target();
    t::compose(root, {{{0, 0}, {20, 20}}, 20, 1}, nested_target, {});
    nested.expect(8, 8, green, "the inner square, upright");
    nested.expect(11, 11, green, "its other side");
    nested.expect(10, 5, red, "outside it");

    // A mask: an image whose top half is opaque and bottom half clear keeps the top of a green layer.
    const Picture half(20, 20, white, clear);
    auto masked = layer({{0, 0}, {20, 20}}, {0, 0});
    masked.background = rgba(green);
    auto mask = layer({{0, 0}, {20, 20}}, {0, 0});
    mask.contents = half.image(false);
    masked.mask = std::make_shared<const t::Layer>(mask);
    root.sublayers = {masked};
    Canvas masking(20, 20);
    auto masking_target = masking.target();
    t::compose(root, {{{0, 0}, {20, 20}}, 20, 1}, masking_target, {});
    masking.expect(3, 3, green, "the mask's opaque top keeps the layer");
    masking.expect(16, 8, green, "down to the middle");
    masking.expect(3, 16, red, "its clear bottom hides it");
    // A half-transparent mask (a background of alpha 0.5) halves the layer.
    auto faded = layer({{0, 0}, {20, 20}}, {0, 0});
    faded.background = {1, 1, 1, 0.5F};
    masked.mask = std::make_shared<const t::Layer>(faded);
    root.sublayers = {masked};
    Canvas fading(20, 20);
    auto fading_target = fading.target();
    t::compose(root, {{{0, 0}, {20, 20}}, 20, 1}, fading_target, {});
    require(near(fading.at(10, 10), {0, 128, 127, 255}), "a half-alpha mask blends the layer over what is below: " + text(fading.at(10, 10)));

    // Filters: a 2x1 image (red, blue) stretched over 20x10 pixels.
    std::vector<std::uint8_t> two{0, 0, 255, 255, 255, 0, 0, 255};
    auto stretched = layer({{0, 0}, {20, 10}}, {0, 0});
    stretched.contents = t::Image{2, 1, 8, two.data(), true};
    root.sublayers = {stretched};
    Canvas linear(20, 20);
    auto linear_target = linear.target();
    t::compose(root, {{{0, 0}, {20, 20}}, 20, 1}, linear_target, {});
    const auto middle = linear.at(10, 15);
    require(middle[0] > 80 && middle[0] < 175 && middle[2] > 80 && middle[2] < 175, "kCAFilterLinear blends across the middle: " + text(middle));
    linear.expect(0, 15, red, "the left edge stays red");
    linear.expect(19, 15, blue, "the right edge stays blue");
    root.sublayers[0].nearest_magnification = true;
    Canvas nearest(20, 20);
    auto nearest_target = nearest.target();
    t::compose(root, {{{0, 0}, {20, 20}}, 20, 1}, nearest_target, {});
    nearest.expect(9, 15, red, "kCAFilterNearest: the left half red");
    nearest.expect(10, 15, blue, "and the right half blue");
}

// 3D: perspective (a sublayerTransform's m34) projects a turned layer onto its
// superlayer's plane; a CATransformLayer keeps its sublayers in 3D, drawn back
// to front by depth, and draws nothing itself.
void perspective_and_transform_layers() {
    const double pi = 3.14159265358979323846;
    const double c = std::cos(pi / 3), s = std::sin(pi / 3);
    // Row vectors (Core Animation's layout): a turn of 60 degrees about the y axis.
    const std::array<double, 16> turn{c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, 0, 0, 0, 1};
    const auto blue_column_height = [](const Canvas& canvas, long x) {
        long count = 0;
        for (long y = 0; y < static_cast<long>(canvas.height); ++y) {
            const auto p = canvas.at(x, y);
            if (p[0] > 128 && p[2] < 128) ++count;
        }
        return count;
    };
    const auto blue_columns = [&](const Canvas& canvas) {
        std::vector<long> columns;
        for (long x = 0; x < static_cast<long>(canvas.width); ++x) if (blue_column_height(canvas, x) > 0) columns.push_back(x);
        return columns;
    };
    auto root = layer({{0, 0}, {40, 40}}, {0, 0});
    root.background = rgba(red);
    auto card = layer({{0, 0}, {20, 20}}, {20, 20}, {0.5, 0.5});
    card.background = rgba(blue);
    card.transform3d = turn;
    root.sublayers = {card};
    // Orthographic: half as wide (cos 60), both edges as tall.
    Canvas flat(40, 40);
    auto flat_target = flat.target();
    auto stats = t::compose(root, {{{0, 0}, {40, 40}}, 40, 1}, flat_target, {});
    auto columns = blue_columns(flat);
    require(stats.unsupported_transforms == 0 && columns.size() >= 9 && columns.size() <= 12, "orthographic width: " + std::to_string(columns.size()));
    const auto flat_left = blue_column_height(flat, columns.front() + 1), flat_right = blue_column_height(flat, columns.back() - 1);
    require(std::abs(flat_left - flat_right) <= 1, "orthographic: equal edges " + std::to_string(flat_left) + " / " + std::to_string(flat_right));
    // Perspective (m34 = -1/40 in the superlayer's sublayerTransform): the nearer edge is taller.
    std::array<double, 16> perspective{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, -1.0 / 40, 0, 0, 0, 1};
    root.sublayer_transform3d = perspective;
    root.anchor = {0.5, 0.5};
    root.position = {20, 20};
    Canvas deep(40, 40);
    auto deep_target = deep.target();
    stats = t::compose(root, {{{0, 0}, {40, 40}}, 40, 1}, deep_target, {});
    columns = blue_columns(deep);
    require(stats.unsupported_transforms == 0 && columns.size() >= 6, "the perspective card is drawn: " + std::to_string(columns.size()));
    const auto left = blue_column_height(deep, columns.front() + 1), right = blue_column_height(deep, columns.back() - 1);
    require(std::abs(left - right) >= 3, "perspective: the nearer edge is taller " + std::to_string(left) + " / " + std::to_string(right));

    // Two overlapping squares 5 points in front of and behind their parent's plane: the later (behind) one in array order.
    auto front = layer({{0, 0}, {10, 10}}, {20, 20}, {0.5, 0.5});
    front.background = rgba(green);
    front.transform3d = std::array<double, 16>{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 5, 1};
    auto back = layer({{0, 0}, {10, 10}}, {20, 20}, {0.5, 0.5});
    back.background = rgba(blue);
    back.transform3d = std::array<double, 16>{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, -5, 1};
    auto plain = layer({{0, 0}, {40, 40}}, {0, 0});
    plain.background = rgba(white);
    auto holder = layer({{0, 0}, {40, 40}}, {0, 0});
    holder.sublayers = {front, back};
    plain.sublayers = {holder};
    Canvas flattened(40, 40);
    auto flattened_target = flattened.target();
    t::compose(plain, {{{0, 0}, {40, 40}}, 40, 1}, flattened_target, {});
    flattened.expect(20, 20, blue, "a flattening layer draws its sublayers in order");
    // In a CATransformLayer the nearer one is in front, and its own background is not drawn.
    holder.transform_layer = true;
    holder.background = rgba(red);
    plain.sublayers = {holder};
    Canvas three_d(40, 40);
    auto three_d_target = three_d.target();
    t::compose(plain, {{{0, 0}, {40, 40}}, 40, 1}, three_d_target, {});
    three_d.expect(20, 20, green, "a CATransformLayer draws the nearer sublayer in front");
    three_d.expect(5, 5, white, "and not its own background");
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
    require(stats.unsupported_transforms == 0, "a quarter turn is drawn");
    second.expect(14, 6, blue, "the quarter-turned square");
    second.expect(9, 10, green, "a scale transform is drawn (bottom-left 10x10)");
    second.expect(16, 9, red, "beyond the scaled and turned layers");
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
        rotations_masks_and_filters();
        perspective_and_transform_layers();
        contents_rect();
        corners_borders_shadows();
    } catch (const std::exception& error) {
        std::cerr << "layer tree test failed: " << error.what() << "\n";
        return 1;
    }
    std::cout << "Alchemy GUI QuartzCore layer tree passed\n";
    return 0;
}
