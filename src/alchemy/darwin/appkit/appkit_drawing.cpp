#include "alchemy/darwin/kit/completion/graphics/providers.hpp"

#include "alchemy/darwin/quartzcore/graphics_math.hpp"
#include "alchemy/darwin/quartzcore/graphics_support.hpp"

#include "alchemy/engine_adapter.hpp"

#include <cmath>
#include <cstdio>
#include <map>
#include <numbers>
#include <set>

// AppKit drawing additions QLab uses, over the CoreGraphics provider (the
// current NSGraphicsContext's CGContext):
//   NSShadow                 offset, blur radius, color (black at 1/3 alpha by
//                            default), -set (CGContextSetShadowWithColor), nib decoding;
//   NSGradient               color stops in sRGB, interpolation, linear and
//                            radial drawing (CGGradient), -drawInRect:angle: /
//                            -drawInBezierPath:angle: spanning the shape corner to corner;
//   NSAffineTransform        the 2D matrix (graphics_math.cpp) with
//                            -concat on the current context (-set is not provided);
//   NSColor                  HSB components and constructors, +colorWithCIColor:,
//                            +colorWithPatternImage: (NSPatternColor: a colored CGPattern
//                            tiling the image when set);
//   NSImageSymbolConfiguration  the configuration values;
//   NSImage symbols          +imageWithSystemSymbolName:accessibilityDescription:
//                            answers nil (no symbol images exist under Alchemy: the
//                            SF Symbols artwork is Apple's), as for an unknown name.
// (+[NSImage imageWithSize:flipped:drawingHandler:] and NSCustomImageRep are
// the Quartz 2D layer's: src/alchemy/darwin/coregraphics/appkit_drawing.cpp.)
namespace alchemy::gui::completion_kit::graphics {
namespace {

constexpr std::string_view appkit = "/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit";

struct Drawing {
    Support& support;

