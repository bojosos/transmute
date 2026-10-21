#pragma once

// Host-only value math of the QLab graphics group (G6), shared by the
// providers in this directory and tested by tests/graphics_math_tests.cpp:
//   - CATransform3D (Core Animation's 4x4 row-vector matrix),
//   - CAMediaTimingFunction curves (cubic Bezier from (0,0) to (1,1)),
//   - NSGradient color interpolation and its drawing geometry,
//   - NSAffineTransform (AppKit's 2D affine matrix),
//   - HSB <-> RGB (NSColor's hue/saturation/brightness),
//   - CIVector / CIColor string representations,
//   - GLKQuaternion slerp.
// Behaviour follows Apple's public documentation; no guest access here.

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy::gui::completion_kit::graphics {

// ---- CATransform3D --------------------------------------------------------------
// Sixteen doubles m11 m12 m13 m14 m21 ... m44, row-major, applied to row
// vectors (p' = p * M), as Core Animation lays the struct out.
using Transform3D = std::array<double, 16>;

[[nodiscard]] Transform3D transform_identity();
[[nodiscard]] Transform3D transform_translation(double tx, double ty, double tz);
[[nodiscard]] Transform3D transform_scale(double sx, double sy, double sz);
// Rotation by `angle` radians about the vector (x, y, z); the identity when
// the vector has zero length (Core Animation's behaviour).
[[nodiscard]] Transform3D transform_rotation(double angle, double x, double y, double z);
// CATransform3DConcat(a, b): a then b (a * b).
[[nodiscard]] Transform3D transform_concat(const Transform3D& a, const Transform3D& b);
// CATransform3DTranslate(t, ...) = translation then t, i.e. concat(translation, t).
[[nodiscard]] Transform3D transform_translate(const Transform3D& t, double tx, double ty, double tz);
[[nodiscard]] Transform3D transform_scale_by(const Transform3D& t, double sx, double sy, double sz);
[[nodiscard]] Transform3D transform_rotate(const Transform3D& t, double angle, double x, double y, double z);
// CATransform3DInvert: the inverse, or `t` unchanged when it is singular.
[[nodiscard]] Transform3D transform_invert(const Transform3D& t);
[[nodiscard]] bool transform_is_identity(const Transform3D& t);
[[nodiscard]] bool transform_is_affine(const Transform3D& t);
// CGAffineTransform {a, b, c, d, tx, ty} <-> CATransform3D.
[[nodiscard]] Transform3D transform_from_affine(const std::array<double, 6>& affine);
[[nodiscard]] std::array<double, 6> transform_to_affine(const Transform3D& t);

// ---- CAMediaTimingFunction -----------------------------------------------------------
// The y value of the cubic Bezier through (0,0), (x1,y1), (x2,y2), (1,1) at
// the point whose x is `x` (clamped to [0, 1]); x1 and x2 are clamped to
// [0, 1] as Core Animation does, so x is monotonic in the curve parameter.
[[nodiscard]] double timing_curve_value(const std::array<float, 4>& points, double x);

// ---- NSGradient -------------------------------------------------------------------------
struct Rgba { double r{}, g{}, b{}, a{1}; };
struct GradientStop { Rgba color; double location{}; };
// The color at `location` in [0, 1]: stops sorted by location, linear
// interpolation of the straight (non-premultiplied) components between the
// two neighbouring stops, and the first / last color outside them.
[[nodiscard]] Rgba gradient_color_at(std::vector<GradientStop> stops, double location);
// Start and end points of -[NSGradient drawInRect:angle:]: the gradient
// runs through the rectangle's center along `degrees` (counterclockwise from
// the +x axis) and spans the rectangle's projection onto that direction, so
// the first and last colors land exactly on opposite corners (or edges).
struct GradientLine { double x0{}, y0{}, x1{}, y1{}; };
[[nodiscard]] GradientLine gradient_line(double x, double y, double width, double height, double degrees);

// ---- NSAffineTransform ---------------------------------------------------------------------
// NSAffineTransformStruct {m11, m12, m21, m22, tX, tY}: x' = m11 x + m21 y + tX,
// y' = m12 x + m22 y + tY.
struct Affine { double m11{1}, m12{0}, m21{0}, m22{1}, tx{0}, ty{0}; };
// a then b (a's mapping applied first): the matrix product a * b.
[[nodiscard]] Affine affine_multiply(const Affine& a, const Affine& b);
// -translateXBy:yBy:, -rotateByRadians:, -scaleXBy:yBy: prepend the
// operation (it applies before the receiver's existing mapping).
[[nodiscard]] Affine affine_translate(const Affine& t, double x, double y);
[[nodiscard]] Affine affine_rotate(const Affine& t, double radians);
[[nodiscard]] Affine affine_scale(const Affine& t, double sx, double sy);
[[nodiscard]] std::optional<Affine> affine_invert(const Affine& t);
[[nodiscard]] std::array<double, 2> affine_point(const Affine& t, double x, double y);
[[nodiscard]] std::array<double, 2> affine_size(const Affine& t, double width, double height);

// ---- HSB ---------------------------------------------------------------------------------------
// Hue, saturation and brightness in [0, 1] (hue 1 is red again).
[[nodiscard]] std::array<double, 3> hsb_to_rgb(double hue, double saturation, double brightness);
[[nodiscard]] std::array<double, 3> rgb_to_hsb(double red, double green, double blue);

// ---- CIVector / CIColor strings -----------------------------------------------------------------
// "[1 2 3]" (CIVector -stringRepresentation): the numbers in brackets
// separated by spaces; "%g" formatting like Core Image. Parsing accepts the
// numbers with or without the brackets and fails (nullopt) on anything else.
[[nodiscard]] std::string vector_string(const std::vector<double>& values);
[[nodiscard]] std::optional<std::vector<double>> parse_vector_string(std::string_view text);

// ---- GLKQuaternion ---------------------------------------------------------------------------------
using Quaternion = std::array<float, 4>;   // x, y, z, w
// GLKQuaternionSlerp: spherical interpolation along the shorter arc,
// linear interpolation when the quaternions are nearly parallel.
[[nodiscard]] Quaternion quaternion_slerp(const Quaternion& from, const Quaternion& to, float t);

} // namespace alchemy::gui::completion_kit::graphics
