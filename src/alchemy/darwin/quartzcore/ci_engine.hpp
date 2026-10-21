#pragma once

// Core Image's evaluation model on the host (QLab graphics group G6): an
// image is an immutable recipe (a node graph) that is only evaluated when a
// context renders a region of it. Coordinates are Core Image's: y up, one
// unit per pixel, the origin at the bottom-left of a bitmap source.
//
// Colors: sources keep their sRGB-encoded, premultiplied pixels; a render
// evaluates in the context's working space -- linear sRGB (Core Image's
// default) or, with no working color space, the encoded values themselves --
// and encodes the result as premultiplied sRGB. Color filters work on
// unpremultiplied values and premultiply their result, as Core Image's
// kernels do.
//
// Sampling is linear (bilinear between pixel centers) unless the image asks
// for nearest sampling; outside a finite extent an image is clear.
//
// The built-in filters implemented here are listed by filter_names(); a
// filter Core Image has but that is not implemented is not listed (the
// Objective-C layer answers nil for it, as for an unknown name).

#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace alchemy::gui::completion_kit::graphics::ci {

using Rgba = std::array<double, 4>;   // premultiplied unless stated otherwise

struct Rect {
    double x{}, y{}, width{}, height{};
    [[nodiscard]] bool infinite() const { return !std::isfinite(width) || !std::isfinite(height); }
    [[nodiscard]] bool empty() const { return !(width > 0) || !(height > 0); }
    [[nodiscard]] static Rect infinite_rect() {
        const auto big = std::numeric_limits<double>::max() / 4;
        return {-big / 2, -big / 2, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};
    }
    [[nodiscard]] Rect intersect(const Rect& other) const;
    [[nodiscard]] Rect unite(const Rect& other) const;
    [[nodiscard]] bool contains(double px, double py) const;
};

// CGAffineTransform layout: x' = a x + c y + tx, y' = b x + d y + ty.
struct Transform {
    double a{1}, b{0}, c{0}, d{1}, tx{0}, ty{0};
    [[nodiscard]] std::array<double, 2> apply(double x, double y) const { return {a * x + c * y + tx, b * x + d * y + ty}; }
    [[nodiscard]] std::optional<Transform> inverted() const;
    [[nodiscard]] Rect apply(const Rect& r) const;   // bounding box
};

struct Bitmap {
    std::size_t width{}, height{};
    std::vector<std::uint8_t> rgba;   // premultiplied sRGB, rows top to bottom
};

struct Node;
using Image = std::shared_ptr<const Node>;

struct Node {
    enum class Kind { bitmap, constant, transform, crop, clamp, filter } kind{Kind::constant};
    Rect extent;
    std::shared_ptr<const Bitmap> bitmap;            // bitmap
    Rgba color{};                                     // constant (premultiplied, encoded sRGB)
    Transform transform;                              // transform
    Rect rect;                                        // crop / clamp: the clamped extent
    bool nearest{};                                   // sampling of this subtree's sources
    std::vector<Image> inputs;                        // transform / crop / clamp: one; filter: its images
    // filter: evaluates one point from the inputs' samples (working space,
    // premultiplied in and out).
    std::function<Rgba(const std::vector<Rgba>&)> combine;
};

// Image constructors.
[[nodiscard]] Image bitmap_image(std::shared_ptr<const Bitmap> pixels);
[[nodiscard]] Image constant_image(const Rgba& straight_srgb);   // infinite extent
[[nodiscard]] Image empty_image();                                // clear, empty extent
[[nodiscard]] Image transformed(const Image& image, const Transform& t);
[[nodiscard]] Image cropped(const Image& image, const Rect& r);
[[nodiscard]] Image clamped(const Image& image);                  // edge pixels repeat to infinity
[[nodiscard]] Image sampling_nearest(const Image& image, bool nearest = true);

// ---- filters --------------------------------------------------------------------------
// A filter input value: an image, a number, a vector (CIVector), a color
// (CIColor: straight sRGB) or a transform.
using Value = std::variant<std::monostate, Image, double, std::vector<double>, Rgba, Transform>;

struct InputSpec {
    std::string key;            // "inputImage", "inputRadius", ...
    enum class Type { image, number, vector, color, transform } type{Type::number};
    Value fallback;             // the default (monostate: none, as for images)
    std::string display;        // kCIAttributeDisplayName
    std::optional<double> minimum, maximum;
};
struct FilterSpec {
    std::string name, display;
    std::vector<std::string> categories;
    std::vector<InputSpec> inputs;
    // The output for the inputs (every declared input present or defaulted),
    // nullptr when a required image input is missing.
    std::function<Image(const std::map<std::string, Value>&)> output;
};
[[nodiscard]] const FilterSpec* find_filter(const std::string& name);
[[nodiscard]] std::vector<std::string> filter_names();

// ---- rendering --------------------------------------------------------------------------
struct RenderOptions {
    bool linear_working_space{true};   // false: no color management
};
// The image's value at a point (working space, premultiplied).
[[nodiscard]] Rgba sample(const Image& image, double x, double y, const RenderOptions& options);
// Renders `area` at one pixel per unit into premultiplied 8-bit sRGB RGBA,
// rows top to bottom (row 0 is the top of the area).
[[nodiscard]] std::vector<std::uint8_t> render_rgba8(const Image& image, const Rect& area, std::size_t width, std::size_t height,
                                                     const RenderOptions& options);

// sRGB transfer functions (exposed for tests).
[[nodiscard]] double srgb_to_linear(double value);
[[nodiscard]] double linear_to_srgb(double value);

} // namespace alchemy::gui::completion_kit::graphics::ci