    Address cg(const char* what) {
        const auto context_class = support.objc.find_class("NSGraphicsContext");
        const auto context = context_class ? support.send_id(context_class, "currentContext") : 0;
        const auto result = context ? support.send_id(context, "CGContext") : 0;
        static_cast<void>(what);
        return result;   // no current context: nothing is drawn, as in AppKit
    }
    // Straight sRGB components of an NSColor.
    Rgba rgba(Address color) {
        if (!color) return {0, 0, 0, 0};
        const auto buffer = support.process.allocate_guest(32);
        CallRegs registers;
        for (unsigned i = 0; i < 4; ++i) registers.x[2 + i] = buffer + i * 8;
        support.send(color, "getRed:green:blue:alpha:", registers);
        Rgba value;
        value.r = std::bit_cast<double>(support.memory().read64(buffer));
        value.g = std::bit_cast<double>(support.memory().read64(buffer + 8));
        value.b = std::bit_cast<double>(support.memory().read64(buffer + 16));
        value.a = std::bit_cast<double>(support.memory().read64(buffer + 24));
        support.process.release_guest(buffer);
        return value;
    }
    Address color(double r, double g, double b, double a) {
        CallRegs registers;
        registers.set_d(0, r); registers.set_d(1, g); registers.set_d(2, b); registers.set_d(3, a);
        return support.send(support.objc.find_class("NSColor"), "colorWithSRGBRed:green:blue:alpha:", registers).x0;
    }
    void cg_call(const char* symbol, const CallRegs& registers) { static_cast<void>(support.call(coregraphics_library, symbol, registers)); }
    Address cg_id(const char* symbol, std::initializer_list<Address> arguments) { return support.call(coregraphics_library, symbol, arguments); }
};

// ---- NSGradient ------------------------------------------------------------------------
struct GradientStops { std::vector<double> locations; };   // colors are the bag's "color<i>"

std::vector<GradientStop> stops_of(Support& support, Drawing& drawing, Address gradient) {
    const auto b = bag(support, gradient);
    std::vector<double> locations;
    {
        std::lock_guard lock(b->mutex);
        if (const auto* value = std::any_cast<GradientStops>(&b->extra)) locations = value->locations;
    }
    std::vector<GradientStop> stops;
    for (std::size_t i = 0; i < locations.size(); ++i)
        stops.push_back({drawing.rgba(bag_object(support, gradient, "color" + std::to_string(i))), locations[i]});
    return stops;
}

// +1 CGGradient of the stops (sRGB components).
Address cg_gradient(Support& support, Drawing& drawing, const std::vector<GradientStop>& stops) {
    const auto count = stops.size();
    const auto components = support.process.allocate_guest(count * 32 + 8);
    const auto locations = support.process.allocate_guest(count * 8 + 8);
    for (std::size_t i = 0; i < count; ++i) {
        const std::array<double, 4> c{stops[i].color.r, stops[i].color.g, stops[i].color.b, stops[i].color.a};
        for (unsigned k = 0; k < 4; ++k) support.memory().write64(components + (i * 4 + k) * 8, std::bit_cast<std::uint64_t>(c[k]));
        support.memory().write64(locations + i * 8, std::bit_cast<std::uint64_t>(stops[i].location));
    }
    const auto space = drawing.cg_id("_CGColorSpaceCreateWithName", {support.cf.constant_string("kCGColorSpaceSRGB")});
    const auto gradient = drawing.cg_id("_CGGradientCreateWithColorComponents", {space, components, locations, count});
    drawing.cg_id("_CGColorSpaceRelease", {space});
    support.process.release_guest(components);
    support.process.release_guest(locations);
    return gradient;
}

void draw_linear(Support& support, Drawing& drawing, Address gradient, double x0, double y0, double x1, double y1, std::uint64_t options) {
    const auto context = drawing.cg("NSGradient drawing");
    if (!context) return;
    const auto stops = stops_of(support, drawing, gradient);
    const auto made = cg_gradient(support, drawing, stops);
    CallRegs registers;
    registers.x[0] = context; registers.x[1] = made; registers.x[2] = options;
    registers.set_d(0, x0); registers.set_d(1, y0); registers.set_d(2, x1); registers.set_d(3, y1);
    drawing.cg_call("_CGContextDrawLinearGradient", registers);
    drawing.cg_id("_CGGradientRelease", {made});
}

void draw_radial(Support& support, Drawing& drawing, Address gradient, double x0, double y0, double r0, double x1, double y1, double r1,
                 std::uint64_t options) {
    const auto context = drawing.cg("NSGradient drawing");
    if (!context) return;
    const auto made = cg_gradient(support, drawing, stops_of(support, drawing, gradient));
    CallRegs registers;
    registers.x[0] = context; registers.x[1] = made; registers.x[2] = options;
    registers.set_d(0, x0); registers.set_d(1, y0); registers.set_d(2, r0);
    registers.set_d(3, x1); registers.set_d(4, y1); registers.set_d(5, r1);
    drawing.cg_call("_CGContextDrawRadialGradient", registers);
    drawing.cg_id("_CGGradientRelease", {made});
}

void set_stops(Support& support, Address gradient, const std::vector<Address>& colors, std::vector<double> locations) {
    for (std::size_t i = 0; i < colors.size(); ++i) bag_set_object(support, gradient, "color" + std::to_string(i), colors[i]);
    const auto b = bag(support, gradient);
    std::lock_guard lock(b->mutex);
    b->extra = GradientStops{std::move(locations)};
}

// ---- NSAffineTransform ---------------------------------------------------------------------
Affine matrix_of(Support& support, Address object) {
    const auto b = bag(support, object);
    std::lock_guard lock(b->mutex);
    if (const auto* value = std::any_cast<Affine>(&b->extra)) return *value;
    return {};
}
void set_matrix(Support& support, Address object, const Affine& value) {
    const auto b = bag(support, object);
    std::lock_guard lock(b->mutex);
    b->extra = value;
}
Affine read_struct(GuestMemory& m, Address at) {
    if (!at) fail("NSAffineTransformStruct argument without storage");
    std::array<double, 6> v{};
    for (unsigned i = 0; i < 6; ++i) v[i] = std::bit_cast<double>(m.read64(at + i * 8));
    return {v[0], v[1], v[2], v[3], v[4], v[5]};
}
void write_struct(GuestMemory& m, Address at, const Affine& t) {
    if (!at) fail("NSAffineTransformStruct result without storage (x8)");
    const std::array<double, 6> v{t.m11, t.m12, t.m21, t.m22, t.tx, t.ty};
    for (unsigned i = 0; i < 6; ++i) m.write64(at + i * 8, std::bit_cast<std::uint64_t>(v[i]));
}

// ---- NSPatternColor ------------------------------------------------------------------------------
// Pattern colors keep their image here (NSColor instances carry AppKit's
// color state), with the CGPattern made on first use.
struct PatternColors {
    std::mutex mutex;
    struct Entry { Address image{}; Address pattern{}; };   // image +1, pattern +1
    std::map<Address, Entry> colors;
    Address draw_callback{};
};

} // namespace

void install_appkit_drawing(Support& support) {
    auto* sp = &support;
    auto drawing = std::make_shared<Drawing>(Drawing{support});
    const auto key = [sp](const char* name) { return sp->cf.constant_string(name); };

    // ---- NSShadow ---------------------------------------------------------------------------------
    const auto shadow = support.declare_class("NSShadow", "NSObject", appkit);
    support.objc.add_native_protocol(shadow, "NSCopying");
    support.objc.add_native_protocol(shadow, "NSSecureCoding");
    const auto default_shadow_color = [sp, drawing] {
        // Black with an alpha of 1/3 (made once, kept for the process).
        static Address color{};
        if (!color) color = sp->retain(drawing->color(0, 0, 0, 1.0 / 3.0));
        return color;
    };
    Props(support, shadow)
        .lifecycle()
        .point("shadowOffset", "setShadowOffset:")
        .real("shadowBlurRadius", "setShadowBlurRadius:")
        .object("shadowColor", "setShadowColor:", false, default_shadow_color);
    support.method(shadow, "set", [sp, drawing](CpuState& s, GuestMemory&) {
        const auto context = drawing->cg("-[NSShadow set]");
        if (!context) return;
        const auto offset = bag_raw(*sp, s.x[0], "shadowOffset").value_or(std::array<std::uint64_t, 4>{});
        const auto blur = bag_real(*sp, s.x[0], "shadowBlurRadius").value_or(0);
        const auto color = sp->send_id(s.x[0], "shadowColor");
        CallRegs registers;
        registers.x[0] = context;
        registers.x[1] = color ? sp->send_id(color, "CGColor") : 0;
        registers.v[0] = {offset[0], 0};
        registers.v[1] = {offset[1], 0};
        registers.set_d(2, blur);
        drawing->cg_call("_CGContextSetShadowWithColor", registers);
    }, "v@:");
    support.method(shadow, "supportsSecureCoding", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:", true);
    support.method(shadow, "initWithCoder:", [sp, key](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], coder = s.x[2];
        const auto real = [&](const char* name) {
            CallRegs registers;
            registers.x[2] = key(name);
            return sp->send(coder, "decodeDoubleForKey:", registers).d(0);
        };
        bag_set_raw(*sp, self, "shadowOffset",
                    {std::bit_cast<std::uint64_t>(real("NSShadowHoriz")), std::bit_cast<std::uint64_t>(real("NSShadowVert")), 0, 0});
        bag_set_raw(*sp, self, "shadowBlurRadius", {std::bit_cast<std::uint64_t>(real("NSShadowBlurRadius")), 0, 0, 0});
        if (const auto color = sp->send_id(coder, "decodeObjectForKey:", {key("NSShadowColor")}))
            bag_set_object(*sp, self, "shadowColor", color);
    }, "@@:@");

    support.method(shadow, "encodeWithCoder:", [sp, key](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], coder = s.x[2];
        const auto offset = bag_raw(*sp, self, "shadowOffset").value_or(std::array<std::uint64_t, 4>{});
        const auto put = [&](const char* name, std::uint64_t bits) {
            CallRegs registers;
            registers.v[0] = {bits, 0};
            registers.x[2] = key(name);
            sp->send(coder, "encodeDouble:forKey:", registers);
        };
        put("NSShadowHoriz", offset[0]);
        put("NSShadowVert", offset[1]);
        put("NSShadowBlurRadius", std::bit_cast<std::uint64_t>(bag_real(*sp, self, "shadowBlurRadius").value_or(0)));
        if (const auto color = bag_object(*sp, self, "shadowColor")) sp->send_id(coder, "encodeObject:forKey:", {color, key("NSShadowColor")});
    }, "v@:@");

