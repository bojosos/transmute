// Host-only tests of the Core Image evaluator (kit/completion/graphics/ci_engine.cpp):
// sRGB transfer, extents, sampling, the color, compositing and geometry
// filters, and rendering to 8-bit sRGB.
#include "alchemy/darwin/quartzcore/ci_engine.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
namespace ci = alchemy::gui::completion_kit::graphics::ci;

void require(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error(what);
}
bool near(double a, double b, double tolerance = 1e-9) { return std::abs(a - b) <= tolerance; }

// A w x h bitmap filled by f(column, row_from_top) -> straight RGBA bytes.
ci::Image bitmap(std::size_t w, std::size_t h, std::array<std::uint8_t, 4> (*f)(std::size_t, std::size_t)) {
    auto b = std::make_shared<ci::Bitmap>();
    b->width = w;
    b->height = h;
    b->rgba.resize(w * h * 4);
    for (std::size_t r = 0; r < h; ++r)
        for (std::size_t c = 0; c < w; ++c) {
            const auto p = f(c, r);
            for (int i = 0; i < 3; ++i) b->rgba[(r * w + c) * 4 + i] = static_cast<std::uint8_t>(std::lround(p[i] * p[3] / 255.0));
            b->rgba[(r * w + c) * 4 + 3] = p[3];
        }
    return ci::bitmap_image(b);
}

std::array<std::uint8_t, 4> pixel(const std::vector<std::uint8_t>& rgba, std::size_t width, std::size_t c, std::size_t r) {
    const auto* p = rgba.data() + (r * width + c) * 4;
    return {p[0], p[1], p[2], p[3]};
}

void transfer() {
    for (const double v : {0.0, 0.001, 0.04045, 0.2, 0.5, 0.9, 1.0})
        require(near(ci::linear_to_srgb(ci::srgb_to_linear(v)), v, 1e-6), "sRGB round trip");
    require(near(ci::srgb_to_linear(0.5), 0.21404114048223255, 1e-12), "sRGB 0.5 is linear 0.214");
    require(near(ci::srgb_to_linear(-0.5), -ci::srgb_to_linear(0.5)), "extended range is odd-symmetric");
}

void sampling_and_extents() {
    // 2x2: top row red, blue; bottom row green, white.
    const auto image = bitmap(2, 2, [](std::size_t c, std::size_t r) -> std::array<std::uint8_t, 4> {
        if (r == 0) return c == 0 ? std::array<std::uint8_t, 4>{255, 0, 0, 255} : std::array<std::uint8_t, 4>{0, 0, 255, 255};
        return c == 0 ? std::array<std::uint8_t, 4>{0, 255, 0, 255} : std::array<std::uint8_t, 4>{255, 255, 255, 255};
    });
    require(image->extent.x == 0 && image->extent.width == 2 && image->extent.height == 2, "bitmap extent");
    const ci::RenderOptions raw{false};
    const auto top_left = ci::sample(image, 0.5, 1.5, raw);
    require(near(top_left[0], 1) && near(top_left[1], 0), "y up: the first row is at the top");
    const auto bottom_left = ci::sample(image, 0.5, 0.5, raw);
    require(near(bottom_left[1], 1), "bottom-left pixel");
    const auto between = ci::sample(image, 1.0, 1.5, raw);
    require(near(between[0], 0.5) && near(between[2], 0.5), "bilinear between pixel centers");
    const auto nearest = ci::sample(ci::sampling_nearest(image), 0.9, 1.5, raw);
    require(near(nearest[0], 1) && near(nearest[2], 0), "nearest sampling");
    require(ci::sample(image, 3, 1, raw)[3] == 0, "clear outside the extent");
    const auto clamped = ci::clamped(image);
    require(clamped->extent.infinite() && near(ci::sample(clamped, 5, 1.5, raw)[2], 1), "clamping repeats the edge");
    const auto moved = ci::transformed(image, {1, 0, 0, 1, 10, 20});
    require(near(moved->extent.x, 10) && near(moved->extent.y, 20), "translated extent");
    require(near(ci::sample(moved, 10.5, 21.5, raw)[0], 1), "translated sample");
    const auto rotated = ci::transformed(image, {0, 1, -1, 0, 0, 0});   // 90 degrees counterclockwise
    require(near(rotated->extent.x, -2) && near(rotated->extent.width, 2), "rotated extent");
    const auto crop = ci::cropped(image, {1, 0, 5, 5});
    require(near(crop->extent.x, 1) && near(crop->extent.width, 1) && ci::sample(crop, 0.5, 0.5, raw)[3] == 0, "crop");
    require(ci::constant_image({1, 0, 0, 1})->extent.infinite(), "constant images are infinite");
    require(ci::empty_image()->extent.empty(), "empty image");
}

