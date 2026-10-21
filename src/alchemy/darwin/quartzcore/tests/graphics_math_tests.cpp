// Host-only tests of the QLab graphics group's value math
// (kit/completion/graphics/graphics_math.cpp): CATransform3D, timing-function
// curves, NSGradient interpolation and geometry, NSAffineTransform, HSB,
// CIVector strings and GLKQuaternion slerp.
#include "alchemy/darwin/quartzcore/graphics_math.hpp"

#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string>

namespace {
namespace g = alchemy::gui::completion_kit::graphics;

void require(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error(what);
}
bool near(double a, double b, double tolerance = 1e-9) { return std::abs(a - b) <= tolerance; }
bool near(const g::Transform3D& a, const g::Transform3D& b) {
    for (std::size_t i = 0; i < 16; ++i) if (!near(a[i], b[i])) return false;
    return true;
}
// p * M for the row vector (x, y, z, 1).
std::array<double, 3> apply(const g::Transform3D& m, double x, double y, double z) {
    const double w = x * m[3] + y * m[7] + z * m[11] + m[15];
    return {(x * m[0] + y * m[4] + z * m[8] + m[12]) / w, (x * m[1] + y * m[5] + z * m[9] + m[13]) / w,
            (x * m[2] + y * m[6] + z * m[10] + m[14]) / w};
}

void transforms() {
    const auto id = g::transform_identity();
    require(g::transform_is_identity(id) && g::transform_is_affine(id), "identity");
    const auto r = g::transform_rotation(std::numbers::pi / 2, 0, 0, 1);
    const auto p = apply(r, 1, 0, 0);
    require(near(p[0], 0) && near(p[1], 1), "a rotation about +z maps x to y (counterclockwise)");
    require(near(r[0], 0) && near(r[1], 1) && near(r[4], -1) && near(r[5], 0), "rotation layout matches CGAffineTransformMakeRotation");
    require(g::transform_is_identity(g::transform_rotation(1, 0, 0, 0)), "zero axis: identity");
    const auto rx = g::transform_rotation(std::numbers::pi / 2, 1, 0, 0);
    const auto py = apply(rx, 0, 1, 0);
    require(near(py[1], 0) && near(py[2], 1), "rotation about +x maps y to z");
    // Translate(t, ...) = translation * t: the translation applies first.
    const auto t = g::transform_translate(r, 10, 0, 0);
    const auto q = apply(t, 0, 0, 0);
    require(near(q[0], 0) && near(q[1], 10), "CATransform3DTranslate prepends the translation");
    const auto s = g::transform_scale_by(g::transform_translation(5, 0, 0), 2, 3, 1);
    const auto sp = apply(s, 1, 1, 0);
    require(near(sp[0], 7) && near(sp[1], 3), "CATransform3DScale prepends the scale");
    // Concat(a, b) = a then b.
    const auto c = g::transform_concat(g::transform_scale(2, 2, 2), g::transform_translation(1, 0, 0));
    const auto cp = apply(c, 1, 0, 0);
    require(near(cp[0], 3), "CATransform3DConcat applies a then b");
    require(near(g::transform_concat(t, g::transform_invert(t)), id), "invert");
    std::array<double, 16> singular{};
    require(g::transform_invert(singular) == singular, "a singular matrix inverts to itself");
    auto perspective = id;
    perspective[11] = -1.0 / 500;
    require(!g::transform_is_affine(perspective), "perspective is not affine");
    const std::array<double, 6> affine{1, 2, 3, 4, 5, 6};
    require(g::transform_to_affine(g::transform_from_affine(affine)) == affine, "affine round trip");
    require(g::transform_is_affine(g::transform_from_affine(affine)), "an affine transform is affine");
}

void timing() {
    const std::array<float, 4> linear{0, 0, 1, 1}, ease_in{0.42f, 0, 1, 1}, ease_out{0, 0, 0.58f, 1},
        ease{0.42f, 0, 0.58f, 1}, standard{0.25f, 0.1f, 0.25f, 1};
    for (const double x : {0.0, 0.1, 0.25, 0.5, 0.9, 1.0})
        require(near(g::timing_curve_value(linear, x), x, 1e-7), "linear is the identity");
    for (const auto& curve : {ease_in, ease_out, ease, standard}) {
        require(near(g::timing_curve_value(curve, 0), 0, 1e-7) && near(g::timing_curve_value(curve, 1), 1, 1e-7), "end points");
        double previous = 0;
        for (int i = 1; i <= 100; ++i) {
            const auto value = g::timing_curve_value(curve, i / 100.0);
            require(value >= previous - 1e-9, "curves with y in [0,1] are monotonic");
            previous = value;
        }
    }
    require(near(g::timing_curve_value(ease, 0.5), 0.5, 1e-6), "easeInEaseOut is symmetric about the midpoint");
    require(g::timing_curve_value(ease_in, 0.5) < 0.5 && g::timing_curve_value(ease_out, 0.5) > 0.5, "ease in / out bend");
    // The Bezier at parameter s = 0.5 has x = 0.375*(x1 + x2) + 0.125.
    const auto x_half = 0.375 * (0.42 + 0.58) + 0.125, y_half = 0.375 * (0 + 1) + 0.125;
    require(near(g::timing_curve_value(ease, x_half), y_half, 1e-6), "the curve passes through B(0.5)");
    require(near(g::timing_curve_value(ease, -1), 0) && near(g::timing_curve_value(ease, 2), 1, 1e-7), "x is clamped");
}

void gradients() {
    const std::vector<g::GradientStop> stops{{{1, 0, 0, 1}, 0}, {{0, 0, 1, 0.5}, 1}};
    auto c = g::gradient_color_at(stops, 0.25);
    require(near(c.r, 0.75) && near(c.b, 0.25) && near(c.a, 0.875), "two-stop interpolation");
    c = g::gradient_color_at({{{0, 1, 0, 1}, 0.8}, {{1, 1, 1, 1}, 0.2}}, 0.5);   // unsorted input
    require(near(c.r, 0.5) && near(c.g, 1), "stops are sorted by location");
    c = g::gradient_color_at({{{0, 0, 0, 1}, 0.3}, {{1, 1, 1, 1}, 0.7}}, 0.1);
    require(near(c.r, 0), "before the first stop: the first color");
    c = g::gradient_color_at({{{0, 0, 0, 1}, 0.3}, {{1, 1, 1, 1}, 0.7}}, 0.9);
    require(near(c.r, 1), "after the last stop: the last color");
    const auto horizontal = g::gradient_line(10, 20, 100, 50, 0);
    require(near(horizontal.x0, 10) && near(horizontal.x1, 110) && near(horizontal.y0, 45) && near(horizontal.y1, 45),
            "angle 0 runs left to right through the middle");
    const auto vertical = g::gradient_line(0, 0, 100, 50, 90);
    require(near(vertical.x0, 50) && near(vertical.y0, 0) && near(vertical.y1, 50), "angle 90 runs bottom to top");
    const auto diagonal = g::gradient_line(0, 0, 100, 100, 45);
    // The projections of opposite corners onto the direction.
    const auto ux = std::cos(std::numbers::pi / 4), uy = std::sin(std::numbers::pi / 4);
    require(near(diagonal.x0 * ux + diagonal.y0 * uy, 0) && near(diagonal.x1 * ux + diagonal.y1 * uy, 200 * ux),
            "45 degrees spans corner to corner");
    const auto reversed = g::gradient_line(0, 0, 100, 50, 180);
    require(near(reversed.x0, 100) && near(reversed.x1, 0), "angle 180 runs right to left");
}

void affine() {
    // [t translateXBy:10 yBy:0]; [t rotateByRadians:pi/2]: rotation first.
    auto t = g::affine_translate({}, 10, 0);
    t = g::affine_rotate(t, std::numbers::pi / 2);
    auto p = g::affine_point(t, 1, 0);
    require(near(p[0], 10) && near(p[1], 1), "operations apply before the existing mapping");
    t = g::affine_scale({}, 2, 3);
    p = g::affine_point(t, 1, 1);
    require(near(p[0], 2) && near(p[1], 3), "scale");
    const auto size = g::affine_size(g::affine_translate(g::affine_scale({}, 2, 2), 100, 100), 3, 4);
    require(near(size[0], 6) && near(size[1], 8), "sizes ignore translation");
    // append: self then other.
    const auto appended = g::affine_multiply(g::affine_translate({}, 5, 0), g::affine_scale({}, 2, 2));
    p = g::affine_point(appended, 1, 0);
    require(near(p[0], 12), "multiply(a, b) applies a first");
    const auto inverse = g::affine_invert(t);
    require(inverse && near(g::affine_point(*inverse, 2, 3)[0], 1) && near(g::affine_point(*inverse, 2, 3)[1], 1), "invert");
    require(!g::affine_invert({0, 0, 0, 0, 1, 1}), "singular");
}

void hsb() {
    const auto red = g::hsb_to_rgb(0, 1, 1);
    require(near(red[0], 1) && near(red[1], 0) && near(red[2], 0), "hue 0 is red");
    const auto green = g::hsb_to_rgb(1.0 / 3, 1, 1);
    require(near(green[1], 1) && near(green[0], 0, 1e-9), "hue 1/3 is green");
    const auto wrapped = g::hsb_to_rgb(1, 1, 1);
    require(near(wrapped[0], 1) && near(wrapped[2], 0), "hue 1 is red again");
    const auto gray = g::hsb_to_rgb(0.7, 0, 0.4);
    require(near(gray[0], 0.4) && near(gray[1], 0.4) && near(gray[2], 0.4), "no saturation is gray");
    for (const auto& rgb : {std::array<double, 3>{0.2, 0.4, 0.6}, {0.9, 0.1, 0.5}, {0.3, 0.3, 0.3}, {1, 0.5, 0}}) {
        const auto h = g::rgb_to_hsb(rgb[0], rgb[1], rgb[2]);
        const auto back = g::hsb_to_rgb(h[0], h[1], h[2]);
        require(near(back[0], rgb[0]) && near(back[1], rgb[1]) && near(back[2], rgb[2]), "HSB round trip");
    }
    const auto orange = g::rgb_to_hsb(1, 0.5, 0);
    require(near(orange[0], 30.0 / 360) && near(orange[1], 1) && near(orange[2], 1), "orange's HSB");
}

void vectors() {
    require(g::vector_string({1, 2.5, -3}) == "[1 2.5 -3]", "CIVector string");
    require(g::vector_string({}) == "[]", "empty CIVector string");
    const auto parsed = g::parse_vector_string(" [0.25 1 2e2] ");
    require(parsed && parsed->size() == 3 && near((*parsed)[2], 200), "parse a CIVector string");
    const auto bare = g::parse_vector_string("1 2");
    require(bare && bare->size() == 2, "brackets are optional");
    require(!g::parse_vector_string("[1 x]") && !g::parse_vector_string("[1 2"), "malformed strings fail");
}

void quaternions() {
    const g::Quaternion identity{0, 0, 0, 1};
    const auto half = static_cast<float>(std::sin(std::numbers::pi / 4));
    const g::Quaternion quarter{0, 0, half, half};   // 90 degrees about z
    const auto mid = g::quaternion_slerp(identity, quarter, 0.5f);
    const auto eighth = static_cast<float>(std::sin(std::numbers::pi / 8));
    require(near(mid[2], eighth, 1e-6) && near(mid[3], std::cos(std::numbers::pi / 8), 1e-6), "slerp halfway is 45 degrees");
    const auto start = g::quaternion_slerp(identity, quarter, 0), end = g::quaternion_slerp(identity, quarter, 1);
    require(near(start[3], 1, 1e-6) && near(end[2], half, 1e-6), "slerp end points");
    const g::Quaternion negated{0, 0, -half, -half};   // the same rotation
    const auto shorter = g::quaternion_slerp(identity, negated, 0.5f);
    require(near(std::abs(shorter[2]), eighth, 1e-6), "slerp takes the shorter arc");
}

} // namespace

int main() {
    try {
        transforms();
        timing();
        gradients();
        affine();
        hsb();
        vectors();
        quaternions();
        std::cout << "Alchemy QLab graphics math tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