    // ---- NSGradient -------------------------------------------------------------------------------------
    const auto gradient = support.declare_class("NSGradient", "NSObject", appkit);
    support.objc.add_native_protocol(gradient, "NSCopying");
    Props(support, gradient).lifecycle();
    // Fewer than two colors: nil (the initializers fail as AppKit's do).
    const auto finish_init = [sp](CpuState& s, const std::vector<Address>& colors, std::vector<double> locations) {
        if (colors.size() < 2) { sp->release(s.x[0]); s.x[0] = 0; return; }
        if (locations.empty())
            for (std::size_t i = 0; i < colors.size(); ++i) locations.push_back(static_cast<double>(i) / (colors.size() - 1));
        set_stops(*sp, s.x[0], colors, std::move(locations));
    };
    support.method(gradient, "initWithStartingColor:endingColor:", [finish_init](CpuState& s, GuestMemory&) {
        if (!s.x[2] || !s.x[3]) fail("-[NSGradient initWithStartingColor:endingColor:] with a nil color");
        finish_init(s, {s.x[2], s.x[3]}, {0, 1});
    }, "@@:@@");
    support.method(gradient, "initWithColors:", [sp, finish_init](CpuState& s, GuestMemory&) {
        finish_init(s, array_items(*sp, s.x[2]), {});
    }, "@@:@");
    support.method(gradient, "initWithColors:atLocations:colorSpace:", [sp, finish_init](CpuState& s, GuestMemory& m) {
        const auto colors = array_items(*sp, s.x[2]);
        std::vector<double> locations;
        if (s.x[3]) for (std::size_t i = 0; i < colors.size(); ++i) locations.push_back(std::bit_cast<double>(m.read64(s.x[3] + i * 8)));
        finish_init(s, colors, locations);
    }, "@@:@^d@");
    // (firstColor, ...): variadic location, color, location, ..., nil, each
    // in an 8-byte stack slot.
    support.method(gradient, "initWithColorsAndLocations:", [finish_init](CpuState& s, GuestMemory& m) {
        std::vector<Address> colors;
        std::vector<double> locations;
        auto color = s.x[2];
        auto slot = s.sp;
        while (color) {
            colors.push_back(color);
            locations.push_back(std::bit_cast<double>(m.read64(slot)));
            color = m.read64(slot + 8);
            slot += 16;
            if (colors.size() > 4096) fail("-[NSGradient initWithColorsAndLocations:] list is not nil-terminated");
        }
        finish_init(s, colors, locations);
    }, "@@:@");
    support.method(gradient, "numberOfColorStops", [sp, drawing](CpuState& s, GuestMemory&) {
        s.x[0] = stops_of(*sp, *drawing, s.x[0]).size();
    }, "q@:");
    support.method(gradient, "getColor:location:atIndex:", [sp](CpuState& s, GuestMemory& m) {
        const auto b = bag(*sp, s.x[0]);
        std::vector<double> locations;
        {
            std::lock_guard lock(b->mutex);
            if (const auto* value = std::any_cast<GradientStops>(&b->extra)) locations = value->locations;
        }
        const auto index = s.x[4];
        if (index >= locations.size()) fail("NSRangeException: -[NSGradient getColor:location:atIndex:] index " + std::to_string(index));
        if (s.x[2]) m.write64(s.x[2], bag_object(*sp, s.x[0], "color" + std::to_string(index)));
        if (s.x[3]) m.write64(s.x[3], std::bit_cast<std::uint64_t>(locations[index]));
    }, "v@:^@^dq");
    support.method(gradient, "interpolatedColorAtLocation:", [sp, drawing](CpuState& s, GuestMemory&) {
        const auto c = gradient_color_at(stops_of(*sp, *drawing, s.x[0]), d(s, 0));
        s.x[0] = drawing->color(c.r, c.g, c.b, c.a);
    }, "@@:d");
    support.method(gradient, "colorSpace", [sp](CpuState& s, GuestMemory&) {
        s.x[0] = sp->send_id(sp->objc.find_class("NSColorSpace"), "sRGBColorSpace");
    }, "@@:");
    constexpr std::uint64_t both_ends = 3;   // kCGGradientDrawsBeforeStartLocation | AfterEndLocation
    support.method(gradient, "drawFromPoint:toPoint:options:", [sp, drawing](CpuState& s, GuestMemory&) {
        draw_linear(*sp, *drawing, s.x[0], d(s, 0), d(s, 1), d(s, 2), d(s, 3), s.x[2]);
    }, "v@:{CGPoint=dd}{CGPoint=dd}Q");
    support.method(gradient, "drawFromCenter:radius:toCenter:radius:options:", [sp, drawing](CpuState& s, GuestMemory&) {
        draw_radial(*sp, *drawing, s.x[0], d(s, 0), d(s, 1), d(s, 2), d(s, 3), d(s, 4), d(s, 5), s.x[2]);
    }, "v@:{CGPoint=dd}d{CGPoint=dd}dQ");
    // Clipped to `clip` (a rect or a path), then `paint`.
    const auto clipped = [sp, drawing](Address path, std::optional<std::array<double, 4>> rect, const std::function<void()>& paint) {
        const auto context = drawing->cg("NSGradient drawing");
        if (!context) return;
        drawing->cg_id("_CGContextSaveGState", {context});
        try {
            if (rect) {
                CallRegs registers;
                registers.x[0] = context;
                for (unsigned i = 0; i < 4; ++i) registers.set_d(i, (*rect)[i]);
                drawing->cg_call("_CGContextClipToRect", registers);
            } else {
                sp->send_id(path, "addClip");
            }
            paint();
        } catch (...) {
            drawing->cg_id("_CGContextRestoreGState", {context});
            throw;
        }
        drawing->cg_id("_CGContextRestoreGState", {context});
    };
    const auto linear_in = [sp, drawing](Address self, const std::array<double, 4>& r, double angle) {
        const auto line = gradient_line(r[0], r[1], r[2], r[3], angle);
        draw_linear(*sp, *drawing, self, line.x0, line.y0, line.x1, line.y1, both_ends);
    };
    support.method(gradient, "drawInRect:angle:", [clipped, linear_in](CpuState& s, GuestMemory&) {
        const auto r = rect_arg(s);
        if (!(r[2] > 0) || !(r[3] > 0)) return;
        const auto self = s.x[0];
        const auto angle = d(s, 4);
        clipped(0, r, [&] { linear_in(self, r, angle); });
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}d");
    support.method(gradient, "drawInBezierPath:angle:", [sp, clipped, linear_in](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], path = s.x[2];
        if (!path) return;
        const auto bounds = result_rect(sp->send(path, "bounds"));
        const auto angle = d(s, 0);
        clipped(path, std::nullopt, [&] { linear_in(self, bounds, angle); });
    }, "v@:@d");
    // Radial fill of a rect: centered at the rect's center offset by
    // relativeCenterPosition (each in [-1, 1]), from radius 0 to the farthest corner.
    const auto radial_in = [sp, drawing](Address self, const std::array<double, 4>& r, double rx, double ry) {
        const auto cx = r[0] + r[2] / 2 * (1 + std::clamp(rx, -1.0, 1.0));
        const auto cy = r[1] + r[3] / 2 * (1 + std::clamp(ry, -1.0, 1.0));
        double radius = 0;
        for (const auto& [x, y] : std::array<std::pair<double, double>, 4>{{{r[0], r[1]}, {r[0] + r[2], r[1]}, {r[0], r[1] + r[3]},
                                                                             {r[0] + r[2], r[1] + r[3]}}})
            radius = std::max(radius, std::hypot(x - cx, y - cy));
        draw_radial(*sp, *drawing, self, cx, cy, 0, cx, cy, radius, both_ends);
    };
    support.method(gradient, "drawInRect:relativeCenterPosition:", [clipped, radial_in](CpuState& s, GuestMemory&) {
        const auto r = rect_arg(s);
        const auto self = s.x[0];
        const auto rx = d(s, 4), ry = d(s, 5);
        clipped(0, r, [&] { radial_in(self, r, rx, ry); });
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}{CGPoint=dd}");
    support.method(gradient, "drawInBezierPath:relativeCenterPosition:", [sp, clipped, radial_in](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], path = s.x[2];
        if (!path) return;
        const auto bounds = result_rect(sp->send(path, "bounds"));
        const auto rx = d(s, 0), ry = d(s, 1);
        clipped(path, std::nullopt, [&] { radial_in(self, bounds, rx, ry); });
    }, "v@:@{CGPoint=dd}");

