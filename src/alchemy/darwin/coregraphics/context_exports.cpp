#include "alchemy/darwin/coretext/ct_internal.hpp"
#include "alchemy/darwin/coregraphics/context.hpp"
#include "alchemy/darwin/coregraphics/quartz2d.hpp"

#include <algorithm>

// CGContext, CGBitmapContext and CGLayer functions.
namespace alchemy::gui::quartz2d {
namespace {

struct LayerInfo final : ObjectPayload {
    Address context{};                    // +1 bitmap context
    Size size{};
};

// CGPathDrawingMode
constexpr std::int32_t path_fill = 0, path_eo_fill = 1, path_stroke = 2, path_fill_stroke = 3, path_eo_fill_stroke = 4;

PixelLayout bitmap_layout(Quartz& q, std::uint64_t width, std::uint64_t height, std::uint64_t bpc, std::uint64_t bytes_per_row,
                          Address space, std::uint32_t info, bool allocate) {
    PixelLayout layout;
    layout.width = static_cast<std::uint32_t>(std::min<std::uint64_t>(width, UINT32_MAX));
    layout.height = static_cast<std::uint32_t>(std::min<std::uint64_t>(height, UINT32_MAX));
    layout.bits_per_component = static_cast<std::uint32_t>(bpc);
    layout.bitmap_info = info;
    if (!space) {
        layout.model = ColorModel::unknown;
        layout.color_components = 0;
        layout.bits_per_pixel = layout.bits_per_component;
    } else {
        const auto space_info = q.space_info(space);
        layout.model = space_info->model;
        layout.color_components = space_info->components();
        const bool alpha_slot = layout.has_alpha() || layout.alpha() == alpha_info::none_skip_first || layout.alpha() == alpha_info::none_skip_last;
        layout.bits_per_pixel = layout.bits_per_component * (layout.color_components + (alpha_slot ? 1 : 0));
    }
    const auto minimum = (static_cast<std::uint64_t>(layout.width) * layout.bits_per_pixel + 7) / 8;
    layout.bytes_per_row = bytes_per_row ? bytes_per_row : allocate ? (minimum + 15) / 16 * 16 : minimum;
    return layout;
}

} // namespace

std::uint64_t create_bitmap_context(std::uint32_t width, std::uint32_t height, double scale) {
    const ProviderScope scope;
    auto* q = provider();
    if (!q) fail("Quartz provider is not installed");
    PixelLayout layout;
    layout.width = width;
    layout.height = height;
    layout.bytes_per_row = static_cast<std::uint64_t>(width) * 4;
    layout.bitmap_info = alpha_info::premultiplied_first | bitmap_info::order_32_little;
    const auto context = create_context(*q, layout, 0, q->srgb());
    if (!context) fail("creating a " + std::to_string(width) + "x" + std::to_string(height) + " bitmap context");
    const auto state = context_state(*q, context);
    state->base_ctm = Affine::scaling(scale, scale);
    state->g.ctm = state->base_ctm;
    return context;
}

std::optional<Image> context_image(std::uint64_t context) {
    const ProviderScope scope;
    auto* q = provider();
    if (!q) return std::nullopt;
    const auto state = q->find<ContextState>(context);
    if (!state) return std::nullopt;
    std::lock_guard lock(state->mutex);
    return snapshot(*q, *state);
}

void Quartz::install_contexts() {
    const auto cg = core_graphics_library;
    context_class = declare_class("_AlchemyCGContext");
    layer_class = declare_class("_AlchemyCGLayer");
    // Every context function locks the context (recursively: pattern and
    // layer callbacks may draw into other contexts).
    const auto with = [this](Address object, const std::function<void(ContextState&)>& body) {
        if (!object) return;   // CoreGraphics ignores (and logs) a NULL context
        const auto state = context_state(*this, object);
        std::lock_guard lock(state->mutex);
        body(*state);
    };
    const auto context_function = [this, with](std::string name, std::function<void(CpuState&, GuestMemory&, ContextState&)> body) {
        function(core_graphics_library, std::move(name), [with, body = std::move(body)](CpuState& s, GuestMemory& m) {
            with(s.x[0], [&](ContextState& c) { body(s, m, c); });
        });
    };

    // ---- bitmap contexts ----
    function(cg, "_CGBitmapContextCreate", [this](CpuState& s, GuestMemory&) {
        // (data, width, height, bitsPerComponent, bytesPerRow, space, bitmapInfo)
        const auto layout = bitmap_layout(*this, s.x[1], s.x[2], s.x[3], s.x[4], s.x[5], static_cast<std::uint32_t>(s.x[6]), s.x[0] == 0);
        s.x[0] = create_context(*this, layout, s.x[0], s.x[5]);
    });
    function(cg, "_CGBitmapContextCreateWithData", [this](CpuState& s, GuestMemory& m) {
        // (data, width, height, bitsPerComponent, bytesPerRow, space, bitmapInfo, releaseCallback | releaseInfo)
        StackReader stack{s, m};
        const auto release_info = stack.next<std::uint64_t>();
        const auto layout = bitmap_layout(*this, s.x[1], s.x[2], s.x[3], s.x[4], s.x[5], static_cast<std::uint32_t>(s.x[6]), s.x[0] == 0);
        s.x[0] = create_context(*this, layout, s.x[0], s.x[5], s.x[0] ? s.x[7] : 0, release_info);
    });
    const auto bitmap = [this](Address object) { return object ? find<ContextState>(object) : nullptr; };
    function(cg, "_CGBitmapContextGetData", [bitmap](CpuState& s, GuestMemory&) { const auto c = bitmap(s.x[0]); s.x[0] = c ? c->data : 0; });
    function(cg, "_CGBitmapContextGetWidth", [bitmap](CpuState& s, GuestMemory&) { const auto c = bitmap(s.x[0]); s.x[0] = c ? c->layout.width : 0; });
    function(cg, "_CGBitmapContextGetHeight", [bitmap](CpuState& s, GuestMemory&) { const auto c = bitmap(s.x[0]); s.x[0] = c ? c->layout.height : 0; });
    function(cg, "_CGBitmapContextGetBitsPerComponent", [bitmap](CpuState& s, GuestMemory&) { const auto c = bitmap(s.x[0]); s.x[0] = c ? c->layout.bits_per_component : 0; });
    function(cg, "_CGBitmapContextGetBitsPerPixel", [bitmap](CpuState& s, GuestMemory&) { const auto c = bitmap(s.x[0]); s.x[0] = c ? c->layout.bits_per_pixel : 0; });
    function(cg, "_CGBitmapContextGetBytesPerRow", [bitmap](CpuState& s, GuestMemory&) { const auto c = bitmap(s.x[0]); s.x[0] = c ? c->layout.bytes_per_row : 0; });
    function(cg, "_CGBitmapContextGetColorSpace", [bitmap](CpuState& s, GuestMemory&) { const auto c = bitmap(s.x[0]); s.x[0] = c ? c->space.get() : 0; });
    function(cg, "_CGBitmapContextGetAlphaInfo", [bitmap](CpuState& s, GuestMemory&) { const auto c = bitmap(s.x[0]); s.x[0] = c ? c->layout.alpha() : 0; });
    function(cg, "_CGBitmapContextGetBitmapInfo", [bitmap](CpuState& s, GuestMemory&) { const auto c = bitmap(s.x[0]); s.x[0] = c ? c->layout.bitmap_info : 0; });
    context_function("_CGBitmapContextCreateImage", [this](CpuState& s, GuestMemory& m, ContextState& c) {
        auto bytes = std::make_shared<std::vector<std::uint8_t>>(static_cast<std::size_t>(c.layout.byte_size()));
        // Drawing inside an open transparency layer reaches the bitmap at its end (as on macOS).
        m.read_bytes(c.data, *bytes);
        auto layout = c.layout;
        layout.straight_alpha = false;
        s.x[0] = make_image(layout, c.space.get(), bytes);
    });

    // ---- lifetime ----
    function(cg, "_CGContextRetain", [this](CpuState& s, GuestMemory&) { s.x[0] = retain(s.x[0]); });
    function(cg, "_CGContextRelease", [this](CpuState& s, GuestMemory&) { release(s.x[0]); });
    function(cg, "_CGContextGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = 0x100; });
    context_function("_CGContextFlush", [this](CpuState&, GuestMemory&, ContextState& c) { synchronize(*this, c); });
    context_function("_CGContextSynchronize", [this](CpuState&, GuestMemory&, ContextState& c) { synchronize(*this, c); });
    // Page boundaries apply to PDF contexts; a bitmap context has one page.
    context_function("_CGContextBeginPage", [](CpuState&, GuestMemory&, ContextState&) {});
    context_function("_CGContextEndPage", [](CpuState&, GuestMemory&, ContextState&) {});

    // ---- graphics state ----
    context_function("_CGContextSaveGState", [](CpuState&, GuestMemory&, ContextState& c) { save_state(c); });
    context_function("_CGContextRestoreGState", [this](CpuState&, GuestMemory&, ContextState& c) { restore_state(*this, c); });
    context_function("_CGContextScaleCTM", [](CpuState& s, GuestMemory&, ContextState& c) { c.g.ctm = Affine::scaling(d(s, 0), d(s, 1)).then(c.g.ctm); });
    context_function("_CGContextTranslateCTM", [](CpuState& s, GuestMemory&, ContextState& c) { c.g.ctm = Affine::translation(d(s, 0), d(s, 1)).then(c.g.ctm); });
    context_function("_CGContextRotateCTM", [](CpuState& s, GuestMemory&, ContextState& c) { c.g.ctm = Affine::rotation(d(s, 0)).then(c.g.ctm); });
    context_function("_CGContextConcatCTM", [this](CpuState& s, GuestMemory&, ContextState& c) { c.g.ctm = read_affine(s.x[1]).then(c.g.ctm); });
    function(cg, "_CGContextGetCTM", [this, with](CpuState& s, GuestMemory&) {
        Affine value{};
        with(s.x[0], [&](ContextState& c) { value = c.g.ctm; });
        write_affine(s.x[8], value);
    });
    function(cg, "_CGContextGetUserSpaceToDeviceSpaceTransform", [this, with](CpuState& s, GuestMemory&) {
        Affine value{};
        with(s.x[0], [&](ContextState& c) { value = c.g.ctm; });
        write_affine(s.x[8], value);
    });
    const auto convert = [](ContextState& c, bool to_device) {
        return to_device ? c.g.ctm : c.g.ctm.inverse().value_or(Affine{});
    };
    context_function("_CGContextConvertPointToDeviceSpace", [convert](CpuState& s, GuestMemory&, ContextState& c) { return_point(s, convert(c, true).apply(point_arg(s))); });
    context_function("_CGContextConvertPointToUserSpace", [convert](CpuState& s, GuestMemory&, ContextState& c) { return_point(s, convert(c, false).apply(point_arg(s))); });
    context_function("_CGContextConvertSizeToDeviceSpace", [convert](CpuState& s, GuestMemory&, ContextState& c) {
        const auto v = convert(c, true).apply_vector(point_arg(s));
        return_size(s, {v.x, v.y});
    });
    context_function("_CGContextConvertSizeToUserSpace", [convert](CpuState& s, GuestMemory&, ContextState& c) {
        const auto v = convert(c, false).apply_vector(point_arg(s));
        return_size(s, {v.x, v.y});
    });
    context_function("_CGContextConvertRectToDeviceSpace", [convert](CpuState& s, GuestMemory&, ContextState& c) { return_rect(s, transform_rect(convert(c, true), rect_arg(s))); });
    context_function("_CGContextConvertRectToUserSpace", [convert](CpuState& s, GuestMemory&, ContextState& c) { return_rect(s, transform_rect(convert(c, false), rect_arg(s))); });

    context_function("_CGContextSetLineWidth", [](CpuState& s, GuestMemory&, ContextState& c) { c.g.line_width = std::max(0.0, d(s, 0)); });
    context_function("_CGContextSetLineCap", [](CpuState& s, GuestMemory&, ContextState& c) { c.g.cap = static_cast<std::uint32_t>(s.x[1]) & 3; });
    context_function("_CGContextSetLineJoin", [](CpuState& s, GuestMemory&, ContextState& c) { c.g.join = static_cast<std::uint32_t>(s.x[1]) & 3; });
    context_function("_CGContextSetMiterLimit", [](CpuState& s, GuestMemory&, ContextState& c) { c.g.miter_limit = d(s, 0); });
    context_function("_CGContextSetFlatness", [](CpuState& s, GuestMemory&, ContextState& c) { c.g.flatness = d(s, 0); });
    context_function("_CGContextSetLineDash", [this](CpuState& s, GuestMemory&, ContextState& c) {
        // (c, phase d0, lengths, count)
        c.g.dashes.clear();
        c.g.dash_phase = d(s, 0);
        if (!s.x[1]) return;
        for (Address index = 0; index < s.x[2] && index < 1024; ++index) c.g.dashes.push_back(read_double(s.x[1] + index * 8));
    });
    context_function("_CGContextSetAlpha", [](CpuState& s, GuestMemory&, ContextState& c) { c.g.alpha = std::clamp(d(s, 0), 0.0, 1.0); });
    context_function("_CGContextSetBlendMode", [](CpuState& s, GuestMemory&, ContextState& c) {
        const auto mode = static_cast<std::int32_t>(s.x[1]);
        if (mode < 0 || mode > blend::plus_lighter) unsupported("CGBlendMode " + std::to_string(mode));
        c.g.blend_mode = mode;
    });
    context_function("_CGContextSetShouldAntialias", [](CpuState& s, GuestMemory&, ContextState& c) { c.g.antialias = bool_arg(s, 1); });
    context_function("_CGContextSetAllowsAntialiasing", [](CpuState& s, GuestMemory&, ContextState& c) { c.g.allows_antialias = bool_arg(s, 1); });
    context_function("_CGContextSetShouldSmoothFonts", [](CpuState& s, GuestMemory&, ContextState& c) { c.g.smooth_fonts = bool_arg(s, 1); });
    context_function("_CGContextSetAllowsFontSmoothing", [](CpuState& s, GuestMemory&, ContextState& c) { c.g.allows_smoothing = bool_arg(s, 1); });
    // Glyph positions are always subpixel (DirectWrite natural mode); the
    // positioning / quantization hints change nothing visible here.
    for (const auto* name : {"_CGContextSetShouldSubpixelPositionFonts", "_CGContextSetAllowsFontSubpixelPositioning",
             "_CGContextSetShouldSubpixelQuantizeFonts", "_CGContextSetAllowsFontSubpixelQuantization"})
        context_function(name, [](CpuState&, GuestMemory&, ContextState&) {});
    context_function("_CGContextSetInterpolationQuality", [](CpuState& s, GuestMemory&, ContextState& c) { c.g.interpolation = static_cast<std::int32_t>(s.x[1]); });
    function(cg, "_CGContextGetInterpolationQuality", [with](CpuState& s, GuestMemory&) {
        std::int32_t value{};
        with(s.x[0], [&](ContextState& c) { value = c.g.interpolation; });
        s.x[0] = static_cast<std::uint32_t>(value);
    });
    context_function("_CGContextSetRenderingIntent", [](CpuState& s, GuestMemory&, ContextState& c) { c.g.intent = static_cast<std::int32_t>(s.x[1]); });
    context_function("_CGContextSetShadow", [](CpuState& s, GuestMemory&, ContextState& c) {
        c.g.shadow = {true, {d(s, 0), d(s, 1)}, std::max(0.0, d(s, 2)), {0, 0, 0, 1.0 / 3.0}};
    });
    context_function("_CGContextSetShadowWithColor", [this](CpuState& s, GuestMemory&, ContextState& c) {
        if (!s.x[1]) { c.g.shadow.enabled = false; return; }
        c.g.shadow = {true, {d(s, 0), d(s, 1)}, std::max(0.0, d(s, 2)), color_rgba(s.x[1])};
    });

    // ---- colors ----
    const auto set_color = [this](Paint& paint, Address color) {
        if (!color) return;
        const auto info = require<ColorInfo>(color, "CGColor");
        paint.space = ObjectRef::retain(info->space);
        if (info->pattern) {
            paint.pattern = ObjectRef::retain(info->pattern);
            paint.tint = info->components;
            const auto space = info->space ? space_info(info->space) : nullptr;
            const auto count = paint.tint.empty() ? 0 : paint.tint.size() - 1;
            paint.color = space && space->base ? to_rgba(space->base, std::span(paint.tint.data(), count), info->alpha())
                                               : Rgba{0, 0, 0, info->alpha()};
        } else {
            paint.pattern.reset();
            paint.color = color_rgba(color);
        }
    };
    const auto set_space = [this](Paint& paint, Address space) {
        if (!space) return;
        const auto info = space_info(space);
        paint.space = ObjectRef::retain(space);
        paint.pattern.reset();
        std::vector<double> zero(info->components(), 0.0);
        if (info->model == ColorModel::cmyk) zero[3] = 1;   // black
        paint.color = info->model == ColorModel::pattern ? Rgba{0, 0, 0, 1} : to_rgba(space, zero, 1);
    };
    const auto set_components = [this](Paint& paint, Address components) {
        if (!components) return;
        const auto space = paint.space.get() ? paint.space.get() : device_rgb();
        const auto info = space_info(space);
        if (info->model == ColorModel::pattern) fail("CGContextSetFillColor with a pattern color space (use CGContextSetFillPattern)");
        const auto count = info->components();
        std::vector<double> values(count);
        for (unsigned index = 0; index < count; ++index) values[index] = read_double(components + index * 8);
        paint.pattern.reset();
        paint.color = to_rgba(space, values, read_double(components + count * 8));
    };
    const auto set_pattern = [this](Paint& paint, Address pattern, Address components) {
        if (!pattern) return;
        const auto info = require<PatternInfo>(pattern, "CGPattern");
        paint.pattern = ObjectRef::retain(pattern);
        const auto space = paint.space.get() ? space_info(paint.space.get()) : nullptr;
        if (!info->colored && space && space->base && components) {
            const auto count = space_info(space->base)->components();
            std::vector<double> values(count);
            for (unsigned index = 0; index < count; ++index) values[index] = read_double(components + index * 8);
            paint.tint = values;
            paint.tint.push_back(read_double(components + count * 8));
            paint.color = to_rgba(space->base, values, paint.tint.back());
        } else {
            paint.color = {0, 0, 0, components ? read_double(components) : 1.0};
        }
    };
    context_function("_CGContextSetFillColorWithColor", [set_color](CpuState& s, GuestMemory&, ContextState& c) { set_color(c.g.fill, s.x[1]); });
    context_function("_CGContextSetStrokeColorWithColor", [set_color](CpuState& s, GuestMemory&, ContextState& c) { set_color(c.g.stroke, s.x[1]); });
    context_function("_CGContextSetFillColorSpace", [set_space](CpuState& s, GuestMemory&, ContextState& c) { set_space(c.g.fill, s.x[1]); });
    context_function("_CGContextSetStrokeColorSpace", [set_space](CpuState& s, GuestMemory&, ContextState& c) { set_space(c.g.stroke, s.x[1]); });
    context_function("_CGContextSetFillColor", [set_components](CpuState& s, GuestMemory&, ContextState& c) { set_components(c.g.fill, s.x[1]); });
    context_function("_CGContextSetStrokeColor", [set_components](CpuState& s, GuestMemory&, ContextState& c) { set_components(c.g.stroke, s.x[1]); });
    context_function("_CGContextSetFillPattern", [set_pattern](CpuState& s, GuestMemory&, ContextState& c) { set_pattern(c.g.fill, s.x[1], s.x[2]); });
    context_function("_CGContextSetStrokePattern", [set_pattern](CpuState& s, GuestMemory&, ContextState& c) { set_pattern(c.g.stroke, s.x[1], s.x[2]); });
    context_function("_CGContextSetPatternPhase", [](CpuState& s, GuestMemory&, ContextState& c) { c.g.pattern_phase = size_arg(s); });
    const auto set_rgb = [this](Paint& paint, Rgba color) {
        paint.space = ObjectRef::retain(device_rgb());
        paint.pattern.reset();
        paint.color = {std::clamp(color.r, 0.0, 1.0), std::clamp(color.g, 0.0, 1.0), std::clamp(color.b, 0.0, 1.0), std::clamp(color.a, 0.0, 1.0)};
    };
    context_function("_CGContextSetRGBFillColor", [set_rgb](CpuState& s, GuestMemory&, ContextState& c) { set_rgb(c.g.fill, {d(s, 0), d(s, 1), d(s, 2), d(s, 3)}); });
    context_function("_CGContextSetRGBStrokeColor", [set_rgb](CpuState& s, GuestMemory&, ContextState& c) { set_rgb(c.g.stroke, {d(s, 0), d(s, 1), d(s, 2), d(s, 3)}); });
    context_function("_CGContextSetGrayFillColor", [this, set_rgb](CpuState& s, GuestMemory&, ContextState& c) {
        set_rgb(c.g.fill, {d(s, 0), d(s, 0), d(s, 0), d(s, 1)});
        c.g.fill.space = ObjectRef::retain(device_gray());
    });
    context_function("_CGContextSetGrayStrokeColor", [this, set_rgb](CpuState& s, GuestMemory&, ContextState& c) {
        set_rgb(c.g.stroke, {d(s, 0), d(s, 0), d(s, 0), d(s, 1)});
        c.g.stroke.space = ObjectRef::retain(device_gray());
    });
    const auto cmyk = [](const CpuState& s) {
        return Rgba{(1 - d(s, 0)) * (1 - d(s, 3)), (1 - d(s, 1)) * (1 - d(s, 3)), (1 - d(s, 2)) * (1 - d(s, 3)), d(s, 4)};
    };
    context_function("_CGContextSetCMYKFillColor", [set_rgb, cmyk](CpuState& s, GuestMemory&, ContextState& c) { set_rgb(c.g.fill, cmyk(s)); });
    context_function("_CGContextSetCMYKStrokeColor", [set_rgb, cmyk](CpuState& s, GuestMemory&, ContextState& c) { set_rgb(c.g.stroke, cmyk(s)); });

    // ---- the current path (stored in base space) ----
    context_function("_CGContextBeginPath", [](CpuState&, GuestMemory&, ContextState& c) { c.path = {}; });
    context_function("_CGContextMoveToPoint", [](CpuState& s, GuestMemory&, ContextState& c) { c.path.move_to(c.g.ctm.apply(point_arg(s))); });
    context_function("_CGContextAddLineToPoint", [](CpuState& s, GuestMemory&, ContextState& c) { c.path.line_to(c.g.ctm.apply(point_arg(s))); });
    context_function("_CGContextAddCurveToPoint", [](CpuState& s, GuestMemory&, ContextState& c) {
        c.path.cubic_to(c.g.ctm.apply(point_arg(s)), c.g.ctm.apply(point_arg(s, 2)), c.g.ctm.apply(point_arg(s, 4)));
    });
    context_function("_CGContextAddQuadCurveToPoint", [](CpuState& s, GuestMemory&, ContextState& c) {
        c.path.quad_to(c.g.ctm.apply(point_arg(s)), c.g.ctm.apply(point_arg(s, 2)));
    });
    context_function("_CGContextClosePath", [](CpuState&, GuestMemory&, ContextState& c) { c.path.close(); });
    context_function("_CGContextAddRect", [](CpuState& s, GuestMemory&, ContextState& c) { c.path.add_rect(c.g.ctm, rect_arg(s)); });
    context_function("_CGContextAddRects", [this](CpuState& s, GuestMemory&, ContextState& c) {
        for (Address index = 0; index < s.x[2]; ++index) c.path.add_rect(c.g.ctm, read_rect(s.x[1] + index * 32));
    });
    context_function("_CGContextAddLines", [this](CpuState& s, GuestMemory&, ContextState& c) {
        for (Address index = 0; index < s.x[2]; ++index) {
            const auto p = c.g.ctm.apply(read_point(s.x[1] + index * 16));
            if (index == 0) c.path.move_to(p); else c.path.line_to(p);
        }
    });
    context_function("_CGContextAddEllipseInRect", [](CpuState& s, GuestMemory&, ContextState& c) { c.path.add_ellipse(c.g.ctm, rect_arg(s)); });
    context_function("_CGContextAddArc", [](CpuState& s, GuestMemory&, ContextState& c) {
        // (c, x, y, radius, startAngle, endAngle, int clockwise)
        c.path.add_arc(c.g.ctm, point_arg(s), d(s, 2), d(s, 3), d(s, 4), static_cast<std::int32_t>(s.x[1]) != 0);
    });
    context_function("_CGContextAddArcToPoint", [](CpuState& s, GuestMemory&, ContextState& c) {
        c.path.add_arc_to_point(c.g.ctm, point_arg(s), point_arg(s, 2), d(s, 4));
    });
    context_function("_CGContextAddPath", [this](CpuState& s, GuestMemory&, ContextState& c) {
        if (!s.x[1]) return;
        c.path.append(require<PathInfo>(s.x[1], "CGPath")->data, c.g.ctm);
    });
    context_function("_CGContextIsPathEmpty", [](CpuState& s, GuestMemory&, ContextState& c) { return_bool(s, c.path.empty()); });
    context_function("_CGContextGetPathCurrentPoint", [](CpuState& s, GuestMemory&, ContextState& c) {
        const auto inverse = c.g.ctm.inverse();
        return_point(s, c.path.has_current && inverse ? inverse->apply(c.path.current) : Point{});
    });
    context_function("_CGContextGetPathBoundingBox", [](CpuState& s, GuestMemory&, ContextState& c) {
        const auto inverse = c.g.ctm.inverse();
        return_rect(s, inverse ? c.path.transformed(*inverse).control_bounds() : null_rect());
    });
    context_function("_CGContextCopyPath", [this](CpuState& s, GuestMemory&, ContextState& c) {
        const auto inverse = c.g.ctm.inverse();
        s.x[0] = make_path(inverse ? c.path.transformed(*inverse) : PathData{}, false);
    });
    context_function("_CGContextReplacePathWithStrokedPath", [](CpuState&, GuestMemory&, ContextState& c) {
        if (c.path.empty()) return;
        const auto inverse = c.g.ctm.inverse();
        if (!inverse) { c.path = {}; return; }
        const auto user = c.path.transformed(*inverse);
        const auto geometry = make_geometry(user, Affine{}, false, false);
        // The pen without dashes (CGContextReplacePathWithStrokedPath ignores them).
        const auto style = stroke_style(c.g.cap, c.g.join, c.g.miter_limit);
        const auto outline = geometry.widened(std::max(c.g.line_width, 1e-3), style, to_host(c.g.ctm));
        if (!outline) fail("widening a path failed (" + std::string(host::name(outline.error())) + ")");
        c.path = {};
        append(c.path, *outline);
    });
    context_function("_CGContextPathContainsPoint", [](CpuState& s, GuestMemory&, ContextState& c) {
        const auto mode = static_cast<std::int32_t>(s.x[1]);
        const auto p = c.g.ctm.apply(point_arg(s));
        bool inside{};
        if (mode == path_fill || mode == path_fill_stroke) inside = c.path.contains(p, false);
        else if (mode == path_eo_fill || mode == path_eo_fill_stroke) inside = c.path.contains(p, true);
        if (!inside && (mode == path_stroke || mode == path_fill_stroke || mode == path_eo_fill_stroke)) {
            const auto inverse = c.g.ctm.inverse();
            if (inverse && !c.path.empty()) {
                const auto geometry = make_geometry(c.path.transformed(*inverse), Affine{}, false, false);
                const auto hit = geometry.stroke_contains(to_host(inverse->apply(p)), c.g.line_width);
                if (!hit) fail("hit testing a stroke failed (" + std::string(host::name(hit.error())) + ")");
                inside = *hit;
            }
        }
        return_bool(s, inside);
    });

    // ---- painting ----
    const auto draw_path = [this](ContextState& c, std::int32_t mode) {
        auto path = std::move(c.path);
        c.path = {};
        switch (mode) {
        case path_fill: fill_path(*this, c, path, false); break;
        case path_eo_fill: fill_path(*this, c, path, true); break;
        case path_stroke: stroke_path(*this, c, path); break;
        case path_fill_stroke: fill_path(*this, c, path, false); stroke_path(*this, c, path); break;
        case path_eo_fill_stroke: fill_path(*this, c, path, true); stroke_path(*this, c, path); break;
        default: fail("invalid CGPathDrawingMode " + std::to_string(mode));
        }
    };
    context_function("_CGContextDrawPath", [draw_path](CpuState& s, GuestMemory&, ContextState& c) { draw_path(c, static_cast<std::int32_t>(s.x[1])); });
    context_function("_CGContextFillPath", [draw_path](CpuState&, GuestMemory&, ContextState& c) { draw_path(c, path_fill); });
    context_function("_CGContextEOFillPath", [draw_path](CpuState&, GuestMemory&, ContextState& c) { draw_path(c, path_eo_fill); });
    context_function("_CGContextStrokePath", [draw_path](CpuState&, GuestMemory&, ContextState& c) { draw_path(c, path_stroke); });
    const auto shape = [](ContextState& c, const std::function<void(PathData&)>& build) {
        PathData path;
        build(path);
        return path;
    };
    context_function("_CGContextFillRect", [this, shape](CpuState& s, GuestMemory&, ContextState& c) {
        fill_path(*this, c, shape(c, [&](PathData& p) { p.add_rect(c.g.ctm, rect_arg(s)); }), false);
    });
    context_function("_CGContextFillRects", [this, shape](CpuState& s, GuestMemory&, ContextState& c) {
        fill_path(*this, c, shape(c, [&](PathData& p) {
            for (Address index = 0; index < s.x[2]; ++index) p.add_rect(c.g.ctm, read_rect(s.x[1] + index * 32));
        }), false);
    });
    context_function("_CGContextStrokeRect", [this, shape](CpuState& s, GuestMemory&, ContextState& c) {
        stroke_path(*this, c, shape(c, [&](PathData& p) { p.add_rect(c.g.ctm, rect_arg(s)); }));
    });
    context_function("_CGContextStrokeRectWithWidth", [this, shape](CpuState& s, GuestMemory&, ContextState& c) {
        const auto saved = c.g.line_width;
        c.g.line_width = std::max(0.0, d(s, 4));
        try { stroke_path(*this, c, shape(c, [&](PathData& p) { p.add_rect(c.g.ctm, rect_arg(s)); })); } catch (...) { c.g.line_width = saved; throw; }
        c.g.line_width = saved;
    });
    context_function("_CGContextClearRect", [this](CpuState& s, GuestMemory&, ContextState& c) { clear_rect(*this, c, rect_arg(s)); });
    context_function("_CGContextFillEllipseInRect", [this, shape](CpuState& s, GuestMemory&, ContextState& c) {
        fill_path(*this, c, shape(c, [&](PathData& p) { p.add_ellipse(c.g.ctm, rect_arg(s)); }), false);
    });
    context_function("_CGContextStrokeEllipseInRect", [this, shape](CpuState& s, GuestMemory&, ContextState& c) {
        stroke_path(*this, c, shape(c, [&](PathData& p) { p.add_ellipse(c.g.ctm, rect_arg(s)); }));
    });
    context_function("_CGContextStrokeLineSegments", [this, shape](CpuState& s, GuestMemory&, ContextState& c) {
        stroke_path(*this, c, shape(c, [&](PathData& p) {
            for (Address index = 0; index + 1 < s.x[2]; index += 2) {
                p.move_to(c.g.ctm.apply(read_point(s.x[1] + index * 16)));
                p.line_to(c.g.ctm.apply(read_point(s.x[1] + (index + 1) * 16)));
            }
        }));
    });

    // ---- clipping ----
    context_function("_CGContextClip", [this](CpuState&, GuestMemory&, ContextState& c) {
        clip_to_path(*this, c, c.path, false);
        c.path = {};
    });
    context_function("_CGContextEOClip", [this](CpuState&, GuestMemory&, ContextState& c) {
        clip_to_path(*this, c, c.path, true);
        c.path = {};
    });
    context_function("_CGContextClipToRect", [this](CpuState& s, GuestMemory&, ContextState& c) {
        PathData path;
        path.add_rect(c.g.ctm, rect_arg(s));
        clip_to_path(*this, c, path, false);
    });
    context_function("_CGContextClipToRects", [this](CpuState& s, GuestMemory&, ContextState& c) {
        PathData path;
        for (Address index = 0; index < s.x[2]; ++index) path.add_rect(c.g.ctm, standardize(read_rect(s.x[1] + index * 32)));
        clip_to_path(*this, c, path, false);
    });
    context_function("_CGContextClipToMask", [this](CpuState& s, GuestMemory&, ContextState& c) {
        if (s.x[1]) clip_to_mask(*this, c, rect_arg(s), s.x[1]);
    });
    context_function("_CGContextResetClip", [](CpuState&, GuestMemory&, ContextState& c) { c.g.clips.clear(); });
    context_function("_CGContextGetClipBoundingBox", [](CpuState& s, GuestMemory&, ContextState& c) {
        Rect bounds{{0, 0}, {static_cast<double>(c.layout.width), static_cast<double>(c.layout.height)}};
        for (const auto& clip : c.g.clips) bounds = rect_intersection(bounds, clip->device);
        if (rect_is_null(bounds)) { return_rect(s, null_rect()); return; }
        const Affine device_to_base{1, 0, 0, -1, 0, static_cast<double>(c.layout.height)};
        const auto inverse = c.g.ctm.inverse();
        return_rect(s, inverse ? transform_rect(device_to_base.then(*inverse), bounds) : null_rect());
    });

    // ---- images, gradients, shadings, layers ----
    context_function("_CGContextDrawImage", [this](CpuState& s, GuestMemory&, ContextState& c) { draw_image(*this, c, s.x[1], rect_arg(s)); });
    context_function("_CGContextDrawTiledImage", [this](CpuState& s, GuestMemory&, ContextState& c) { draw_tiled_image(*this, c, s.x[1], rect_arg(s)); });
    context_function("_CGContextDrawLinearGradient", [this](CpuState& s, GuestMemory&, ContextState& c) {
        if (!s.x[1]) return;
        const auto gradient = require<GradientInfo>(s.x[1], "CGGradient");
        const auto options = static_cast<std::uint32_t>(s.x[2]);
        draw_gradient(*this, c, *gradient, false, point_arg(s), 0, point_arg(s, 2), 0, options & 1, options & 2);
    });
    context_function("_CGContextDrawRadialGradient", [this](CpuState& s, GuestMemory&, ContextState& c) {
        if (!s.x[1]) return;
        const auto gradient = require<GradientInfo>(s.x[1], "CGGradient");
        const auto options = static_cast<std::uint32_t>(s.x[2]);
        draw_gradient(*this, c, *gradient, true, point_arg(s), d(s, 2), point_arg(s, 3), d(s, 5), options & 1, options & 2);
    });
    context_function("_CGContextDrawShading", [this](CpuState& s, GuestMemory&, ContextState& c) {
        if (!s.x[1]) return;
        const auto shading = require<ShadingInfo>(s.x[1], "CGShading");
        draw_gradient(*this, c, *shading->sampled, shading->radial, shading->start, shading->start_radius, shading->end, shading->end_radius,
                      shading->extend_start, shading->extend_end);
    });
    context_function("_CGContextBeginTransparencyLayer", [this](CpuState&, GuestMemory&, ContextState& c) { begin_transparency_layer(*this, c, std::nullopt); });
    context_function("_CGContextBeginTransparencyLayerWithRect", [this](CpuState& s, GuestMemory&, ContextState& c) {
        begin_transparency_layer(*this, c, rect_arg(s));
    });
    context_function("_CGContextEndTransparencyLayer", [this](CpuState&, GuestMemory&, ContextState& c) { end_transparency_layer(*this, c); });

    // CGLayer: an offscreen bitmap context drawn as an image.
    function(cg, "_CGLayerCreateWithContext", [this](CpuState& s, GuestMemory&) {
        // (context, size d0 d1, auxiliaryInfo)
        const auto size = size_arg(s);
        if (!s.x[0] || size.width <= 0 || size.height <= 0) { s.x[0] = 0; return; }
        const auto parent = context_state(*this, s.x[0]);
        const auto scale = parent->base_ctm.scale();
        const auto context = create_bitmap_context(static_cast<std::uint32_t>(std::ceil(size.width * scale)),
                                                   static_cast<std::uint32_t>(std::ceil(size.height * scale)), scale);
        auto info = std::make_shared<LayerInfo>();
        info->context = context;
        info->size = size;
        s.x[0] = create(layer_class, info, [this, info](Address) { release(std::exchange(info->context, 0)); });
    });
    function(cg, "_CGLayerGetContext", [this](CpuState& s, GuestMemory&) { s.x[0] = s.x[0] ? require<LayerInfo>(s.x[0], "CGLayer")->context : 0; });
    function(cg, "_CGLayerGetSize", [this](CpuState& s, GuestMemory&) { return_size(s, s.x[0] ? require<LayerInfo>(s.x[0], "CGLayer")->size : Size{}); });
    function(cg, "_CGLayerRetain", [this](CpuState& s, GuestMemory&) { s.x[0] = retain(s.x[0]); });
    function(cg, "_CGLayerRelease", [this](CpuState& s, GuestMemory&) { release(s.x[0]); });
    function(cg, "_CGLayerGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = 0x100; });
    const auto draw_layer = [this](ContextState& c, Address layer, const Rect& rect) {
        if (!layer) return;
        const auto info = require<LayerInfo>(layer, "CGLayer");
        const auto source = context_state(*this, info->context);
        Address image{};
        {
            std::lock_guard lock(source->mutex);
            auto bytes = std::make_shared<std::vector<std::uint8_t>>(static_cast<std::size_t>(source->layout.byte_size()));
            memory().read_bytes(source->data, *bytes);
            image = make_image(source->layout, source->space.get(), bytes);
        }
        try { draw_image(*this, c, image, rect); } catch (...) { release(image); throw; }
        release(image);
    };
    context_function("_CGContextDrawLayerInRect", [this, draw_layer](CpuState& s, GuestMemory&, ContextState& c) { draw_layer(c, s.x[1], rect_arg(s)); });
    context_function("_CGContextDrawLayerAtPoint", [this, draw_layer](CpuState& s, GuestMemory&, ContextState& c) {
        if (!s.x[1]) return;
        draw_layer(c, s.x[1], {point_arg(s), require<LayerInfo>(s.x[1], "CGLayer")->size});
    });

    // ---- text (CGFont-based) ----
    context_function("_CGContextSetTextMatrix", [this](CpuState& s, GuestMemory&, ContextState& c) { c.text_matrix = read_affine(s.x[1]); });
    function(cg, "_CGContextGetTextMatrix", [this, with](CpuState& s, GuestMemory&) {
        Affine value{};
        with(s.x[0], [&](ContextState& c) { value = c.text_matrix; });
        write_affine(s.x[8], value);
    });
    context_function("_CGContextSetTextPosition", [](CpuState& s, GuestMemory&, ContextState& c) { c.text_matrix.tx = d(s, 0); c.text_matrix.ty = d(s, 1); });
    context_function("_CGContextGetTextPosition", [](CpuState& s, GuestMemory&, ContextState& c) { return_point(s, {c.text_matrix.tx, c.text_matrix.ty}); });
    context_function("_CGContextSetCharacterSpacing", [](CpuState& s, GuestMemory&, ContextState& c) { c.g.character_spacing = d(s, 0); });
    context_function("_CGContextSetTextDrawingMode", [](CpuState& s, GuestMemory&, ContextState& c) {
        const auto mode = static_cast<std::int32_t>(s.x[1]);
        if (mode < text_mode::fill || mode > text_mode::clip) fail("invalid CGTextDrawingMode " + std::to_string(mode));
        c.g.text_mode = mode;
    });
    context_function("_CGContextSetFont", [this](CpuState& s, GuestMemory&, ContextState& c) {
        if (s.x[1]) static_cast<void>(require<GraphicsFontState>(s.x[1], "CGFont"));
        c.g.font = ObjectRef::retain(s.x[1]);
    });
    context_function("_CGContextSetFontSize", [](CpuState& s, GuestMemory&, ContextState& c) { c.g.font_size = d(s, 0); });
    const auto glyph_run = [this](ContextState& c) {
        if (!c.g.font) fail("drawing glyphs without a font (CGContextSetFont)");
        GlyphRunDraw run;
        run.face = require<GraphicsFontState>(c.g.font.get(), "CGFont")->face;
        run.size = c.g.font_size;
        return run;
    };
    const auto read_glyphs = [this](Address glyphs, Address count) {
        return read_units(glyphs, static_cast<std::size_t>(std::min<Address>(count, 1 << 20)));
    };
    context_function("_CGContextShowGlyphsAtPositions", [this, glyph_run, read_glyphs](CpuState& s, GuestMemory&, ContextState& c) {
        // (c, glyphs, positions, count)
        if (!s.x[3]) return;
        auto run = glyph_run(c);
        run.glyphs = read_glyphs(s.x[1], s.x[3]);
        for (Address index = 0; index < s.x[3]; ++index) run.positions.push_back(read_point(s.x[2] + index * 16));
        draw_glyphs(*this, c, run, {}, std::nullopt);
    });
    context_function("_CGContextShowGlyphsWithAdvances", [this, glyph_run, read_glyphs](CpuState& s, GuestMemory&, ContextState& c) {
        // (c, glyphs, advances (CGSize), count): advances in user space
        if (!s.x[3]) return;
        auto run = glyph_run(c);
        run.glyphs = read_glyphs(s.x[1], s.x[3]);
        Point pen{};
        for (Address index = 0; index < s.x[3]; ++index) {
            run.positions.push_back(pen);
            const auto advance = read_point(s.x[2] + index * 16);
            pen.x += advance.x + c.g.character_spacing;
            pen.y += advance.y;
        }
        draw_glyphs(*this, c, run, {}, std::nullopt);
        c.text_matrix.tx += pen.x * c.text_matrix.a + pen.y * c.text_matrix.c;
        c.text_matrix.ty += pen.x * c.text_matrix.b + pen.y * c.text_matrix.d;
    });
    const auto show_glyphs = [this, glyph_run](ContextState& c, std::vector<std::uint16_t> glyphs) {
        auto run = glyph_run(c);
        const auto advances = advances_for(*run.face, run.size, glyphs);
        double pen{};
        for (std::size_t index = 0; index < glyphs.size(); ++index) {
            run.positions.push_back({pen, 0});
            pen += advances[index] + c.g.character_spacing;
        }
        run.glyphs = std::move(glyphs);
        draw_glyphs(*this, c, run, {}, std::nullopt);
        c.text_matrix.tx += pen * c.text_matrix.a;
        c.text_matrix.ty += pen * c.text_matrix.b;
    };
    context_function("_CGContextShowGlyphs", [show_glyphs, read_glyphs](CpuState& s, GuestMemory&, ContextState& c) {
        show_glyphs(c, read_glyphs(s.x[1], s.x[2]));
    });
    context_function("_CGContextShowGlyphsAtPoint", [show_glyphs, read_glyphs](CpuState& s, GuestMemory&, ContextState& c) {
        c.text_matrix.tx = d(s, 0);
        c.text_matrix.ty = d(s, 1);
        show_glyphs(c, read_glyphs(s.x[1], s.x[2]));
    });
    // Deprecated MacRoman text drawing (CGContextSelectFont / ShowText).
    context_function("_CGContextSelectFont", [this](CpuState& s, GuestMemory& m, ContextState& c) {
        // (c, const char* name, size d0, CGTextEncoding)
        bool terminated{};
        const auto raw = s.x[1] ? m.read_string(s.x[1], 1, 256, &terminated) : std::vector<std::uint8_t>{};
        const auto resolved = resolve_font(std::string(raw.begin(), raw.end()));
        if (!resolved) { diagnostic("CGContextSelectFont: no font named " + std::string(raw.begin(), raw.end())); return; }
        auto info = std::make_shared<GraphicsFontState>();
        info->face = resolved->face;
        info->postscript = resolved->postscript;
        info->family = resolved->family;
        c.g.font = ObjectRef::adopt(create(font_class, info));
        c.g.font_size = d(s, 0);
    });
    const auto show_text = [show_glyphs](ContextState& c, const std::vector<std::uint8_t>& bytes) {
        if (!c.g.font) return;
        auto* q = provider();
        const auto face = q->require<GraphicsFontState>(c.g.font.get(), "CGFont")->face;
        std::vector<std::uint16_t> units(bytes.begin(), bytes.end());
        show_glyphs(c, glyphs_for(*face, units));
    };
    context_function("_CGContextShowText", [this, show_text](CpuState& s, GuestMemory&, ContextState& c) {
        show_text(c, read(s.x[1], static_cast<std::size_t>(std::min<Address>(s.x[2], 1 << 20))));
    });
    context_function("_CGContextShowTextAtPoint", [this, show_text](CpuState& s, GuestMemory&, ContextState& c) {
        c.text_matrix.tx = d(s, 0);
        c.text_matrix.ty = d(s, 1);
        show_text(c, read(s.x[1], static_cast<std::size_t>(std::min<Address>(s.x[2], 1 << 20))));
    });
}

} // namespace alchemy::gui::quartz2d
