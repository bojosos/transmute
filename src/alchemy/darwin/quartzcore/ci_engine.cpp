#include "alchemy/darwin/quartzcore/ci_engine.hpp"

#include <algorithm>
#include <cctype>

namespace alchemy::gui::completion_kit::graphics::ci {
namespace {

Rgba unpremultiply(const Rgba& c) {
    if (c[3] <= 0) return {0, 0, 0, 0};
    return {c[0] / c[3], c[1] / c[3], c[2] / c[3], c[3]};
}
Rgba premultiply(const Rgba& c) { return {c[0] * c[3], c[1] * c[3], c[2] * c[3], c[3]}; }

// Encoded straight sRGB -> working premultiplied.
Rgba to_working(Rgba straight, const RenderOptions& options) {
    if (options.linear_working_space)
        for (int i = 0; i < 3; ++i) straight[i] = srgb_to_linear(straight[i]);
    return premultiply(straight);
}

Rgba pixel_at(const Bitmap& b, std::size_t column, std::size_t row_from_bottom, const RenderOptions& options) {
    const auto row = b.height - 1 - row_from_bottom;
    const auto* p = b.rgba.data() + (row * b.width + column) * 4;
    const Rgba premultiplied{p[0] / 255.0, p[1] / 255.0, p[2] / 255.0, p[3] / 255.0};
    return to_working(unpremultiply(premultiplied), options);
}

Rgba sample_node(const Node& node, double x, double y, const RenderOptions& options, bool nearest) {
    nearest = nearest || node.nearest;
    switch (node.kind) {
    case Node::Kind::constant: {
        if (!node.extent.infinite() && !node.extent.contains(x, y)) return {0, 0, 0, 0};
        return to_working(unpremultiply(node.color), options);
    }
    case Node::Kind::bitmap: {
        const auto& b = *node.bitmap;
        if (!node.extent.contains(x, y) || !b.width || !b.height) return {0, 0, 0, 0};
        if (nearest) {
            const auto column = std::min<std::size_t>(static_cast<std::size_t>(std::floor(x)), b.width - 1);
            const auto row = std::min<std::size_t>(static_cast<std::size_t>(std::floor(y)), b.height - 1);
            return pixel_at(b, column, row, options);
        }
        // Bilinear between pixel centers; inside the extent the edge pixels
        // extend to the boundary.
        const auto u = std::clamp(x - 0.5, 0.0, static_cast<double>(b.width - 1));
        const auto v = std::clamp(y - 0.5, 0.0, static_cast<double>(b.height - 1));
        const auto c0 = static_cast<std::size_t>(std::floor(u)), r0 = static_cast<std::size_t>(std::floor(v));
        const auto c1 = std::min(c0 + 1, b.width - 1), r1 = std::min(r0 + 1, b.height - 1);
        const auto fu = u - c0, fv = v - r0;
        const auto p00 = pixel_at(b, c0, r0, options), p10 = pixel_at(b, c1, r0, options);
        const auto p01 = pixel_at(b, c0, r1, options), p11 = pixel_at(b, c1, r1, options);
        Rgba out{};
        for (int i = 0; i < 4; ++i)
            out[i] = (p00[i] * (1 - fu) + p10[i] * fu) * (1 - fv) + (p01[i] * (1 - fu) + p11[i] * fu) * fv;
        return out;
    }
    case Node::Kind::transform: {
        const auto inverse = node.transform.inverted();
        if (!inverse) return {0, 0, 0, 0};
        const auto p = inverse->apply(x, y);
        return sample_node(*node.inputs[0], p[0], p[1], options, nearest);
    }
    case Node::Kind::crop:
        if (!node.rect.contains(x, y)) return {0, 0, 0, 0};
        return sample_node(*node.inputs[0], x, y, options, nearest);
    case Node::Kind::clamp: {
        const auto& r = node.rect;
        if (r.empty()) return {0, 0, 0, 0};
        const auto cx = r.infinite() ? x : std::clamp(x, r.x + 0.5, r.x + r.width - 0.5);
        const auto cy = r.infinite() ? y : std::clamp(y, r.y + 0.5, r.y + r.height - 0.5);
        return sample_node(*node.inputs[0], cx, cy, options, nearest);
    }
    case Node::Kind::filter: {
        if (!node.extent.infinite() && !node.extent.contains(x, y)) return {0, 0, 0, 0};
        std::vector<Rgba> samples;
        samples.reserve(node.inputs.size());
        for (const auto& input : node.inputs) samples.push_back(sample_node(*input, x, y, options, nearest));
        return node.combine(samples);
    }
    }
    return {0, 0, 0, 0};
}

// ---- filter building blocks ----------------------------------------------------------------
std::shared_ptr<Node> filter_node(std::vector<Image> inputs, Rect extent, std::function<Rgba(const std::vector<Rgba>&)> combine) {
    auto node = std::make_shared<Node>();
    node->kind = Node::Kind::filter;
    node->inputs = std::move(inputs);
    node->extent = extent;
    node->combine = std::move(combine);
    return node;
}

// A per-pixel color filter on unpremultiplied values.
Image color_filter(const Image& input, std::function<Rgba(Rgba)> straight) {
    return filter_node({input}, input->extent, [straight](const std::vector<Rgba>& s) {
        auto out = straight(unpremultiply(s[0]));
        out[3] = std::clamp(out[3], 0.0, 1.0);
        return premultiply(out);
    });
}

const Image* image_input(const std::map<std::string, Value>& inputs, const std::string& key) {
    const auto found = inputs.find(key);
    if (found == inputs.end()) return nullptr;
    const auto* image = std::get_if<Image>(&found->second);
    return image && *image ? image : nullptr;
}
double number(const std::map<std::string, Value>& inputs, const std::string& key, double fallback) {
    const auto found = inputs.find(key);
    if (found == inputs.end()) return fallback;
    if (const auto* value = std::get_if<double>(&found->second)) return *value;
    return fallback;
}
std::vector<double> vector(const std::map<std::string, Value>& inputs, const std::string& key, std::vector<double> fallback) {
    const auto found = inputs.find(key);
    if (found == inputs.end()) return fallback;
    if (const auto* value = std::get_if<std::vector<double>>(&found->second)) {
        auto result = *value;
        while (result.size() < fallback.size()) result.push_back(fallback[result.size()]);
        return result;
    }
    return fallback;
}

std::string display_name(const std::string& name) {
    // "CIColorInvert" -> "Color Invert".
    std::string text;
    const auto body = name.rfind("CI", 0) == 0 ? name.substr(2) : name;
    for (std::size_t i = 0; i < body.size(); ++i) {
        const auto c = body[i];
        if (i && std::isupper(static_cast<unsigned char>(c)) && !std::isupper(static_cast<unsigned char>(body[i - 1]))) text += ' ';
        text += c;
    }
    return text;
}

InputSpec image_spec(const std::string& key = "inputImage") { return {key, InputSpec::Type::image, std::monostate{}, display_name(key.substr(5)), {}, {}}; }
InputSpec number_spec(const std::string& key, double fallback, std::optional<double> minimum = {}, std::optional<double> maximum = {}) {
    return {key, InputSpec::Type::number, fallback, display_name(key.substr(5)), minimum, maximum};
}
InputSpec vector_spec(const std::string& key, std::vector<double> fallback) {
    return {key, InputSpec::Type::vector, std::move(fallback), display_name(key.substr(5)), {}, {}};
}

using Inputs = std::map<std::string, Value>;

// Two-image Porter-Duff style compositing (inputImage over/in/... inputBackgroundImage).
FilterSpec compositing(const std::string& name, std::function<Rgba(const Rgba&, const Rgba&)> op,
                       std::function<Rect(const Rect&, const Rect&)> extent) {
    FilterSpec spec{name, display_name(name), {"CICategoryCompositeOperation", "CICategoryVideo", "CICategoryStillImage", "CICategoryBuiltIn"},
                    {image_spec(), image_spec("inputBackgroundImage")}, {}};
    spec.output = [op, extent](const Inputs& inputs) -> Image {
        const auto* source = image_input(inputs, "inputImage");
        const auto* background = image_input(inputs, "inputBackgroundImage");
        if (!source || !background) return nullptr;
        return filter_node({*source, *background}, extent((*source)->extent, (*background)->extent),
                           [op](const std::vector<Rgba>& s) { return op(s[0], s[1]); });
    };
    return spec;
}

FilterSpec color(const std::string& name, std::vector<InputSpec> extra, std::function<Rgba(Rgba, const Inputs&)> f) {
    std::vector<InputSpec> inputs{image_spec()};
    inputs.insert(inputs.end(), extra.begin(), extra.end());
    FilterSpec spec{name, display_name(name), {"CICategoryColorAdjustment", "CICategoryVideo", "CICategoryStillImage", "CICategoryBuiltIn"},
                    std::move(inputs), {}};
    spec.output = [f](const Inputs& inputs) -> Image {
        const auto* image = image_input(inputs, "inputImage");
        if (!image) return nullptr;
        return color_filter(*image, [f, inputs](Rgba c) { return f(c, inputs); });
    };
    return spec;
}

double luma(const Rgba& c) { return 0.2125 * c[0] + 0.7154 * c[1] + 0.0721 * c[2]; }

std::map<std::string, FilterSpec> make_filters() {
    std::vector<FilterSpec> list;
    list.push_back(color("CIColorInvert", {}, [](Rgba c, const Inputs&) { return Rgba{1 - c[0], 1 - c[1], 1 - c[2], c[3]}; }));
    // Saturation (mix with luma), then brightness (added), then contrast (about 0.5).
    list.push_back(color("CIColorControls",
        {number_spec("inputSaturation", 1, 0, 2), number_spec("inputBrightness", 0, -1, 1), number_spec("inputContrast", 1, 0.25, 4)},
        [](Rgba c, const Inputs& in) {
            const auto saturation = number(in, "inputSaturation", 1), brightness = number(in, "inputBrightness", 0);
            const auto contrast = number(in, "inputContrast", 1);
            const auto l = luma(c);
            for (int i = 0; i < 3; ++i) {
                auto v = l + (c[i] - l) * saturation;
                v += brightness;
                c[i] = (v - 0.5) * contrast + 0.5;
            }
            return c;
        }));
    list.push_back(color("CIColorMatrix",
        {vector_spec("inputRVector", {1, 0, 0, 0}), vector_spec("inputGVector", {0, 1, 0, 0}), vector_spec("inputBVector", {0, 0, 1, 0}),
         vector_spec("inputAVector", {0, 0, 0, 1}), vector_spec("inputBiasVector", {0, 0, 0, 0})},
        [](Rgba c, const Inputs& in) {
            const std::array<std::vector<double>, 4> rows{vector(in, "inputRVector", {1, 0, 0, 0}), vector(in, "inputGVector", {0, 1, 0, 0}),
                                                          vector(in, "inputBVector", {0, 0, 1, 0}), vector(in, "inputAVector", {0, 0, 0, 1})};
            const auto bias = vector(in, "inputBiasVector", {0, 0, 0, 0});
            Rgba out{};
            for (int r = 0; r < 4; ++r) out[r] = rows[r][0] * c[0] + rows[r][1] * c[1] + rows[r][2] * c[2] + rows[r][3] * c[3] + bias[r];
            return out;
        }));
    list.push_back(color("CIColorClamp", {vector_spec("inputMinComponents", {0, 0, 0, 0}), vector_spec("inputMaxComponents", {1, 1, 1, 1})},
        [](Rgba c, const Inputs& in) {
            const auto low = vector(in, "inputMinComponents", {0, 0, 0, 0}), high = vector(in, "inputMaxComponents", {1, 1, 1, 1});
            for (int i = 0; i < 4; ++i) c[i] = std::clamp(c[i], low[i], std::max(low[i], high[i]));
            return c;
        }));
    list.push_back(color("CIExposureAdjust", {number_spec("inputEV", 0, -10, 10)}, [](Rgba c, const Inputs& in) {
        const auto factor = std::pow(2.0, number(in, "inputEV", 0));
        for (int i = 0; i < 3; ++i) c[i] *= factor;
        return c;
    }));
    list.push_back(color("CIGammaAdjust", {number_spec("inputPower", 1, 0.25, 4)}, [](Rgba c, const Inputs& in) {
        const auto power = number(in, "inputPower", 1);
        for (int i = 0; i < 3; ++i) c[i] = c[i] < 0 ? -std::pow(-c[i], power) : std::pow(c[i], power);
        return c;
    }));
    list.push_back(color("CIColorThreshold", {number_spec("inputThreshold", 0.5, 0, 1)}, [](Rgba c, const Inputs& in) {
        const auto threshold = number(in, "inputThreshold", 0.5);
        for (int i = 0; i < 3; ++i) c[i] = c[i] > threshold ? 1 : 0;
        return c;
    }));
    list.push_back(color("CIMinimumComponent", {}, [](Rgba c, const Inputs&) {
        const auto v = std::min({c[0], c[1], c[2]});
        return Rgba{v, v, v, c[3]};
    }));
    list.push_back(color("CIMaximumComponent", {}, [](Rgba c, const Inputs&) {
        const auto v = std::max({c[0], c[1], c[2]});
        return Rgba{v, v, v, c[3]};
    }));
    list.push_back(color("CILinearToSRGBToneCurve", {}, [](Rgba c, const Inputs&) {
        for (int i = 0; i < 3; ++i) c[i] = linear_to_srgb(c[i]);
        return c;
    }));
    list.push_back(color("CISRGBToneCurveToLinear", {}, [](Rgba c, const Inputs&) {
        for (int i = 0; i < 3; ++i) c[i] = srgb_to_linear(c[i]);
        return c;
    }));

    const auto unite = [](const Rect& a, const Rect& b) { return a.unite(b); };
    const auto meet = [](const Rect& a, const Rect& b) { return a.intersect(b); };
    const auto first = [](const Rect& a, const Rect&) { return a; };
    const auto second = [](const Rect&, const Rect& b) { return b; };
    const auto each = [](const Rgba& s, const Rgba& d, auto f) { Rgba o{}; for (int i = 0; i < 4; ++i) o[i] = f(s[i], d[i], s[3], d[3]); return o; };
    list.push_back(compositing("CISourceOverCompositing", [each](const Rgba& s, const Rgba& d) {
        return each(s, d, [](double a, double b, double sa, double) { return a + b * (1 - sa); });
    }, unite));
    list.push_back(compositing("CISourceInCompositing", [each](const Rgba& s, const Rgba& d) {
        return each(s, d, [](double a, double, double, double da) { return a * da; });
    }, meet));
    list.push_back(compositing("CISourceOutCompositing", [each](const Rgba& s, const Rgba& d) {
        return each(s, d, [](double a, double, double, double da) { return a * (1 - da); });
    }, first));
    list.push_back(compositing("CISourceAtopCompositing", [each](const Rgba& s, const Rgba& d) {
        return each(s, d, [](double a, double b, double sa, double da) { return a * da + b * (1 - sa); });
    }, second));
    list.push_back(compositing("CIAdditionCompositing", [each](const Rgba& s, const Rgba& d) {
        return each(s, d, [](double a, double b, double, double) { return a + b; });
    }, unite));
    list.push_back(compositing("CIMultiplyCompositing", [each](const Rgba& s, const Rgba& d) {
        return each(s, d, [](double a, double b, double sa, double da) { return a * b + a * (1 - da) + b * (1 - sa); });
    }, unite));
    list.push_back(compositing("CIMinimumCompositing", [each](const Rgba& s, const Rgba& d) {
        return each(s, d, [](double a, double b, double, double) { return std::min(a, b); });
    }, unite));
    list.push_back(compositing("CIMaximumCompositing", [each](const Rgba& s, const Rgba& d) {
        return each(s, d, [](double a, double b, double, double) { return std::max(a, b); });
    }, unite));

    // Geometry and generators.
    FilterSpec affine{"CIAffineTransform", "Affine Transform", {"CICategoryGeometryAdjustment", "CICategoryVideo", "CICategoryStillImage", "CICategoryBuiltIn"},
                      {image_spec(), {"inputTransform", InputSpec::Type::transform, Transform{}, "Transform", {}, {}}}, {}};
    affine.output = [](const Inputs& in) -> Image {
        const auto* image = image_input(in, "inputImage");
        if (!image) return nullptr;
        const auto found = in.find("inputTransform");
        const auto* t = found == in.end() ? nullptr : std::get_if<Transform>(&found->second);
        return transformed(*image, t ? *t : Transform{});
    };
    list.push_back(affine);
    FilterSpec clamp{"CIAffineClamp", "Affine Clamp", {"CICategoryTileEffect", "CICategoryVideo", "CICategoryStillImage", "CICategoryBuiltIn"},
                     {image_spec(), {"inputTransform", InputSpec::Type::transform, Transform{}, "Transform", {}, {}}}, {}};
    clamp.output = [](const Inputs& in) -> Image {
        const auto* image = image_input(in, "inputImage");
        if (!image) return nullptr;
        const auto found = in.find("inputTransform");
        const auto* t = found == in.end() ? nullptr : std::get_if<Transform>(&found->second);
        return clamped(transformed(*image, t ? *t : Transform{}));
    };
    list.push_back(clamp);
    const auto big = 8.98846567431158e307;
    FilterSpec crop{"CICrop", "Crop", {"CICategoryGeometryAdjustment", "CICategoryVideo", "CICategoryStillImage", "CICategoryBuiltIn"},
                    {image_spec(), vector_spec("inputRectangle", {-big, -big, 2 * big, 2 * big})}, {}};
    crop.output = [big](const Inputs& in) -> Image {
        const auto* image = image_input(in, "inputImage");
        if (!image) return nullptr;
        const auto r = vector(in, "inputRectangle", {-big, -big, 2 * big, 2 * big});
        return cropped(*image, {r[0], r[1], r[2], r[3]});
    };
    list.push_back(crop);
    FilterSpec constant{"CIConstantColorGenerator", "Constant Color", {"CICategoryGenerator", "CICategoryVideo", "CICategoryStillImage", "CICategoryBuiltIn"},
                        {{"inputColor", InputSpec::Type::color, Rgba{1, 0, 0, 1}, "Color", {}, {}}}, {}};
    constant.output = [](const Inputs& in) -> Image {
        const auto found = in.find("inputColor");
        const auto* c = found == in.end() ? nullptr : std::get_if<Rgba>(&found->second);
        return constant_image(c ? *c : Rgba{1, 0, 0, 1});
    };
    list.push_back(constant);

    std::map<std::string, FilterSpec> filters;
    for (auto& spec : list) filters.emplace(spec.name, std::move(spec));
    return filters;
}

const std::map<std::string, FilterSpec>& filters() {
    static const auto all = make_filters();
    return all;
}

} // namespace

Rect Rect::intersect(const Rect& o) const {
    if (infinite()) return o;
    if (o.infinite()) return *this;
    const auto x0 = std::max(x, o.x), y0 = std::max(y, o.y);
    const auto x1 = std::min(x + width, o.x + o.width), y1 = std::min(y + height, o.y + o.height);
    if (x1 <= x0 || y1 <= y0) return {0, 0, 0, 0};
    return {x0, y0, x1 - x0, y1 - y0};
}

Rect Rect::unite(const Rect& o) const {
    if (infinite() || o.infinite()) return infinite_rect();
    if (empty()) return o;
    if (o.empty()) return *this;
    const auto x0 = std::min(x, o.x), y0 = std::min(y, o.y);
    return {x0, y0, std::max(x + width, o.x + o.width) - x0, std::max(y + height, o.y + o.height) - y0};
}

bool Rect::contains(double px, double py) const {
    if (infinite()) return true;
    return px >= x && px < x + width && py >= y && py < y + height;
}

std::optional<Transform> Transform::inverted() const {
    const auto det = a * d - b * c;
    if (det == 0 || !std::isfinite(det)) return std::nullopt;
    Transform r;
    r.a = d / det; r.b = -b / det; r.c = -c / det; r.d = a / det;
    r.tx = -(tx * r.a + ty * r.c);
    r.ty = -(tx * r.b + ty * r.d);
    return r;
}

Rect Transform::apply(const Rect& r) const {
    if (r.infinite()) return r;
    if (r.empty()) return {0, 0, 0, 0};
    double x0 = std::numeric_limits<double>::infinity(), y0 = x0, x1 = -x0, y1 = -x0;
    for (const auto& [px, py] : std::array<std::pair<double, double>, 4>{{{r.x, r.y}, {r.x + r.width, r.y}, {r.x, r.y + r.height},
                                                                          {r.x + r.width, r.y + r.height}}}) {
        const auto p = apply(px, py);
        x0 = std::min(x0, p[0]); x1 = std::max(x1, p[0]);
        y0 = std::min(y0, p[1]); y1 = std::max(y1, p[1]);
    }
    return {x0, y0, x1 - x0, y1 - y0};
}

double srgb_to_linear(double v) {
    const auto magnitude = std::abs(v);
    const auto result = magnitude <= 0.04045 ? magnitude / 12.92 : std::pow((magnitude + 0.055) / 1.055, 2.4);
    return v < 0 ? -result : result;
}

double linear_to_srgb(double v) {
    const auto magnitude = std::abs(v);
    const auto result = magnitude <= 0.0031308 ? magnitude * 12.92 : 1.055 * std::pow(magnitude, 1 / 2.4) - 0.055;
    return v < 0 ? -result : result;
}

Image bitmap_image(std::shared_ptr<const Bitmap> pixels) {
    auto node = std::make_shared<Node>();
    node->kind = Node::Kind::bitmap;
    node->extent = {0, 0, static_cast<double>(pixels->width), static_cast<double>(pixels->height)};
    node->bitmap = std::move(pixels);
    return node;
}

Image constant_image(const Rgba& straight_srgb) {
    auto node = std::make_shared<Node>();
    node->kind = Node::Kind::constant;
    node->extent = Rect::infinite_rect();
    node->color = premultiply(straight_srgb);
    return node;
}

Image empty_image() {
    auto node = std::make_shared<Node>();
    node->kind = Node::Kind::constant;
    node->extent = {0, 0, 0, 0};
    return node;
}

Image transformed(const Image& image, const Transform& t) {
    auto node = std::make_shared<Node>();
    node->kind = Node::Kind::transform;
    node->inputs = {image};
    node->transform = t;
    node->extent = t.apply(image->extent);
    return node;
}

Image cropped(const Image& image, const Rect& r) {
    auto node = std::make_shared<Node>();
    node->kind = Node::Kind::crop;
    node->inputs = {image};
    node->rect = r;
    node->extent = image->extent.intersect(r);
    return node;
}

Image clamped(const Image& image) {
    auto node = std::make_shared<Node>();
    node->kind = Node::Kind::clamp;
    node->inputs = {image};
    node->rect = image->extent;
    node->extent = Rect::infinite_rect();
    return node;
}

Image sampling_nearest(const Image& image, bool nearest) {
    auto node = std::make_shared<Node>(*image);
    node->nearest = nearest;
    return node;
}

const FilterSpec* find_filter(const std::string& name) {
    const auto found = filters().find(name);
    return found == filters().end() ? nullptr : &found->second;
}

std::vector<std::string> filter_names() {
    std::vector<std::string> names;
    for (const auto& [name, spec] : filters()) names.push_back(name);
    return names;
}

Rgba sample(const Image& image, double x, double y, const RenderOptions& options) {
    if (!image) return {0, 0, 0, 0};
    return sample_node(*image, x, y, options, false);
}

std::vector<std::uint8_t> render_rgba8(const Image& image, const Rect& area, std::size_t width, std::size_t height,
                                       const RenderOptions& options) {
    std::vector<std::uint8_t> out(width * height * 4);
    if (!image || !width || !height) return out;
    const auto sx = area.width / static_cast<double>(width), sy = area.height / static_cast<double>(height);
    const auto byte = [](double v) { return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255)); };
    for (std::size_t row = 0; row < height; ++row) {
        const auto y = area.y + area.height - (static_cast<double>(row) + 0.5) * sy;
        for (std::size_t column = 0; column < width; ++column) {
            const auto x = area.x + (static_cast<double>(column) + 0.5) * sx;
            auto c = unpremultiply(sample_node(*image, x, y, options, false));
            c[3] = std::clamp(c[3], 0.0, 1.0);
            if (options.linear_working_space)
                for (int i = 0; i < 3; ++i) c[i] = linear_to_srgb(c[i]);
            auto* p = out.data() + (row * width + column) * 4;
            for (int i = 0; i < 3; ++i) p[i] = byte(std::clamp(c[i], 0.0, 1.0) * c[3]);
            p[3] = byte(c[3]);
        }
    }
    return out;
}

} // namespace alchemy::gui::completion_kit::graphics::ci