    // ---- NSAffineTransform ------------------------------------------------------------------------------
    const auto transform = support.declare_class("NSAffineTransform", "NSObject", foundation_library);
    support.objc.add_native_protocol(transform, "NSCopying");
    Props(support, transform).lifecycle();
    support.method(transform, "transform", [sp](CpuState& s, GuestMemory&) {
        s.x[0] = sp->send_id(sp->send_id(sp->send_id(s.x[0], "alloc"), "init"), "autorelease");
    }, "@@:", true);
    support.method(transform, "init", [sp](CpuState& s, GuestMemory&) { set_matrix(*sp, s.x[0], {}); }, "@@:");
    support.method(transform, "initWithTransform:", [sp](CpuState& s, GuestMemory&) {
        set_matrix(*sp, s.x[0], s.x[2] ? matrix_of(*sp, s.x[2]) : Affine{});
    }, "@@:@");
    const auto update = [sp](const char* name, std::function<Affine(const Affine&, const CpuState&)> change, const char* types) {
        sp->method(sp->objc.find_class("NSAffineTransform"), name, [sp, change](CpuState& s, GuestMemory&) {
            set_matrix(*sp, s.x[0], change(matrix_of(*sp, s.x[0]), s));
        }, types);
    };
    update("translateXBy:yBy:", [](const Affine& t, const CpuState& s) { return affine_translate(t, d(s, 0), d(s, 1)); }, "v@:dd");
    update("rotateByRadians:", [](const Affine& t, const CpuState& s) { return affine_rotate(t, d(s, 0)); }, "v@:d");
    update("rotateByDegrees:", [](const Affine& t, const CpuState& s) { return affine_rotate(t, d(s, 0) * std::numbers::pi / 180); }, "v@:d");
    update("scaleBy:", [](const Affine& t, const CpuState& s) { return affine_scale(t, d(s, 0), d(s, 0)); }, "v@:d");
    update("scaleXBy:yBy:", [](const Affine& t, const CpuState& s) { return affine_scale(t, d(s, 0), d(s, 1)); }, "v@:dd");
    support.method(transform, "invert", [sp](CpuState& s, GuestMemory&) {
        const auto inverse = affine_invert(matrix_of(*sp, s.x[0]));
        if (!inverse) fail("-[NSAffineTransform invert] on a singular matrix");
        set_matrix(*sp, s.x[0], *inverse);
    }, "v@:");
    support.method(transform, "appendTransform:", [sp](CpuState& s, GuestMemory&) {
        if (!s.x[2]) fail("-[NSAffineTransform appendTransform:] with nil");
        set_matrix(*sp, s.x[0], affine_multiply(matrix_of(*sp, s.x[0]), matrix_of(*sp, s.x[2])));
    }, "v@:@");
    support.method(transform, "prependTransform:", [sp](CpuState& s, GuestMemory&) {
        if (!s.x[2]) fail("-[NSAffineTransform prependTransform:] with nil");
        set_matrix(*sp, s.x[0], affine_multiply(matrix_of(*sp, s.x[2]), matrix_of(*sp, s.x[0])));
    }, "v@:@");
    support.method(transform, "transformPoint:", [sp](CpuState& s, GuestMemory&) {
        const auto p = affine_point(matrix_of(*sp, s.x[0]), d(s, 0), d(s, 1));
        put_d(s, 0, p[0]); put_d(s, 1, p[1]);
    }, "{CGPoint=dd}@:{CGPoint=dd}");
    support.method(transform, "transformSize:", [sp](CpuState& s, GuestMemory&) {
        const auto p = affine_size(matrix_of(*sp, s.x[0]), d(s, 0), d(s, 1));
        put_d(s, 0, p[0]); put_d(s, 1, p[1]);
    }, "{CGSize=dd}@:{CGSize=dd}");
    support.method(transform, "transformStruct", [sp](CpuState& s, GuestMemory& m) { write_struct(m, s.x[8], matrix_of(*sp, s.x[0])); },
                   "{_NSAffineTransformStruct=dddddd}@:");
    support.method(transform, "setTransformStruct:", [sp](CpuState& s, GuestMemory& m) { set_matrix(*sp, s.x[0], read_struct(m, s.x[2])); },
                   "v@:{_NSAffineTransformStruct=dddddd}");
    support.method(transform, "transformBezierPath:", [sp](CpuState& s, GuestMemory&) {
        if (!s.x[2]) { s.x[0] = 0; return; }
        const auto copy = sp->send_id(s.x[2], "copy");
        sp->send_id(copy, "transformUsingAffineTransform:", {s.x[0]});
        s.x[0] = sp->send_id(copy, "autorelease");
    }, "@@:@");
    support.method(transform, "concat", [sp, drawing](CpuState& s, GuestMemory&) {
        const auto context = drawing->cg("-[NSAffineTransform concat]");
        if (!context) return;
        const auto buffer = sp->process.allocate_guest(48);
        write_struct(sp->memory(), buffer, matrix_of(*sp, s.x[0]));
        drawing->cg_id("_CGContextConcatCTM", {context, buffer});
        sp->process.release_guest(buffer);
    }, "v@:");
    support.method(transform, "isEqual:", [sp](CpuState& s, GuestMemory&) {
        if (s.x[0] == s.x[2]) { s.x[0] = 1; return; }
        if (!s.x[2] || !sp->objc.find_host_state<PropertyBag>(s.x[2])) { s.x[0] = 0; return; }
        const auto a = matrix_of(*sp, s.x[0]), b = matrix_of(*sp, s.x[2]);
        put_bool(s, a.m11 == b.m11 && a.m12 == b.m12 && a.m21 == b.m21 && a.m22 == b.m22 && a.tx == b.tx && a.ty == b.ty);
    }, "B@:@");