void filters() {
    const ci::RenderOptions raw{false}, linear{true};
    const auto gray = ci::constant_image({0.5, 0.5, 0.5, 1});
    const auto* invert = ci::find_filter("CIColorInvert");
    require(invert && invert->display == "Color Invert", "CIColorInvert and its display name");
    const auto inverted = invert->output({{"inputImage", gray}});
    require(near(ci::sample(inverted, 0, 0, raw)[0], 0.5), "invert of 0.5 without color management");
    // In the linear working space: 0.5 sRGB -> 0.214 linear -> 0.786 -> encoded ~0.9.
    const auto rendered = ci::render_rgba8(inverted, {0, 0, 1, 1}, 1, 1, linear);
    require(rendered[0] >= 229 && rendered[0] <= 231 && rendered[3] == 255, "invert in linear sRGB");
    require(!invert->output({}), "a missing input image gives no output");

    const auto* controls = ci::find_filter("CIColorControls");
    const auto red = ci::constant_image({1, 0, 0, 1});
    const auto desaturated = controls->output({{"inputImage", red}, {"inputSaturation", 0.0}, {"inputBrightness", 0.0}, {"inputContrast", 1.0}});
    const auto d = ci::sample(desaturated, 0, 0, raw);
    require(near(d[0], 0.2125) && near(d[1], 0.2125) && near(d[2], 0.2125), "saturation 0 is luma (Rec. 709 weights)");
    const auto brighter = controls->output({{"inputImage", gray}, {"inputSaturation", 1.0}, {"inputBrightness", 0.25}, {"inputContrast", 2.0}});
    require(near(ci::sample(brighter, 0, 0, raw)[0], 1.0), "brightness then contrast about 0.5");

    const auto* matrix = ci::find_filter("CIColorMatrix");
    const auto swapped = matrix->output({{"inputImage", red}, {"inputRVector", std::vector<double>{0, 0, 1, 0}},
                                          {"inputBVector", std::vector<double>{1, 0, 0, 0}}, {"inputBiasVector", std::vector<double>{0, 0.25, 0, 0}}});
    const auto m = ci::sample(swapped, 0, 0, raw);
    require(near(m[0], 0) && near(m[1], 0.25) && near(m[2], 1) && near(m[3], 1), "color matrix rows and bias");

    const auto half_red = ci::constant_image({1, 0, 0, 0.5});
    const auto blue = ci::constant_image({0, 0, 1, 1});
    const auto* over = ci::find_filter("CISourceOverCompositing");
    const auto o = ci::sample(over->output({{"inputImage", half_red}, {"inputBackgroundImage", blue}}), 0, 0, raw);
    require(near(o[0], 0.5) && near(o[2], 0.5) && near(o[3], 1), "source over");
    const auto* multiply = ci::find_filter("CIMultiplyCompositing");
    const auto mu = ci::sample(multiply->output({{"inputImage", red}, {"inputBackgroundImage", gray}}), 0, 0, raw);
    require(near(mu[0], 0.5) && near(mu[1], 0), "multiply");
    const auto* exposure = ci::find_filter("CIExposureAdjust");
    require(near(ci::sample(exposure->output({{"inputImage", gray}, {"inputEV", 1.0}}), 0, 0, raw)[0], 1.0), "exposure +1 EV doubles");
    const auto* threshold = ci::find_filter("CIColorThreshold");
    const auto t = ci::sample(threshold->output({{"inputImage", ci::constant_image({0.6, 0.4, 0.5, 1})}, {"inputThreshold", 0.5}}), 0, 0, raw);
    require(t[0] == 1 && t[1] == 0 && t[2] == 0, "threshold");

    const auto* crop = ci::find_filter("CICrop");
    const auto cropped = crop->output({{"inputImage", blue}, {"inputRectangle", std::vector<double>{1, 2, 3, 4}}});
    require(near(cropped->extent.x, 1) && near(cropped->extent.height, 4), "CICrop extent");
    const auto* affine = ci::find_filter("CIAffineTransform");
    const auto scaled = affine->output({{"inputImage", cropped}, {"inputTransform", ci::Transform{2, 0, 0, 2, 0, 0}}});
    require(near(scaled->extent.x, 2) && near(scaled->extent.width, 6), "CIAffineTransform extent");
    const auto* constant = ci::find_filter("CIConstantColorGenerator");
    require(constant->output({})->extent.infinite(), "constant color generator");
    require(ci::find_filter("CIGaussianBlur") == nullptr, "unimplemented filters are not listed");
}

void rendering() {
    const auto image = bitmap(4, 2, [](std::size_t c, std::size_t) -> std::array<std::uint8_t, 4> {
        return {static_cast<std::uint8_t>(c * 80), 0, 0, 255};
    });
    const auto out = ci::render_rgba8(image, {0, 0, 4, 2}, 4, 2, {true});
    for (std::size_t c = 0; c < 4; ++c) require(pixel(out, 4, c, 0)[0] == c * 80, "sources round-trip through the linear working space");
    const auto half = ci::render_rgba8(ci::constant_image({1, 1, 1, 0.5}), {0, 0, 1, 1}, 1, 1, {true});
    require(half[3] == 128 && half[0] >= 127 && half[0] <= 128, "premultiplied output: " + std::to_string(half[0]) + "," + std::to_string(half[3]));
    // Downsampled render: one pixel covering 4 units samples the center.
    const auto small = ci::render_rgba8(image, {0, 0, 4, 2}, 2, 1, {false});
    require(small.size() == 8, "render size");
}

} // namespace

int main() {
    try {
        transfer();
        sampling_and_extents();
        filters();
        rendering();
        std::cout << "Alchemy QLab Core Image evaluator tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
