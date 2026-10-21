#include "alchemy/darwin/shell_kit.hpp"
#include "alchemy/darwin/shells.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

// alchemy.exe only (darwin/shells.hpp): CoreGraphics' headless answers, from
// the framework identities (darwin/frameworks.cpp, darwin/headless_additions.cpp):
// named color spaces, CGColor values, geometry and the display queries (no
// display) are headless-correct; drawing and image codecs fail explicitly.
namespace alchemy::darwin_shells {
namespace {
constexpr std::string_view core_graphics = "/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics";
constexpr std::string_view image_io = "/System/Library/Frameworks/ImageIO.framework/Versions/A/ImageIO";

// CGColorSpace.h names a CGColorSpaceCreateWithName accepts.
constexpr std::string_view color_space_names[] = {"kCGColorSpaceSRGB", "kCGColorSpaceExtendedSRGB",
    "kCGColorSpaceLinearSRGB", "kCGColorSpaceExtendedLinearSRGB", "kCGColorSpaceGenericRGB",
    "kCGColorSpaceGenericRGBLinear", "kCGColorSpaceDisplayP3", "kCGColorSpaceGenericGrayGamma2_2",
    "kCGColorSpaceLinearGray", "kCGColorSpaceExtendedGray"};
// CGColorSpace.h CGColorSpaceModel
constexpr std::int32_t model_monochrome = 0, model_rgb = 1;

struct ColorSpace final : ObjectPayload { std::string name; };
struct Color final : ObjectPayload {
    Address space{};
    std::vector<double> components;
    Address storage{};
    ProcessContext* context{};
    ~Color() override {
        if (storage && context) context->release_guest(storage);
    }
};

struct Rect { double x, y, width, height; };
Rect rect_argument(const CpuState& s, unsigned first) {
    return {real(s, first), real(s, first + 1), real(s, first + 2), real(s, first + 3)};
}
void return_rect(CpuState& s, const Rect& value) {
    return_real(s, 0, value.x); return_real(s, 1, value.y); return_real(s, 2, value.width); return_real(s, 3, value.height);
}
bool null_rect(const Rect& value) { return std::isinf(value.x) || std::isinf(value.y); }
Rect standardized(Rect value) {
    if (value.width < 0) { value.x += value.width; value.width = -value.width; }
    if (value.height < 0) { value.y += value.height; value.height = -value.height; }
    return value;
}
using Transform = std::array<double, 6>;  // a, b, c, d, tx, ty
Transform read_transform(GuestMemory& m, Address address) {
    Transform value{};
    for (std::size_t index = 0; index < value.size(); ++index) value[index] = std::bit_cast<double>(m.read64(address + index * 8));
    return value;
}
void write_transform(GuestMemory& m, Address address, const Transform& value) {
    if (!address) throw std::runtime_error("Alchemy CoreGraphics: missing CGAffineTransform result address");
    for (std::size_t index = 0; index < value.size(); ++index) m.write64(address + index * 8, std::bit_cast<std::uint64_t>(value[index]));
}
Transform concat(const Transform& t1, const Transform& t2) {
    return {t1[0] * t2[0] + t1[1] * t2[2], t1[0] * t2[1] + t1[1] * t2[3],
            t1[2] * t2[0] + t1[3] * t2[2], t1[2] * t2[1] + t1[3] * t2[3],
            t1[4] * t2[0] + t1[5] * t2[2] + t2[4], t1[4] * t2[1] + t1[5] * t2[3] + t2[5]};
}

void install_color_spaces(ShellKit& kit) {
    auto& context = kit.context;
    auto& cf = kit.cf;
    auto color_space_class = kit.objc.find_class("_AlchemyCGColorSpace");
    if (!color_space_class) color_space_class = kit.objc.register_native_class("_AlchemyCGColorSpace", kit.objc.find_class("NSObject"), 16);
    for (const auto name : color_space_names) kit.string_symbol(core_graphics, "_" + std::string(name), name);
    kit.function(core_graphics, "_CGColorSpaceCreateWithName", [&context, &cf, color_space_class](CpuState& s, GuestMemory&) {
        if (!s.x[0]) { s.x[0] = 0; return; }
        auto name = cf.utf8(s.x[0]);
        if (std::find(std::begin(color_space_names), std::end(color_space_names), name) == std::end(color_space_names)) { s.x[0] = 0; return; }
        auto payload = std::make_shared<ColorSpace>();
        payload->name = std::move(name);
        s.x[0] = context.objects().create(16, color_space_class, ObjectKind::instance, std::move(payload));
    });
    const auto space = [&context](Address object) {
        const auto record = context.objects().require(object);
        const auto value = std::dynamic_pointer_cast<ColorSpace>(record->payload);
        if (!value) throw std::runtime_error("Alchemy CoreGraphics: object is not a CGColorSpace");
        return value;
    };
    kit.function(core_graphics, "_CGColorSpaceRetain", [&context, space](CpuState& s, GuestMemory&) {
        if (s.x[0]) { static_cast<void>(space(s.x[0])); s.x[0] = context.objects().retain(s.x[0]); }
    });
    kit.function(core_graphics, "_CGColorSpaceRelease", [&context, space](CpuState& s, GuestMemory&) {
        if (s.x[0]) { static_cast<void>(space(s.x[0])); context.objects().release(s.x[0]); }
        s.x[0] = 0;
    });
    kit.function(core_graphics, "_CGColorSpaceCopyName", [&cf, space](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[0] ? cf.string(space(s.x[0])->name) : 0;
    });
}

void install_geometry(ShellKit& kit) {
    const std::uint8_t zero_rect[32]{};
    kit.data_symbol(core_graphics, "_CGRectZero", zero_rect);
    kit.data_symbol(core_graphics, "_CGPointZero", std::span(zero_rect, 16));
    Transform identity{1, 0, 0, 1, 0, 0};
    std::array<std::uint8_t, 48> identity_bytes{};
    std::memcpy(identity_bytes.data(), identity.data(), 48);
    kit.data_symbol(core_graphics, "_CGAffineTransformIdentity", identity_bytes);
    kit.function(core_graphics, "_CGRectIsEmpty", [](CpuState& s, GuestMemory&) {
        const auto value = rect_argument(s, 0);
        s.x[0] = null_rect(value) || value.width == 0 || value.height == 0;
    });
    kit.function(core_graphics, "_CGRectEqualToRect", [](CpuState& s, GuestMemory&) {
        const auto left = standardized(rect_argument(s, 0)), right = standardized(rect_argument(s, 4));
        s.x[0] = left.x == right.x && left.y == right.y && left.width == right.width && left.height == right.height;
    });
    kit.function(core_graphics, "_CGRectOffset", [](CpuState& s, GuestMemory&) {
        auto value = rect_argument(s, 0);
        if (!null_rect(value)) { value = standardized(value); value.x += real(s, 4); value.y += real(s, 5); }
        return_rect(s, value);
    });
    kit.function(core_graphics, "_CGAffineTransformMakeScale", [](CpuState& s, GuestMemory& m) {
        write_transform(m, s.x[8], {real(s, 0), 0, 0, real(s, 1), 0, 0});
    });
    kit.function(core_graphics, "_CGAffineTransformMakeTranslation", [](CpuState& s, GuestMemory& m) {
        write_transform(m, s.x[8], {1, 0, 0, 1, real(s, 0), real(s, 1)});
    });
    kit.function(core_graphics, "_CGAffineTransformConcat", [](CpuState& s, GuestMemory& m) {
        write_transform(m, s.x[8], concat(read_transform(m, s.x[0]), read_transform(m, s.x[1])));
    });
    kit.function(core_graphics, "_CGAffineTransformEqualToTransform", [](CpuState& s, GuestMemory& m) {
        s.x[0] = read_transform(m, s.x[0]) == read_transform(m, s.x[1]);
    });
    kit.function(core_graphics, "_CGAffineTransformInvert", [](CpuState& s, GuestMemory& m) {
        const auto t = read_transform(m, s.x[0]);
        const auto determinant = t[0] * t[3] - t[1] * t[2];
        if (determinant == 0) { write_transform(m, s.x[8], t); return; } // singular: returned unchanged
        write_transform(m, s.x[8], {t[3] / determinant, -t[1] / determinant, -t[2] / determinant, t[0] / determinant,
                                    (t[2] * t[5] - t[3] * t[4]) / determinant, (t[1] * t[4] - t[0] * t[5]) / determinant});
    });
    kit.function(core_graphics, "_CGAffineTransformRotate", [](CpuState& s, GuestMemory& m) {
        const auto angle = real(s, 0);
        write_transform(m, s.x[8], concat({std::cos(angle), std::sin(angle), -std::sin(angle), std::cos(angle), 0, 0},
                                          read_transform(m, s.x[0])));
    });
    kit.function(core_graphics, "_CGAffineTransformScale", [](CpuState& s, GuestMemory& m) {
        write_transform(m, s.x[8], concat({real(s, 0), 0, 0, real(s, 1), 0, 0}, read_transform(m, s.x[0])));
    });
    kit.function(core_graphics, "_CGAffineTransformTranslate", [](CpuState& s, GuestMemory& m) {
        write_transform(m, s.x[8], concat({1, 0, 0, 1, real(s, 0), real(s, 1)}, read_transform(m, s.x[0])));
    });
}

void install_color_values(ShellKit& kit) {
    auto& context = kit.context;
    auto& cf = kit.cf;
    // Components per color of `space` without alpha: the color spaces above
    // are named after CGColorSpace.h constants.
    const auto space_components = [&context, &cf, kit_copy = kit](Address space) {
        const auto name = kit_copy.guest_call(core_graphics, "_CGColorSpaceCopyName", {space});
        if (!name) throw std::runtime_error("Alchemy CoreGraphics: color space without a name");
        const auto text = cf.utf8(name);
        context.objects().release(name);
        return text.find("Gray") != std::string::npos ? 1U : 3U;
    };
    kit.function(core_graphics, "_CGColorSpaceCreateDeviceGray", [&context, &cf, kit_copy = kit](CpuState& s, GuestMemory&) {
        const auto name = cf.string("kCGColorSpaceGenericGrayGamma2_2");
        s.x[0] = kit_copy.guest_call(core_graphics, "_CGColorSpaceCreateWithName", {name});
        context.objects().release(name);
    });
    kit.function(core_graphics, "_CGColorSpaceGetModel", [space_components](CpuState& s, GuestMemory&) {
        s.x[0] = static_cast<std::uint32_t>(s.x[0] ? (space_components(s.x[0]) == 1 ? model_monochrome : model_rgb) : -1);
    });

    auto color_class = kit.objc.find_class("_AlchemyCGColor");
    if (!color_class) color_class = kit.objc.register_native_class("_AlchemyCGColor", kit.objc.find_class("NSObject"), 16);
    const auto color = [&context](Address object) {
        const auto record = context.objects().require(object);
        auto value = std::dynamic_pointer_cast<Color>(record->payload);
        if (!value) throw std::runtime_error("Alchemy CoreGraphics: object is not a CGColor");
        return value;
    };
    kit.function(core_graphics, "_CGColorCreate", [&context, kit_copy = kit, space_components, color_class](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[1]) { s.x[0] = 0; return; }
        const auto count = space_components(s.x[0]) + 1;
        auto payload = std::make_shared<Color>();
        payload->context = &context;
        payload->space = kit_copy.guest_call(core_graphics, "_CGColorSpaceRetain", {s.x[0]});
        payload->storage = context.allocate_guest(count * 8);
        for (std::uint32_t index = 0; index < count; ++index) {
            const auto value = m.read64(s.x[1] + index * 8ULL);
            payload->components.push_back(std::bit_cast<double>(value));
            m.write64(payload->storage + index * 8ULL, value);
        }
        s.x[0] = context.objects().create(16, color_class, ObjectKind::instance, std::move(payload));
    });
    kit.function(core_graphics, "_CGColorRetain", [&context, color](CpuState& s, GuestMemory&) {
        if (s.x[0]) { static_cast<void>(color(s.x[0])); s.x[0] = context.objects().retain(s.x[0]); }
    });
    kit.function(core_graphics, "_CGColorRelease", [&context, color](CpuState& s, GuestMemory&) {
        if (s.x[0]) { static_cast<void>(color(s.x[0])); context.objects().release(s.x[0]); }
        s.x[0] = 0;
    });
    kit.function(core_graphics, "_CGColorGetNumberOfComponents", [color](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[0] ? color(s.x[0])->components.size() : 0;
    });
    kit.function(core_graphics, "_CGColorGetComponents", [color](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[0] ? color(s.x[0])->storage : 0;
    });
    kit.function(core_graphics, "_CGColorGetColorSpace", [color](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[0] ? color(s.x[0])->space : 0;
    });
    kit.function(core_graphics, "_CGColorEqualToColor", [color](CpuState& s, GuestMemory&) {
        if (s.x[0] == s.x[1]) { s.x[0] = 1; return; }
        if (!s.x[0] || !s.x[1]) { s.x[0] = 0; return; }
        const auto left = color(s.x[0]), right = color(s.x[1]);
        s.x[0] = left->components == right->components && left->space == right->space;
    });
}

// Quartz Display Services with no display (alchemy-fx: displays.cpp, over the
// window server's screens): the lists are empty, an unknown display answers
// zero values, no display mode exists, and nothing is ever reconfigured.
void install_displays(ShellKit& kit) {
    constexpr std::int32_t cg_success = 0, cg_illegal_argument = 1001;   // CGError.h
    const auto empty_list = [](CpuState& s, GuestMemory& m) {
        // (maxDisplays, displays, count)
        if (!s.x[2]) { s.x[0] = status(cg_illegal_argument); return; }
        m.write32(s.x[2], 0);
        s.x[0] = status(cg_success);
    };
    for (const auto* name : {"_CGGetActiveDisplayList", "_CGGetOnlineDisplayList", "_CGGetDisplaysWithPoint", "_CGGetDisplaysWithRect"})
        kit.function(core_graphics, name, empty_list);
    // kCGNullDirectDisplay, false, no mirroring, no mode, no image.
    for (const auto* name : {"_CGMainDisplayID", "_CGDisplayPixelsWide", "_CGDisplayPixelsHigh", "_CGDisplayIsMain",
             "_CGDisplayIsActive", "_CGDisplayIsOnline", "_CGDisplayIsBuiltin", "_CGDisplayIsAsleep", "_CGDisplayMirrorsDisplay",
             "_CGDisplayIDToOpenGLDisplayMask", "_CGDisplayCopyDisplayMode", "_CGDisplayCopyAllDisplayModes",
             "_CGDisplayCreateImage", "_CGDisplayCreateImageForRect"})
        kit.function(core_graphics, name, [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    kit.function(core_graphics, "_CGDisplayBounds", [](CpuState& s, GuestMemory&) { return_rect(s, {0, 0, 0, 0}); });
    kit.function(core_graphics, "_CGDisplayScreenSize", [](CpuState& s, GuestMemory&) { return_real(s, 0, 0); return_real(s, 1, 0); });
    // A display mode is never made, so only NULL can be passed (it answers 0, as on macOS).
    const auto no_mode = [](const CpuState& s, std::string_view function) {
        if (s.x[0]) throw std::runtime_error("Alchemy CoreGraphics: " + std::string(function) +
                                             ": the argument is not a CGDisplayModeRef (no display exists)");
    };
    for (const auto* name : {"_CGDisplayModeGetWidth", "_CGDisplayModeGetHeight", "_CGDisplayModeGetPixelWidth",
             "_CGDisplayModeGetPixelHeight", "_CGDisplayModeCopyPixelEncoding", "_CGDisplayModeRetain", "_CGDisplayModeRelease"})
        kit.function(core_graphics, name, [no_mode, name](CpuState& s, GuestMemory&) { no_mode(s, name + 1); s.x[0] = 0; });
    kit.function(core_graphics, "_CGDisplayModeGetRefreshRate", [no_mode](CpuState& s, GuestMemory&) {
        no_mode(s, "CGDisplayModeGetRefreshRate");
        return_real(s, 0, 0);
    });
    kit.function(core_graphics, "_CGDisplaySetDisplayMode", [](CpuState& s, GuestMemory&) { s.x[0] = status(cg_illegal_argument); });
    kit.function(core_graphics, "_CGRestorePermanentDisplayConfiguration", [](CpuState&, GuestMemory&) {});
    // Registrations are accepted; with no display, none is ever reported.
    kit.function(core_graphics, "_CGDisplayRegisterReconfigurationCallback", [](CpuState& s, GuestMemory&) {
        s.x[0] = status(s.x[0] ? cg_success : cg_illegal_argument);
    });
    kit.function(core_graphics, "_CGDisplayRemoveReconfigurationCallback", [](CpuState& s, GuestMemory&) { s.x[0] = status(cg_success); });
}
} // namespace

void install_coregraphics_shell(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf) {
    ShellKit kit{context, objc, cf};
    install_color_spaces(kit);
    install_geometry(kit);
    install_color_values(kit);
    install_displays(kit);
    kit.unsupported(core_graphics, {"_CGBitmapContextCreate", "_CGBitmapContextCreateImage", "_CGBitmapContextGetAlphaInfo",
        "_CGBitmapContextGetBitsPerPixel", "_CGBitmapContextGetBytesPerRow", "_CGBitmapContextGetData",
        "_CGBitmapContextGetHeight", "_CGBitmapContextGetWidth", "_CGColorCreateCopyByMatchingToColorSpace",
        "_CGColorSpaceCreatePattern", "_CGContextAddPath", "_CGContextBeginPage", "_CGContextBeginTransparencyLayer",
        "_CGContextClearRect", "_CGContextClip", "_CGContextClipToRect", "_CGContextConcatCTM",
        "_CGContextConvertRectToDeviceSpace", "_CGContextConvertSizeToUserSpace", "_CGContextDrawImage",
        "_CGContextDrawPath", "_CGContextDrawShading", "_CGContextEOFillPath", "_CGContextEndPage",
        "_CGContextEndTransparencyLayer", "_CGContextFillPath", "_CGContextFillRect", "_CGContextFlush",
        "_CGContextGetCTM", "_CGContextGetClipBoundingBox", "_CGContextGetTextMatrix", "_CGContextRelease",
        "_CGContextReplacePathWithStrokedPath", "_CGContextResetClip", "_CGContextRestoreGState", "_CGContextRetain",
        "_CGContextRotateCTM", "_CGContextSaveGState", "_CGContextScaleCTM", "_CGContextSetAlpha",
        "_CGContextSetBlendMode", "_CGContextSetFillColorSpace", "_CGContextSetFillColorWithColor",
        "_CGContextSetFillPattern", "_CGContextSetInterpolationQuality", "_CGContextSetLineCap",
        "_CGContextSetLineDash", "_CGContextSetLineJoin", "_CGContextSetLineWidth", "_CGContextSetPatternPhase",
        "_CGContextSetShouldAntialias", "_CGContextSetStrokeColorSpace", "_CGContextSetStrokeColorWithColor",
        "_CGContextSetStrokePattern", "_CGContextSetTextMatrix", "_CGContextStrokeLineSegments",
        "_CGContextStrokePath", "_CGContextStrokeRect", "_CGContextTranslateCTM", "_CGDataProviderCreateWithURL",
        "_CGFunctionCreate", "_CGFunctionRelease", "_CGImageCreateWithImageInRect", "_CGImageCreateWithJPEGDataProvider",
        "_CGImageCreateWithMask", "_CGImageCreateWithPNGDataProvider", "_CGImageGetAlphaInfo", "_CGImageGetHeight",
        "_CGImageGetWidth", "_CGImageRelease", "_CGImageRetain", "_CGPathAddArc", "_CGPathAddArcToPoint",
        "_CGPathAddCurveToPoint", "_CGPathAddEllipseInRect", "_CGPathAddLineToPoint", "_CGPathAddPath",
        "_CGPathAddQuadCurveToPoint", "_CGPathAddRect", "_CGPathCloseSubpath", "_CGPathContainsPoint",
        "_CGPathCreateMutable", "_CGPathCreateMutableCopy", "_CGPathGetBoundingBox", "_CGPathGetCurrentPoint",
        "_CGPathIsEmpty", "_CGPathMoveToPoint", "_CGPathRelease", "_CGPatternCreate", "_CGShadingCreateAxial",
        "_CGShadingCreateRadial", "_CGShadingRelease"}, "Quartz 2D drawing needs the CoreGraphics drawing provider");
    kit.unsupported(image_io, {"_CGImageDestinationAddImage", "_CGImageDestinationCreateWithData", "_CGImageDestinationFinalize",
        "_CGImageSourceCreateImageAtIndex", "_CGImageSourceCreateWithData"}, "image codecs need the ImageIO provider");
}

} // namespace alchemy::darwin_shells