    // ---- NSColor additions --------------------------------------------------------------------------------
    const auto color_class = support.objc.find_class("NSColor");
    if (!color_class) fail("NSColor is missing");
    for (const auto* name : {"colorWithCalibratedHue:saturation:brightness:alpha:", "colorWithDeviceHue:saturation:brightness:alpha:",
                             "colorWithHue:saturation:brightness:alpha:"})
        support.method(color_class, name, [drawing](CpuState& s, GuestMemory&) {
            const auto rgb = hsb_to_rgb(d(s, 0), d(s, 1), d(s, 2));
            s.x[0] = drawing->color(rgb[0], rgb[1], rgb[2], d(s, 3));
        }, "@@:dddd", true);
    support.method(color_class, "getHue:saturation:brightness:alpha:", [drawing](CpuState& s, GuestMemory& m) {
        const auto c = drawing->rgba(s.x[0]);
        const auto hsb = rgb_to_hsb(c.r, c.g, c.b);
        const std::array<double, 4> values{hsb[0], hsb[1], hsb[2], c.a};
        for (unsigned i = 0; i < 4; ++i)
            if (s.x[2 + i]) m.write64(s.x[2 + i], std::bit_cast<std::uint64_t>(values[i]));
    }, "v@:^d^d^d^d");
    for (unsigned index = 0; index < 3; ++index) {
        static constexpr const char* names[] = {"hueComponent", "saturationComponent", "brightnessComponent"};
        support.method(color_class, names[index], [drawing, index](CpuState& s, GuestMemory&) {
            const auto c = drawing->rgba(s.x[0]);
            put_d(s, 0, rgb_to_hsb(c.r, c.g, c.b)[index]);
        }, "d@:");
    }
    support.method(color_class, "colorWithCIColor:", [sp, drawing](CpuState& s, GuestMemory&) {
        if (!s.x[2]) fail("+[NSColor colorWithCIColor:] with nil");
        const auto component = [&](const char* name) { return sp->send(s.x[2], name).d(0); };
        s.x[0] = drawing->color(component("red"), component("green"), component("blue"), component("alpha"));
    }, "@@:@", true);

    // NSCoding of colors in keyed archives (documents, user defaults), in
    // AppKit's keys: NSColorSpace 1/2 (calibrated / device RGB, "NSRGB"),
    // 3/4 (white, "NSWhite"), 5 (CMYK, "NSCMYK"), 6 (catalog: NSCatalogName,
    // NSColorName, fallback "NSColor"), 7 (component-based: "NSComponents",
    // or the "NSRGB" / "NSWhite" fallback archived next to them) and 10
    // (pattern, "NSImage"). Colors are archived as NSColorSpace 1 with their
    // sRGB components.
    support.method(color_class, "supportsSecureCoding", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:", true);
    support.method(color_class, "initWithCoder:", [sp, drawing, key](CpuState& s, GuestMemory& m) {
        const auto self = s.x[0], coder = s.x[2];
        // Coders without -containsValueForKey: answer zero / nil for absent keys.
        const bool can_ask = sp->objc.responds(coder, sp->objc.selector("containsValueForKey:"));
        const auto has = [&](const char* name) { return !can_ask || (sp->send_id(coder, "containsValueForKey:", {key(name)}) & 0xff) != 0; };
        const auto numbers = [&](const char* name) {
            std::vector<double> values;
            if (!has(name)) return values;
            const auto length_slot = sp->process.allocate_guest(8);
            m.write64(length_slot, 0);
            const auto bytes = sp->send_id(coder, "decodeBytesForKey:returnedLength:", {key(name), length_slot});
            const auto length = m.read64(length_slot);
            sp->process.release_guest(length_slot);
            if (!bytes || !length || length > 4096) return values;
            std::vector<std::uint8_t> raw(length);
            m.read_bytes(bytes, raw);
            std::string text(raw.begin(), raw.end());
            while (!text.empty() && text.back() == '\0') text.pop_back();
            return parse_vector_string(text).value_or(std::vector<double>{});
        };
        auto space = has("NSColorSpace") ? static_cast<std::int32_t>(sp->send_id(coder, "decodeIntForKey:", {key("NSColorSpace")})) : 0;
        if (space == 0) space = 7;   // no space recorded: whichever components are present
        const auto color_class_object = sp->objc.find_class("NSColor");
        Address result{};
        const auto rgb = [&](const std::vector<double>& c) { return drawing->color(c[0], c[1], c[2], c.size() > 3 ? c[3] : 1.0); };
        const auto white = [&](const std::vector<double>& c) { return drawing->color(c[0], c[0], c[0], c.size() > 1 ? c[1] : 1.0); };
        switch (space) {
        case 1: case 2: if (const auto c = numbers("NSRGB"); c.size() >= 3) result = rgb(c); break;
        case 3: case 4: if (const auto c = numbers("NSWhite"); !c.empty()) result = white(c); break;
        case 5:
            if (const auto c = numbers("NSCMYK"); c.size() >= 4)
                result = drawing->color((1 - c[0]) * (1 - c[3]), (1 - c[1]) * (1 - c[3]), (1 - c[2]) * (1 - c[3]), c.size() > 4 ? c[4] : 1.0);
            break;
        case 6: {
            const auto catalog = sp->send_id(coder, "decodeObjectForKey:", {key("NSCatalogName")});
            const auto name = sp->send_id(coder, "decodeObjectForKey:", {key("NSColorName")});
            if (catalog && name && sp->objc.responds(color_class_object, sp->objc.selector("colorWithCatalogName:colorName:")))
                result = sp->send_id(color_class_object, "colorWithCatalogName:colorName:", {catalog, name});
            if (!result) result = sp->send_id(coder, "decodeObjectForKey:", {key("NSColor")});
            break;
        }
        case 7: {
            if (const auto c = numbers("NSRGB"); c.size() >= 3) { result = rgb(c); break; }
            if (const auto c = numbers("NSWhite"); !c.empty()) { result = white(c); break; }
            const auto c = numbers("NSComponents");
            if (c.size() >= 4) result = rgb(c);
            else if (c.size() == 2) result = white(c);
            break;
        }
        case 10: {
            const auto image = sp->send_id(coder, "decodeObjectForKey:", {key("NSImage")});
            if (image) result = sp->send_id(color_class_object, "colorWithPatternImage:", {image});
            break;
        }
        default: break;
        }
        if (!result) unsupported("NSColor keyed archive in color space " + std::to_string(space));
        sp->release(self);
        s.x[0] = sp->retain(result);
    }, "@@:@");
    support.method(color_class, "encodeWithCoder:", [sp, drawing, key](CpuState& s, GuestMemory&) {
        const auto c = drawing->rgba(s.x[0]);
        char text[160];
        const auto length = std::snprintf(text, sizeof text, "%.9g %.9g %.9g %.9g", c.r, c.g, c.b, c.a) + 1;   // with its NUL, as AppKit
        CallRegs space;
        space.x[2] = 1;
        space.x[3] = key("NSColorSpace");
        sp->send(s.x[2], "encodeInt:forKey:", space);
        const auto bytes = sp->process.allocate_guest(static_cast<std::uint64_t>(length));
        sp->memory().write_bytes(bytes, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text), static_cast<std::size_t>(length)));
        sp->send_id(s.x[2], "encodeBytes:length:forKey:", {bytes, static_cast<Address>(length), key("NSRGB")});
        sp->process.release_guest(bytes);
    }, "v@:@");

    // Pattern colors.
    auto patterns = std::make_shared<PatternColors>();
    const auto pattern_class = support.declare_class("NSPatternColor", "NSColor");
    // CGPatternCallbacks.drawPattern(info = the NSPatternColor, context): the
    // image at its point size, origin at the pattern cell's origin.
    patterns->draw_callback = support.process.install_function("_alchemy_ns_pattern_color_draw", [sp, patterns](CpuState& s, GuestMemory&) {
        Address image{};
        {
            std::lock_guard lock(patterns->mutex);
            if (const auto it = patterns->colors.find(s.x[0]); it != patterns->colors.end()) image = it->second.image;
        }
        if (!image) return;
        const auto size = sp->send(image, "size");
        const auto cg_image = sp->send_id(image, "CGImageForProposedRect:context:hints:", {0, 0, 0});
        if (!cg_image) return;
        CallRegs registers;
        registers.x[0] = s.x[1];
        registers.x[1] = cg_image;
        registers.set_d(0, 0); registers.set_d(1, 0); registers.set_d(2, size.d(0)); registers.set_d(3, size.d(1));
        static_cast<void>(sp->call(coregraphics_library, "_CGContextDrawImage", registers));
    });
    const auto callbacks = support.process.allocate_guest(24);
    support.memory().write64(callbacks, 0);
    support.memory().write64(callbacks + 8, patterns->draw_callback);
    support.memory().write64(callbacks + 16, 0);
    support.method(color_class, "colorWithPatternImage:", [sp, patterns, pattern_class](CpuState& s, GuestMemory&) {
        if (!s.x[2]) fail("+[NSColor colorWithPatternImage:] with nil");
        const auto color = sp->objc.allocate(pattern_class);
        {
            std::lock_guard lock(patterns->mutex);
            patterns->colors[color] = {sp->retain(s.x[2]), 0};
        }
        s.x[0] = sp->send_id(color, "autorelease");
    }, "@@:@", true);
    support.method(pattern_class, "patternImage", [patterns](CpuState& s, GuestMemory&) {
        std::lock_guard lock(patterns->mutex);
        const auto it = patterns->colors.find(s.x[0]);
        s.x[0] = it == patterns->colors.end() ? 0 : it->second.image;
    }, "@@:");
    support.method(pattern_class, "colorSpaceName", [key](CpuState& s, GuestMemory&) { s.x[0] = key("NSPatternColorSpace"); }, "@@:");
    support.method(pattern_class, "dealloc", [sp, patterns, pattern_class](CpuState& s, GuestMemory&) {
        PatternColors::Entry entry;
        {
            std::lock_guard lock(patterns->mutex);
            if (const auto it = patterns->colors.find(s.x[0]); it != patterns->colors.end()) { entry = it->second; patterns->colors.erase(it); }
        }
        sp->release(entry.image);
        if (entry.pattern) static_cast<void>(sp->call(coregraphics_library, "_CGPatternRelease", {entry.pattern}));
        static_cast<void>(sp->objc.send_super(s.x[0], pattern_class, sp->objc.selector("dealloc")));
    }, "v@:");
    // The color's CGPattern (made once): a colored pattern whose cell is the image.
    const auto pattern_of = [sp, patterns, callbacks](Address color) -> Address {
        Address image{};
        {
            std::lock_guard lock(patterns->mutex);
            const auto it = patterns->colors.find(color);
            if (it == patterns->colors.end()) fail("NSPatternColor without an image");
            if (it->second.pattern) return it->second.pattern;
            image = it->second.image;
        }
        const auto size = sp->send(image, "size");
        const auto matrix = sp->process.allocate_guest(48);
        const std::array<double, 6> identity{1, 0, 0, 1, 0, 0};
        for (unsigned i = 0; i < 6; ++i) sp->memory().write64(matrix + i * 8, std::bit_cast<std::uint64_t>(identity[i]));
        CallRegs registers;
        registers.x[0] = color;
        registers.set_d(0, 0); registers.set_d(1, 0); registers.set_d(2, size.d(0)); registers.set_d(3, size.d(1));
        registers.x[1] = matrix;
        registers.set_d(4, size.d(0)); registers.set_d(5, size.d(1));
        registers.x[2] = 1;   // kCGPatternTilingConstantSpacing
        registers.x[3] = 1;   // colored
        registers.x[4] = callbacks;
        const auto pattern = sp->call(coregraphics_library, "_CGPatternCreate", registers).x0;
        sp->process.release_guest(matrix);
        std::lock_guard lock(patterns->mutex);
        patterns->colors[color].pattern = pattern;
        return pattern;
    };
    const auto set_pattern = [sp, drawing, pattern_of](Address color, bool fill, bool stroke) {
        const auto context = drawing->cg("-[NSPatternColor set]");
        if (!context) return;
        const auto pattern = pattern_of(color);
        const auto space = sp->call(coregraphics_library, "_CGColorSpaceCreatePattern", {0});
        const auto alpha = sp->process.allocate_guest(8);
        sp->memory().write64(alpha, std::bit_cast<std::uint64_t>(1.0));
        if (fill) {
            sp->call(coregraphics_library, "_CGContextSetFillColorSpace", {context, space});
            sp->call(coregraphics_library, "_CGContextSetFillPattern", {context, pattern, alpha});
        }
        if (stroke) {
            sp->call(coregraphics_library, "_CGContextSetStrokeColorSpace", {context, space});
            sp->call(coregraphics_library, "_CGContextSetStrokePattern", {context, pattern, alpha});
        }
        sp->process.release_guest(alpha);
        sp->call(coregraphics_library, "_CGColorSpaceRelease", {space});
    };
    support.method(pattern_class, "set", [set_pattern](CpuState& s, GuestMemory&) { set_pattern(s.x[0], true, true); }, "v@:");
    support.method(pattern_class, "setFill", [set_pattern](CpuState& s, GuestMemory&) { set_pattern(s.x[0], true, false); }, "v@:");
    support.method(pattern_class, "setStroke", [set_pattern](CpuState& s, GuestMemory&) { set_pattern(s.x[0], false, true); }, "v@:");
    support.method(pattern_class, "CGColor", [sp, pattern_of](CpuState& s, GuestMemory&) {
        const auto space = sp->call(coregraphics_library, "_CGColorSpaceCreatePattern", {0});
        const auto alpha = sp->process.allocate_guest(8);
        sp->memory().write64(alpha, std::bit_cast<std::uint64_t>(1.0));
        const auto color = sp->call(coregraphics_library, "_CGColorCreateWithPattern", {space, pattern_of(s.x[0]), alpha});
        sp->process.release_guest(alpha);
        sp->call(coregraphics_library, "_CGColorSpaceRelease", {space});
        s.x[0] = sp->send_id(color, "autorelease");
    }, "^v@:");
    for (const auto* name : {"redComponent", "greenComponent", "blueComponent", "whiteComponent", "getRed:green:blue:alpha:"})
        support.method(pattern_class, name, [name](CpuState&, GuestMemory&) {
            fail(std::string("NSInvalidArgumentException: -") + name + " is not valid for a pattern color (NSPatternColorSpace)");
        }, "d@:");
    support.method(pattern_class, "alphaComponent", [](CpuState& s, GuestMemory&) { put_d(s, 0, 1); }, "d@:");
    support.method(pattern_class, "copyWithZone:", [sp](CpuState& s, GuestMemory&) { s.x[0] = sp->retain(s.x[0]); }, "@@:^v");
    support.method(pattern_class, "encodeWithCoder:", [sp, key](CpuState& s, GuestMemory&) {
        CallRegs space;
        space.x[2] = 10;
        space.x[3] = key("NSColorSpace");
        sp->send(s.x[2], "encodeInt:forKey:", space);
        sp->send_id(s.x[2], "encodeObject:forKey:", {sp->send_id(s.x[0], "patternImage"), key("NSImage")});
    }, "v@:@");

    // ---- NSImageSymbolConfiguration and symbol images -------------------------------------------------
    const auto symbol_configuration = support.declare_class("NSImageSymbolConfiguration", "NSObject", appkit);
    support.objc.add_native_protocol(symbol_configuration, "NSCopying");
    Props(support, symbol_configuration).lifecycle();
    const auto configuration = [sp, symbol_configuration](std::initializer_list<std::pair<const char*, std::uint64_t>> scalars,
                                                          std::initializer_list<std::pair<const char*, Address>> objects) {
        const auto made = sp->send_id(sp->send_id(symbol_configuration, "alloc"), "init");
        for (const auto& [name, value] : scalars) bag_set_raw(*sp, made, name, {value, 0, 0, 0});
        for (const auto& [name, value] : objects) bag_set_object(*sp, made, name, value, true);
        return sp->send_id(made, "autorelease");
    };
    support.method(symbol_configuration, "configurationWithScale:", [configuration](CpuState& s, GuestMemory&) {
        s.x[0] = configuration({{"scale", s.x[2]}}, {});
    }, "@@:q", true);
    support.method(symbol_configuration, "configurationWithPointSize:weight:", [configuration](CpuState& s, GuestMemory&) {
        s.x[0] = configuration({{"pointSize", s.v[0][0]}, {"weight", s.v[1][0]}}, {});
    }, "@@:dd", true);
    support.method(symbol_configuration, "configurationWithPointSize:weight:scale:", [configuration](CpuState& s, GuestMemory&) {
        s.x[0] = configuration({{"pointSize", s.v[0][0]}, {"weight", s.v[1][0]}, {"scale", s.x[2]}}, {});
    }, "@@:ddq", true);
    support.method(symbol_configuration, "configurationWithTextStyle:", [configuration](CpuState& s, GuestMemory&) {
        s.x[0] = configuration({}, {{"textStyle", s.x[2]}});
    }, "@@:@", true);
    support.method(symbol_configuration, "configurationWithTextStyle:scale:", [configuration](CpuState& s, GuestMemory&) {
        s.x[0] = configuration({{"scale", s.x[3]}}, {{"textStyle", s.x[2]}});
    }, "@@:@q", true);
    support.method(symbol_configuration, "configurationWithHierarchicalColor:", [configuration](CpuState& s, GuestMemory&) {
        s.x[0] = configuration({}, {{"hierarchicalColor", s.x[2]}});
    }, "@@:@", true);
    support.method(symbol_configuration, "configurationWithPaletteColors:", [configuration](CpuState& s, GuestMemory&) {
        s.x[0] = configuration({}, {{"paletteColors", s.x[2]}});
    }, "@@:@", true);
    support.method(symbol_configuration, "configurationPreferringMulticolor", [configuration](CpuState& s, GuestMemory&) {
        s.x[0] = configuration({{"prefersMulticolor", 1}}, {});
    }, "@@:", true);
    // The receiver's values overridden by the other configuration's.
    support.method(symbol_configuration, "configurationByApplyingConfiguration:", [sp](CpuState& s, GuestMemory&) {
        const auto made = sp->send_id(s.x[0], "copy");
        if (s.x[2]) {
            const auto from = bag(*sp, s.x[2]);
            std::map<std::string, std::array<std::uint64_t, 4>> scalars;
            std::map<std::string, Address> objects;
            {
                std::lock_guard lock(from->mutex);
                scalars = from->scalars;
                objects = from->objects;
            }
            for (const auto& [name, value] : scalars) bag_set_raw(*sp, made, name, value);
            for (const auto& [name, value] : objects) bag_set_object(*sp, made, name, value);
        }
        s.x[0] = sp->send_id(made, "autorelease");
    }, "@@:@");

    const auto image_class = support.objc.find_class("NSImage");
    if (!image_class) fail("NSImage is missing");
    auto reported = std::make_shared<std::pair<std::mutex, std::set<std::string>>>();
    support.method(image_class, "imageWithSystemSymbolName:accessibilityDescription:", [sp, reported](CpuState& s, GuestMemory&) {
        const auto name = s.x[2] ? sp->cf.utf8(s.x[2]) : std::string();
        {
            std::lock_guard lock(reported->first);
            if (reported->second.insert(name).second)
                std::fprintf(stderr, "[alchemy completion] no SF Symbols artwork exists, so +[NSImage imageWithSystemSymbolName:\"%s\" "
                                     "accessibilityDescription:] answers nil (an unknown symbol name)\n", name.c_str());
        }
        s.x[0] = 0;
    }, "@@:@@", true);
    // Only symbol images take a symbol configuration; others answer themselves.
    support.method(image_class, "imageWithSymbolConfiguration:", [sp](CpuState& s, GuestMemory&) {
        s.x[0] = sp->send_id(sp->retain(s.x[0]), "autorelease");
    }, "@@:@");
    support.method(image_class, "symbolConfiguration", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "@@:");
}

} // namespace alchemy::gui::completion_kit::graphics
