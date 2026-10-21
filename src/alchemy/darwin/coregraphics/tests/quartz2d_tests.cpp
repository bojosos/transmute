// Quartz 2D / ImageIO unit tests (owner: Q; CoreText's are coretext/tests). Offscreen only: every
// drawing goes into bitmap contexts in guest memory and the pixels are read
// back. The C functions are called through their exported addresses with the
// Apple ARM64 register image (alchemy::engine_call_regs), the Objective-C
// classes through ObjcRuntime::send, exactly as guest code reaches them.
#include "alchemy/darwin/coretext/ct_internal.hpp"
#include "alchemy/darwin_frameworks.hpp"
#include "alchemy/darwin_numeric.hpp"
#include "alchemy/darwin_services.hpp"
#include "alchemy/foundation_bridge.hpp"
#include "alchemy/darwin/coregraphics/imageio/codec.hpp"
#include "alchemy/darwin/coregraphics/q2d_internal.hpp"
#include "alchemy/darwin/coregraphics/quartz2d.hpp"
#include "alchemy/darwin/coregraphics/tests/quartz2d_fixture.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void shapes(Fixture& f) {
    Canvas canvas(f, 64, 64);
    require(canvas.bytes_per_row() >= 256, "bytes per row");
    canvas.rgb_fill(1, 0, 0, 1);
    canvas.fill(8, 8, 16, 16);
    require(same(canvas.cg_at(12, 12), {255, 0, 0, 255}), "filled rect pixel " + describe(canvas.cg_at(12, 12)));
    require(same(canvas.cg_at(30, 12), {0, 0, 0, 0}), "outside the rect");
    require(same(canvas.cg_at(8, 8), {255, 0, 0, 255}) && same(canvas.cg_at(7, 8), {0, 0, 0, 0}), "rect edges are pixel aligned");
    // The guest's own writes survive the next draw (flush semantics).
    f.engine.write32(canvas.data + 0 * canvas.bytes_per_row() + 63 * 4, 0xff00ff00U);   // top-right pixel: opaque green
    canvas.rgb_fill(0, 0, 1, 0.5);
    canvas.fill(40, 40, 8, 8);
    require(same(canvas.at(63, 0), {0, 255, 0, 255}), "guest pixel overwritten: " + describe(canvas.at(63, 0)));
    require(same(canvas.cg_at(44, 44), {0, 0, 128, 128}), "translucent fill (premultiplied) " + describe(canvas.cg_at(44, 44)));
    // Source-over on an existing pixel.
    canvas.rgb_fill(0, 0, 1, 0.5);
    canvas.fill(8, 8, 4, 4);
    require(same(canvas.cg_at(9, 9), {128, 0, 128, 255}), "source over " + describe(canvas.cg_at(9, 9)));
    // Ellipse: centre in, corner out, edge antialiased.
    canvas.rgb_fill(0, 1, 0, 1);
    f.call(cg, "_CGContextFillEllipseInRect", {canvas.context}, {32, 0, 32, 32});
    require(same(canvas.cg_at(48, 16), {0, 255, 0, 255}), "ellipse centre");
    require(canvas.cg_at(33, 1)[3] == 0, "ellipse corner");
    bool partial = false;
    for (std::uint32_t x = 32; x < 48 && !partial; ++x) {
        const auto a = canvas.cg_at(x, 16 + 10)[3];
        partial = a > 10 && a < 245;
    }
    require(partial, "antialiased ellipse edge");
    // Without antialiasing every pixel is all or nothing.
    f.call(cg, "_CGContextSetShouldAntialias", {canvas.context, 0});
    f.call(cg, "_CGContextClearRect", {canvas.context}, {0, 32, 32, 32});
    canvas.rgb_fill(1, 1, 1, 1);
    f.call(cg, "_CGContextFillEllipseInRect", {canvas.context}, {0, 32, 32, 32});
    for (std::uint32_t y = 32; y < 64; ++y)
        for (std::uint32_t x = 0; x < 32; ++x) {
            const auto a = canvas.cg_at(x, y)[3];
            require(a == 0 || a == 255, "aliased fill produced partial coverage");
        }
    f.call(cg, "_CGContextSetShouldAntialias", {canvas.context, 1});
    // ClearRect makes pixels transparent.
    f.call(cg, "_CGContextClearRect", {canvas.context}, {8, 8, 4, 4});
    require(same(canvas.cg_at(9, 9), {0, 0, 0, 0}), "clear rect " + describe(canvas.cg_at(9, 9)));
}

void strokes_and_paths(Fixture& f) {
    Canvas canvas(f, 64, 64);
    f.call(cg, "_CGContextSetRGBStrokeColor", {canvas.context}, {0, 0, 0, 1});
    f.call(cg, "_CGContextSetLineWidth", {canvas.context}, {4});
    f.call(cg, "_CGContextStrokeRect", {canvas.context}, {16, 16, 32, 32});
    require(same(canvas.cg_at(17, 30), {0, 0, 0, 255}), "stroke on the edge " + describe(canvas.cg_at(17, 30)));
    require(canvas.cg_at(32, 32)[3] == 0, "stroke left the inside empty");
    require(canvas.cg_at(10, 30)[3] == 0, "stroke stays near the edge");
    // A path with a CTM: translate + scale, then a line with round caps.
    f.call(cg, "_CGContextSaveGState", {canvas.context});
    f.call(cg, "_CGContextTranslateCTM", {canvas.context}, {4, 4});
    f.call(cg, "_CGContextScaleCTM", {canvas.context}, {2, 2});
    f.call(cg, "_CGContextSetLineWidth", {canvas.context}, {1});   // 2 px after scaling
    f.call(cg, "_CGContextSetLineCap", {canvas.context, 1});
    f.call(cg, "_CGContextBeginPath", {canvas.context});
    f.call(cg, "_CGContextMoveToPoint", {canvas.context}, {2, 2});
    f.call(cg, "_CGContextAddLineToPoint", {canvas.context}, {2, 5});
    f.call(cg, "_CGContextStrokePath", {canvas.context});
    f.call(cg, "_CGContextRestoreGState", {canvas.context});
    require(canvas.cg_at(8, 10)[3] > 200, "scaled stroke " + describe(canvas.cg_at(8, 10)));
    require(canvas.cg_at(11, 10)[3] == 0, "scaled stroke width");
    // The restore brought back the 4-unit line width.
    require(f.read_double(f.allocate(8)) == 0, "scratch");
    // Dashes: alternate on / off along a horizontal line.
    Canvas dashed(f, 64, 8);
    const auto lengths = f.allocate(16);
    f.engine.write64(lengths, std::bit_cast<std::uint64_t>(8.0));
    f.engine.write64(lengths + 8, std::bit_cast<std::uint64_t>(8.0));
    f.call(cg, "_CGContextSetLineDash", {dashed.context, lengths, 2}, {0});
    f.call(cg, "_CGContextSetLineWidth", {dashed.context}, {4});
    f.call(cg, "_CGContextBeginPath", {dashed.context});
    f.call(cg, "_CGContextMoveToPoint", {dashed.context}, {0, 4});
    f.call(cg, "_CGContextAddLineToPoint", {dashed.context}, {64, 4});
    f.call(cg, "_CGContextStrokePath", {dashed.context});
    require(dashed.cg_at(4, 4)[3] > 200 && dashed.cg_at(12, 4)[3] == 0 && dashed.cg_at(20, 4)[3] > 200, "dash pattern");
    // CGPath objects.
    const auto path = f.id(cg, "_CGPathCreateMutable");
    f.call(cg, "_CGPathAddArc", {path, 0, 0}, {10, 10, 5, 0, 2 * 3.14159265358979, 0});
    f.call(cg, "_CGPathCloseSubpath", {path});
    const auto box = f.call(cg, "_CGPathGetPathBoundingBox", {path});
    require(close_to(box.d(0), 5, 0.01) && close_to(box.d(1), 5, 0.01) && close_to(box.d(2), 10, 0.01) && close_to(box.d(3), 10, 0.01), "arc path bounds");
    require((f.id(cg, "_CGPathContainsPoint", {path, 0, 0}, {10, 10}) & 0xff) == 1, "path contains its centre");
    require((f.id(cg, "_CGPathContainsPoint", {path, 0, 0}, {14.9, 14.9}) & 0xff) == 0, "path excludes a far point");
    const auto rect_path = f.id(cg, "_CGPathCreateWithRect", {0}, {1, 2, 3, 4});
    const auto rect_out = f.allocate(32);
    require((f.id(cg, "_CGPathIsRect", {rect_path, rect_out}) & 0xff) == 1 && f.read_double(rect_out + 16) == 3, "CGPathIsRect");
    const auto stroked = f.id(cg, "_CGPathCreateCopyByStrokingPath", {rect_path, 0, 0, 0}, {2, 10});
    const auto stroked_box = f.call(cg, "_CGPathGetPathBoundingBox", {stroked});
    require(close_to(stroked_box.d(0), 0, 0.05) && close_to(stroked_box.d(2), 5, 0.05), "stroked path outline bounds");
    for (const auto object : {path, rect_path, stroked}) f.call(cg, "_CGPathRelease", {object});
    // CGAffineTransform helpers (indirect result).
    const auto transform = f.allocate(48);
    f.call(cg, "_CGAffineTransformMakeRotation", {}, {3.14159265358979 / 2}, transform);
    const auto rotated = f.call(cg, "_CGPointApplyAffineTransform", {transform}, {1, 0});
    require(close_to(rotated.d(0), 0, 1e-9) && close_to(rotated.d(1), 1, 1e-9), "CGAffineTransformMakeRotation");
    const auto intersection = f.call(cg, "_CGRectIntersection", {}, {0, 0, 10, 10, 5, 5, 10, 10});
    require(intersection.d(0) == 5 && intersection.d(2) == 5, "CGRectIntersection");
}

void clipping(Fixture& f) {
    Canvas canvas(f, 64, 64);
    f.call(cg, "_CGContextSaveGState", {canvas.context});
    f.call(cg, "_CGContextClipToRect", {canvas.context}, {16, 16, 16, 16});
    canvas.rgb_fill(1, 0, 0, 1);
    canvas.fill(0, 0, 64, 64);
    const auto box = f.call(cg, "_CGContextGetClipBoundingBox", {canvas.context});
    require(box.d(0) == 16 && box.d(2) == 16, "clip bounding box");
    f.call(cg, "_CGContextRestoreGState", {canvas.context});
    require(canvas.cg_at(20, 20)[0] == 255 && canvas.cg_at(40, 40)[3] == 0 && canvas.cg_at(10, 10)[3] == 0, "rect clip");
    // Even-odd clip of two nested rectangles: a ring.
    f.call(cg, "_CGContextClearRect", {canvas.context}, {0, 0, 64, 64});
    f.call(cg, "_CGContextAddRect", {canvas.context}, {8, 8, 48, 48});
    f.call(cg, "_CGContextAddRect", {canvas.context}, {24, 24, 16, 16});
    f.call(cg, "_CGContextEOClip", {canvas.context});
    canvas.rgb_fill(0, 0, 1, 1);
    canvas.fill(0, 0, 64, 64);
    require(canvas.cg_at(12, 12)[2] == 255 && canvas.cg_at(32, 32)[3] == 0 && canvas.cg_at(2, 2)[3] == 0, "even-odd clip ring");
    // Clip to a mask image: a 2x1 gray image (white | black) stretched over the canvas.
    Canvas masked(f, 32, 16);
    const auto gray = f.id(cg, "_CGColorSpaceCreateDeviceGray");
    const auto bytes = f.allocate(2);
    const std::array<std::uint8_t, 2> mask_bytes{255, 0};
    f.engine.write_bytes(bytes, mask_bytes);
    const auto provider = f.id(cg, "_CGDataProviderCreateWithData", {0, bytes, 2, 0});
    CallRegs image;
    image.x = {2, 1, 8, 8, 2, gray, 0, provider};
    image.push_stack(std::uint64_t{0});
    image.push_stack(std::uint8_t{0});
    image.push_stack(std::int32_t{0});
    const auto mask = alchemy::engine_call_regs(f.engine, f.symbol(cg, "_CGImageCreate"), image).x0;
    require(mask != 0, "CGImageCreate gray");
    f.call(cg, "_CGContextClipToMask", {masked.context, mask}, {0, 0, 32, 16});
    masked.rgb_fill(1, 1, 0, 1);
    masked.fill(0, 0, 32, 16);
    require(masked.cg_at(4, 8)[3] > 200 && masked.cg_at(28, 8)[3] < 30, "mask clip " + describe(masked.cg_at(4, 8)) + " / " + describe(masked.cg_at(28, 8)));
    f.call(cg, "_CGImageRelease", {mask});
    f.call(cg, "_CGDataProviderRelease", {provider});
    f.call(cg, "_CGColorSpaceRelease", {gray});
}

void gradients_and_images(Fixture& f) {
    Canvas canvas(f, 100, 10);
    const auto space = f.id(cg, "_CGColorSpaceCreateDeviceRGB");
    const auto components = f.allocate(64);
    const double values[] = {1, 0, 0, 1, 0, 0, 1, 1};
    for (int index = 0; index < 8; ++index) f.engine.write64(components + index * 8, std::bit_cast<std::uint64_t>(values[index]));
    const auto gradient = f.id(cg, "_CGGradientCreateWithColorComponents", {space, components, 0, 2});
    f.call(cg, "_CGContextDrawLinearGradient", {canvas.context, gradient, 3}, {0, 0, 100, 0});
    const auto left = canvas.cg_at(1, 5), right = canvas.cg_at(98, 5), middle = canvas.cg_at(50, 5);
    require(left[0] > 240 && left[2] < 15 && right[2] > 240 && right[0] < 15, "linear gradient ends " + describe(left) + " " + describe(right));
    require(middle[0] > 60 && middle[0] < 200 && middle[2] > 60 && middle[2] < 200 && middle[3] == 255, "linear gradient middle " + describe(middle));
    // Radial gradient without extension: inside the end circle only.
    Canvas radial(f, 40, 40);
    f.call(cg, "_CGContextDrawRadialGradient", {radial.context, gradient, 0}, {20, 20, 0, 20, 20, 10});
    require(radial.cg_at(20, 20)[0] > 220 && radial.cg_at(27, 20)[2] > 150, "radial centre colour " + describe(radial.cg_at(20, 20)) + " edge " + describe(radial.cg_at(27, 20)));
    require(radial.cg_at(2, 2)[3] == 0, "radial gradient stays inside its circle");
    f.call(cg, "_CGGradientRelease", {gradient});

    // A 2x2 RGBA image drawn into 32x32 without interpolation: four quadrants.
    const auto pixels = f.allocate(16);
    const std::uint8_t data[] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    f.engine.write_bytes(pixels, data);
    const auto provider = f.id(cg, "_CGDataProviderCreateWithData", {0, pixels, 16, 0});
    CallRegs image;
    image.x = {2, 2, 8, 32, 8, space, alpha_last, provider};
    image.push_stack(std::uint64_t{0});
    image.push_stack(std::uint8_t{0});
    image.push_stack(std::int32_t{0});
    const auto picture = alchemy::engine_call_regs(f.engine, f.symbol(cg, "_CGImageCreate"), image).x0;
    require(picture && f.id(cg, "_CGImageGetWidth", {picture}) == 2 && f.id(cg, "_CGImageGetBitsPerPixel", {picture}) == 32, "CGImageCreate");
    Canvas target(f, 32, 32);
    f.call(cg, "_CGContextSetInterpolationQuality", {target.context, 1});
    f.call(cg, "_CGContextDrawImage", {target.context, picture}, {0, 0, 32, 32});
    // The image's first row is the top of the drawn rect.
    require(same(target.at(4, 4), {255, 0, 0, 255}) && same(target.at(28, 4), {0, 255, 0, 255}) &&
            same(target.at(4, 28), {0, 0, 255, 255}) && same(target.at(28, 28), {255, 255, 255, 255}), "image quadrants " + describe(target.at(4, 4)));
    // CGImageCreateWithImageInRect: the top-right pixel.
    const auto cropped = f.id(cg, "_CGImageCreateWithImageInRect", {picture}, {1, 0, 1, 1});
    require(cropped && f.id(cg, "_CGImageGetWidth", {cropped}) == 1, "cropped image size");
    Canvas tiny(f, 4, 4);
    f.call(cg, "_CGContextDrawImage", {tiny.context, cropped}, {0, 0, 4, 4});
    require(same(tiny.at(2, 2), {0, 255, 0, 255}), "cropped image pixel " + describe(tiny.at(2, 2)));
    // Snapshot of a context: CGBitmapContextCreateImage + provider data.
    const auto snapshot = f.id(cg, "_CGBitmapContextCreateImage", {target.context});
    const auto copied = f.id(cg, "_CGDataProviderCopyData", {f.id(cg, "_CGImageGetDataProvider", {snapshot})});
    const std::array<Address, 1> argument{copied};
    require(f.cf.invoke("CFDataGetLength", argument) == 32 * target.bytes_per_row(), "snapshot data size");
    f.call(cf_library, "_CFRelease", {copied});
    for (const auto object : {snapshot, cropped, picture}) f.call(cg, "_CGImageRelease", {object});
    f.call(cg, "_CGDataProviderRelease", {provider});
    f.call(cg, "_CGColorSpaceRelease", {space});
}

void compositing(Fixture& f) {
    Canvas canvas(f, 32, 32);
    canvas.rgb_fill(1, 1, 1, 1);
    canvas.fill(0, 0, 32, 32);
    f.call(cg, "_CGContextSetBlendMode", {canvas.context, 1});   // multiply
    canvas.rgb_fill(0.5, 0.5, 0.5, 1);
    canvas.fill(0, 0, 16, 32);
    require(same(canvas.cg_at(4, 4), {128, 128, 128, 255}), "multiply " + describe(canvas.cg_at(4, 4)));
    require(same(canvas.cg_at(24, 4), {255, 255, 255, 255}), "multiply outside the shape");
    f.call(cg, "_CGContextSetBlendMode", {canvas.context, 17});   // copy
    canvas.rgb_fill(0, 0, 0, 0);
    canvas.fill(24, 24, 8, 8);
    require(canvas.cg_at(28, 28)[3] == 0 && canvas.cg_at(20, 28)[3] == 255, "copy blend mode");
    f.call(cg, "_CGContextSetBlendMode", {canvas.context, 0});
    // Global alpha.
    f.call(cg, "_CGContextSetAlpha", {canvas.context}, {0.5});
    canvas.rgb_fill(0, 0, 0, 1);
    canvas.fill(16, 0, 8, 8);
    require(same(canvas.cg_at(20, 4), {128, 128, 128, 255}), "global alpha " + describe(canvas.cg_at(20, 4)));
    f.call(cg, "_CGContextSetAlpha", {canvas.context}, {1});
    // Shadow without blur: offset copy of the shape in the shadow colour.
    Canvas shadowed(f, 32, 32);
    const auto black = f.id(cg, "_CGColorCreateGenericRGB", {}, {0, 0, 0, 1});
    f.call(cg, "_CGContextSetShadowWithColor", {shadowed.context, black}, {6, -6, 0});
    shadowed.rgb_fill(1, 0, 0, 1);
    shadowed.fill(4, 16, 8, 8);
    require(same(shadowed.cg_at(8, 20), {255, 0, 0, 255}), "shape over its shadow");
    require(same(shadowed.cg_at(14, 12), {0, 0, 0, 255}), "shadow offset " + describe(shadowed.cg_at(14, 12)));
    require(shadowed.cg_at(20, 20)[3] == 0, "nothing beyond the shadow");
    // Blurred shadows spread.
    f.call(cg, "_CGContextSetShadowWithColor", {shadowed.context, black}, {0, 0, 4});
    shadowed.fill(20, 2, 6, 6);
    require(shadowed.cg_at(18, 5)[3] > 0 && shadowed.cg_at(18, 5)[3] < 255, "blurred shadow edge");
    f.call(cg, "_CGColorRelease", {black});
    // Transparency layer: two overlapping 50% fills composite once.
    Canvas layered(f, 16, 16);
    f.call(cg, "_CGContextSetAlpha", {layered.context}, {0.5});
    f.call(cg, "_CGContextBeginTransparencyLayer", {layered.context, 0});
    layered.rgb_fill(0, 0, 1, 1);
    layered.fill(0, 0, 10, 16);
    layered.fill(6, 0, 10, 16);
    f.call(cg, "_CGContextEndTransparencyLayer", {layered.context});
    require(layered.cg_at(8, 8)[3] == layered.cg_at(2, 8)[3] && close_to(layered.cg_at(8, 8)[3], 128, 2), "transparency layer group alpha");
    // Other pixel layouts: BGRA (premultiplied first, 32-bit little endian) and gray.
    Canvas bgra(f, 4, 4, premultiplied_first | order_32_little);
    bgra.rgb_fill(1, 0, 0, 1);
    bgra.fill(0, 0, 4, 4);
    require(same(bgra.at(1, 1), {0, 0, 255, 255}), "BGRA layout " + describe(bgra.at(1, 1)));
    const auto gray_space = f.id(cg, "_CGColorSpaceCreateDeviceGray");
    const auto gray = f.id(cg, "_CGBitmapContextCreate", {0, 4, 4, 8, 0, gray_space, 0});
    require(gray != 0, "gray context");
    f.call(cg, "_CGContextSetGrayFillColor", {gray}, {0.5, 1});
    f.call(cg, "_CGContextFillRect", {gray}, {0, 0, 4, 4});
    const auto gray_data = f.id(cg, "_CGBitmapContextGetData", {gray});
    require(close_to(f.read(gray_data, 1)[0], 128, 2), "gray layout");
    require(f.id(cg, "_CGBitmapContextCreate", {0, 4, 4, 16, 0, gray_space, 0}) == 0, "unsupported layouts give NULL");
    f.call(cg, "_CGContextRelease", {gray});
    f.call(cg, "_CGColorSpaceRelease", {gray_space});
}

void imageio(Fixture& f) {
    // Encode a context snapshot as PNG through ImageIO and decode it back.
    Canvas canvas(f, 8, 8);
    canvas.rgb_fill(0, 1, 0, 1);
    canvas.fill(0, 0, 4, 8);
    const auto image = f.id(cg, "_CGBitmapContextCreateImage", {canvas.context});
    const auto data = f.cf.invoke("CFDataCreateMutable", std::array<Address, 2>{0, 0});
    const auto destination = f.id(io, "_CGImageDestinationCreateWithData", {data, f.str("public.png"), 1, 0});
    require(destination != 0, "CGImageDestinationCreateWithData");
    f.call(io, "_CGImageDestinationAddImage", {destination, image, 0});
    require((f.id(io, "_CGImageDestinationFinalize", {destination}) & 0xff) == 1, "CGImageDestinationFinalize");
    const auto length = f.cf.invoke("CFDataGetLength", std::array<Address, 1>{data});
    require(length > 30, "PNG bytes");
    const auto source = f.id(io, "_CGImageSourceCreateWithData", {data, 0});
    require(f.cf.utf8(f.id(io, "_CGImageSourceGetType", {source})) == "public.png" && f.id(io, "_CGImageSourceGetCount", {source}) == 1, "PNG source");
    const auto properties = f.id(io, "_CGImageSourceCopyPropertiesAtIndex", {source, 0, 0});
    require(f.cf.number_value(f.cf.dictionary_value(properties, f.str("PixelWidth"))).value_or(0) == 8, "PixelWidth property");
    const auto decoded = f.id(io, "_CGImageSourceCreateImageAtIndex", {source, 0, 0});
    Canvas back(f, 8, 8);
    f.call(cg, "_CGContextDrawImage", {back.context, decoded}, {0, 0, 8, 8});
    require(same(back.cg_at(1, 1), {0, 255, 0, 255}) && back.cg_at(6, 1)[3] == 0, "PNG round trip " + describe(back.cg_at(1, 1)));
    // An .icns file: one PNG entry and one RLE 16x16 entry with its mask.
    const auto png_bytes = f.read(f.cf.invoke("CFDataGetBytePtr", std::array<Address, 1>{data}), static_cast<std::size_t>(length));
    std::vector<std::uint8_t> icns{'i', 'c', 'n', 's', 0, 0, 0, 0};
    const auto entry = [&](const char* type, const std::vector<std::uint8_t>& payload) {
        const auto size = static_cast<std::uint32_t>(payload.size() + 8);
        icns.insert(icns.end(), type, type + 4);
        for (int shift = 24; shift >= 0; shift -= 8) icns.push_back(static_cast<std::uint8_t>(size >> shift));
        icns.insert(icns.end(), payload.begin(), payload.end());
    };
    entry("ic07", png_bytes);
    std::vector<std::uint8_t> rle;   // 3 planes of 256 bytes: red 0xff, green 0x00, blue 0x80 as runs of 128
    for (const std::uint8_t value : {0xff, 0xff, 0x00, 0x00, 0x80, 0x80}) { rle.push_back(0x80 + 125); rle.push_back(value); }
    entry("is32", rle);
    entry("s8mk", std::vector<std::uint8_t>(256, 0xff));
    const auto total = static_cast<std::uint32_t>(icns.size());
    for (int index = 0; index < 4; ++index) icns[4 + index] = static_cast<std::uint8_t>(total >> (24 - 8 * index));
    require(q2d::codec::frame_count(icns) == 2, "icns frame count");
    const auto entry16 = q2d::codec::decode(icns, 1);
    require(entry16 && entry16->width == 16 && entry16->rgba[0] == 0xff && entry16->rgba[1] == 0 && entry16->rgba[2] == 0x80 && entry16->rgba[3] == 0xff,
            "icns RLE entry");
    const auto icns_data = f.id(cf_library, "_CFDataCreate", {0, [&] { const auto at = f.allocate(icns.size()); f.engine.write_bytes(at, icns); return at; }(), icns.size()});
    const auto icns_source = f.id(io, "_CGImageSourceCreateWithData", {icns_data, 0});
    require(f.cf.utf8(f.id(io, "_CGImageSourceGetType", {icns_source})) == "com.apple.icns" && f.id(io, "_CGImageSourceGetCount", {icns_source}) == 2,
            "icns image source");
    const auto icon = f.id(io, "_CGImageSourceCreateImageAtIndex", {icns_source, 0, 0});
    require(f.id(cg, "_CGImageGetWidth", {icon}) == 8, "icns PNG entry");
    // JPEG through the destination: lossy but close.
    const auto jpeg_data = f.cf.invoke("CFDataCreateMutable", std::array<Address, 2>{0, 0});
    const auto jpeg = f.id(io, "_CGImageDestinationCreateWithData", {jpeg_data, f.str("public.jpeg"), 1, 0});
    f.call(io, "_CGImageDestinationAddImage", {jpeg, image, 0});
    require((f.id(io, "_CGImageDestinationFinalize", {jpeg}) & 0xff) == 1, "JPEG encode");
    const auto jpeg_source = f.id(io, "_CGImageSourceCreateWithData", {jpeg_data, 0});
    require(f.cf.utf8(f.id(io, "_CGImageSourceGetType", {jpeg_source})) == "public.jpeg", "JPEG source type");
    for (const auto object : {image, data, destination, source, properties, decoded, icns_data, icns_source, icon, jpeg_data, jpeg, jpeg_source}) {
        f.call(cf_library, "_CFRelease", {object});
    }
}

void appkit_drawing(Fixture& f) {
    // NSImage -lockFocus: NSColor, NSBezierPath and NSString drawing into its bitmap.
    const auto image = f.send(f.send(f.cls("NSImage"), "alloc").x0, "initWithSize:", {}, {40, 20}).x0;
    static_cast<void>(f.send(image, "lockFocus"));
    const auto context = f.send(f.cls("NSGraphicsContext"), "currentContext").x0;
    require(context != 0 && f.send(context, "CGContext").x0 != 0, "lockFocus makes a current context");
    static_cast<void>(f.send(f.send(f.cls("NSColor"), "blueColor").x0, "setFill"));
    static_cast<void>(f.send(f.send(f.cls("NSBezierPath"), "bezierPathWithRect:", {}, {0, 0, 10, 20}).x0, "fill"));
    const auto path = f.send(f.cls("NSBezierPath"), "bezierPath").x0;
    static_cast<void>(f.send(path, "moveToPoint:", {}, {20, 2}));
    static_cast<void>(f.send(path, "lineToPoint:", {}, {38, 2}));
    static_cast<void>(f.send(path, "setLineWidth:", {}, {2}));
    static_cast<void>(f.send(f.send(f.cls("NSColor"), "redColor").x0, "setStroke"));
    static_cast<void>(f.send(path, "stroke"));
    static_cast<void>(f.send(image, "unlockFocus"));
    require(f.send(f.cls("NSGraphicsContext"), "currentContext").x0 == 0, "unlockFocus restores the context");
    const auto rep = f.send(f.send(image, "representations").x0, "objectAtIndex:", {0}).x0;
    const auto data = f.send(rep, "bitmapData").x0;
    const auto row = f.send(rep, "bytesPerRow").x0;
    const auto pixel = [&](Address x, Address y_from_top) { return f.read(data + y_from_top * row + x * 4, 4); };
    require(pixel(5, 10) == std::vector<std::uint8_t>{0, 0, 255, 255}, "NSBezierPath fill into the image");
    require(pixel(30, 17)[0] == 255 && pixel(30, 17)[3] == 255 && pixel(30, 10)[3] == 0, "NSBezierPath stroke into the image");
    // NSString measuring with a font attribute.
    const auto font = f.send(f.cls("NSFont"), "systemFontOfSize:", {}, {13}).x0;
    std::vector<std::pair<Address, Address>> pairs{{f.str("NSFont"), font}};
    const auto attributes = f.cf.dictionary(pairs);
    const auto size = f.send(f.str("Hello"), "sizeWithAttributes:", {attributes});
    require(size.d(0) > 20 && size.d(0) < 50 && size.d(1) > 12 && size.d(1) < 25, "sizeWithAttributes " + std::to_string(size.d(0)) + "x" + std::to_string(size.d(1)));
    // Drawing text through NSGraphicsContext over an NSBitmapImageRep.
    Canvas canvas(f, 60, 30);
    const auto ns_context = f.send(f.cls("NSGraphicsContext"), "graphicsContextWithCGContext:flipped:", {canvas.context, 0}).x0;
    static_cast<void>(f.send(f.cls("NSGraphicsContext"), "saveGraphicsState"));
    static_cast<void>(f.send(f.cls("NSGraphicsContext"), "setCurrentContext:", {ns_context}));
    static_cast<void>(f.send(f.str("Hi"), "drawAtPoint:withAttributes:", {attributes}, {2, 4}));
    static_cast<void>(f.send(f.cls("NSGraphicsContext"), "restoreGraphicsState"));
    int dark{};
    for (std::uint32_t y = 0; y < 30; ++y)
        for (std::uint32_t x = 0; x < 60; ++x)
            if (canvas.at(x, y)[3] > 128) ++dark;
    require(dark > 20, "NSString drawing produced pixels");
    require(canvas.cg_at(30, 26)[3] == 0, "text stays near its point");
    // NSAttributedString with attributes, measured.
    const auto attributed = f.send(f.send(f.cls("NSAttributedString"), "alloc").x0, "initWithString:attributes:", {f.str("abc"), attributes}).x0;
    require(f.send(attributed, "length").x0 == 3, "NSAttributedString length");
    const auto range = f.allocate(16);
    const auto at_index = f.send(attributed, "attributesAtIndex:effectiveRange:", {1, range}).x0;
    require(f.cf.dictionary_value(at_index, f.str("NSFont")) == font && f.engine.read64(range + 8) == 3, "attribute runs");
    const auto mutable_string = f.send(attributed, "mutableCopyWithZone:", {0}).x0;
    static_cast<void>(f.send(mutable_string, "addAttribute:value:range:", {f.str("NSKern"), f.cf.real(2), 1, 1}));
    static_cast<void>(f.send(mutable_string, "attributesAtIndex:effectiveRange:", {1, range}));
    require(f.engine.read64(range) == 1 && f.engine.read64(range + 8) == 1, "addAttribute:value:range: splits runs");
    static_cast<void>(f.send(mutable_string, "replaceCharactersInRange:withString:", {0, 1, f.str("XY")}));
    require(f.cf.utf8(f.send(mutable_string, "string").x0) == "XYbc", "replaceCharactersInRange:withString:");
    const auto measured = f.send(mutable_string, "size");
    require(measured.d(0) > 15, "-[NSAttributedString size]");
}

// CGColorSpaceCopyICCData of the named spaces: ICC v4 display profiles built
// from the standards' parameters (icc_profiles.cpp).
std::vector<std::uint8_t> data_contents(Fixture& f, Address data) {
    const auto length = f.cf.invoke("CFDataGetLength", std::array<Address, 1>{data});
    return f.read(f.cf.invoke("CFDataGetBytePtr", std::array<Address, 1>{data}), static_cast<std::size_t>(length));
}
std::uint32_t be32(const std::vector<std::uint8_t>& b, std::size_t at) {
    require(at + 4 <= b.size(), "ICC read past the end");
    return (std::uint32_t{b[at]} << 24) | (std::uint32_t{b[at + 1]} << 16) | (std::uint32_t{b[at + 2]} << 8) | b[at + 3];
}
double fixed(const std::vector<std::uint8_t>& b, std::size_t at) { return static_cast<std::int32_t>(be32(b, at)) / 65536.0; }
std::string four(const std::vector<std::uint8_t>& b, std::size_t at) {
    require(at + 4 <= b.size(), "ICC read past the end");
    return {b.begin() + static_cast<std::ptrdiff_t>(at), b.begin() + static_cast<std::ptrdiff_t>(at + 4)};
}
// Tag signature -> data offset, checking the header and the tag table.
std::map<std::string, std::size_t> icc_tags(const std::vector<std::uint8_t>& b, bool gray, const std::string& name) {
    require(b.size() >= 132 && be32(b, 0) == b.size() && b.size() % 4 == 0, name + ": ICC size field");
    require(b[8] == 4 && four(b, 12) == "mntr" && four(b, 16) == (gray ? "GRAY" : "RGB ") && four(b, 20) == "XYZ " &&
            four(b, 36) == "acsp", name + ": ICC header");
    require(close_to(fixed(b, 68), 0.9642, 1e-4) && close_to(fixed(b, 72), 1.0, 1e-4) && close_to(fixed(b, 76), 0.8249, 1e-4),
            name + ": PCS illuminant D50");
    const auto count = be32(b, 128);
    require(count < 32 && 132 + 12 * count <= b.size(), name + ": ICC tag count");
    std::map<std::string, std::size_t> tags;
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto entry = 132 + 12 * static_cast<std::size_t>(index);
        const auto offset = be32(b, entry + 4), size = be32(b, entry + 8);
        require(offset % 4 == 0 && offset >= 132 + 12 * count && offset + size <= b.size() && size >= 12, name + ": ICC tag extent");
        tags[four(b, entry)] = offset;
    }
    const std::vector<std::string> required = gray ? std::vector<std::string>{"desc", "cprt", "wtpt", "kTRC"}
        : std::vector<std::string>{"desc", "cprt", "wtpt", "chad", "rXYZ", "gXYZ", "bXYZ", "rTRC", "gTRC", "bTRC"};
    for (const auto& tag : required) require(tags.contains(tag), name + ": ICC tag " + tag);
    require(four(b, tags.at("desc")) == "mluc" && four(b, tags.at("wtpt")) == "XYZ ", name + ": desc / wtpt types");
    require(close_to(fixed(b, tags.at("wtpt") + 8), 0.9642, 1e-4) && close_to(fixed(b, tags.at("wtpt") + 12), 1.0, 1e-4),
            name + ": wtpt is D50 (v4 display profiles)");
    return tags;
}

void color_profiles(Fixture& f) {
    struct Expected { const char* name; bool gray; double xy[4][2]; double gamma; };
    // Standard parameters (red, green, blue, white xy; the curve's g).
    const Expected expected[] = {
        {"kCGColorSpaceSRGB", false, {{0.64, 0.33}, {0.30, 0.60}, {0.15, 0.06}, {0.3127, 0.3290}}, 2.4},
        {"kCGColorSpaceExtendedSRGB", false, {{0.64, 0.33}, {0.30, 0.60}, {0.15, 0.06}, {0.3127, 0.3290}}, 2.4},
        {"kCGColorSpaceLinearSRGB", false, {{0.64, 0.33}, {0.30, 0.60}, {0.15, 0.06}, {0.3127, 0.3290}}, 1.0},
        {"kCGColorSpaceExtendedLinearSRGB", false, {{0.64, 0.33}, {0.30, 0.60}, {0.15, 0.06}, {0.3127, 0.3290}}, 1.0},
        {"kCGColorSpaceGenericRGB", false, {{0.64, 0.33}, {0.30, 0.60}, {0.15, 0.06}, {0.3127, 0.3290}}, 2.4},
        {"kCGColorSpaceGenericRGBLinear", false, {{0.64, 0.33}, {0.30, 0.60}, {0.15, 0.06}, {0.3127, 0.3290}}, 1.0},
        {"kCGColorSpaceDisplayP3", false, {{0.680, 0.320}, {0.265, 0.690}, {0.150, 0.060}, {0.3127, 0.3290}}, 2.4},
        {"kCGColorSpaceExtendedDisplayP3", false, {{0.680, 0.320}, {0.265, 0.690}, {0.150, 0.060}, {0.3127, 0.3290}}, 2.4},
        {"kCGColorSpaceDCIP3", false, {{0.680, 0.320}, {0.265, 0.690}, {0.150, 0.060}, {0.314, 0.351}}, 2.6},
        {"kCGColorSpaceAdobeRGB1998", false, {{0.64, 0.33}, {0.21, 0.71}, {0.15, 0.06}, {0.3127, 0.3290}}, 563.0 / 256},
        {"kCGColorSpaceITUR_709", false, {{0.64, 0.33}, {0.30, 0.60}, {0.15, 0.06}, {0.3127, 0.3290}}, 1 / 0.45},
        {"kCGColorSpaceITUR_2020", false, {{0.708, 0.292}, {0.170, 0.797}, {0.131, 0.046}, {0.3127, 0.3290}}, 1 / 0.45},
        {"kCGColorSpaceROMMRGB", false, {{0.7347, 0.2653}, {0.1596, 0.8404}, {0.0366, 0.0001}, {0.3457, 0.3585}}, 1.8},
        {"kCGColorSpaceACESCGLinear", false, {{0.713, 0.293}, {0.165, 0.830}, {0.128, 0.044}, {0.32168, 0.33767}}, 1.0},
        {"kCGColorSpaceGenericGray", true, {}, 2.2},
        {"kCGColorSpaceGenericGrayGamma2_2", true, {}, 2.2},
        {"kCGColorSpaceLinearGray", true, {}, 1.0},
        {"kCGColorSpaceExtendedGray", true, {}, 2.2},
        {"kCGColorSpaceExtendedLinearGray", true, {}, 1.0},
    };
    using M = std::array<std::array<double, 3>, 3>;
    const auto invert = [](const M& m) {
        const auto det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                         m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
        M r{};
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) {
            const int a = (j + 1) % 3, b = (j + 2) % 3, c = (i + 1) % 3, d = (i + 2) % 3;
            r[i][j] = (m[a][c] * m[b][d] - m[a][d] * m[b][c]) / det;
        }
        return r;
    };
    for (const auto& entry : expected) {
        const std::string name = entry.name;
        const auto space = f.id(cg, "_CGColorSpaceCreateWithName", {f.str(name)});
        require(space != 0, name + ": CGColorSpaceCreateWithName");
        const auto data = f.id(cg, "_CGColorSpaceCopyICCData", {space});
        require(data != 0, name + ": CGColorSpaceCopyICCData is NULL");
        const auto bytes = data_contents(f, data);
        const auto tags = icc_tags(bytes, entry.gray, name);
        const auto trc = tags.at(entry.gray ? "kTRC" : "rTRC");
        require(four(bytes, trc) == "para" && close_to(fixed(bytes, trc + 12), entry.gamma, 1e-4), name + ": TRC exponent");
        if (!entry.gray) {
            require(tags.at("rTRC") == tags.at("gTRC") && tags.at("gTRC") == tags.at("bTRC"), name + ": one shared curve");
            // Colorants back through the inverse of chad give the standard's primaries and white.
            M chad{};
            for (int i = 0; i < 9; ++i) chad[i / 3][i % 3] = fixed(bytes, tags.at("chad") + 8 + 4 * static_cast<std::size_t>(i));
            const auto unadapt = invert(chad);
            std::array<double, 3> white{};
            int channel = 0;
            for (const auto* tag : {"rXYZ", "gXYZ", "bXYZ"}) {
                std::array<double, 3> d50{}, source{};
                for (int i = 0; i < 3; ++i) d50[i] = fixed(bytes, tags.at(tag) + 8 + 4 * static_cast<std::size_t>(i));
                for (int i = 0; i < 3; ++i) source[i] = unadapt[i][0] * d50[0] + unadapt[i][1] * d50[1] + unadapt[i][2] * d50[2];
                const auto sum = source[0] + source[1] + source[2];
                require(close_to(source[0] / sum, entry.xy[channel][0], 1e-3) && close_to(source[1] / sum, entry.xy[channel][1], 1e-3),
                        name + ": primary " + tag);
                for (int i = 0; i < 3; ++i) white[i] += source[i];
                ++channel;
            }
            const auto sum = white[0] + white[1] + white[2];
            require(close_to(white[0] / sum, entry.xy[3][0], 1e-3) && close_to(white[1] / sum, entry.xy[3][1], 1e-3) &&
                    close_to(white[1], 1.0, 2e-3), name + ": white point");
        }
        // Cached: a second copy has the same bytes.
        const auto again = f.id(cg, "_CGColorSpaceCopyICCData", {space});
        require(again != 0 && data_contents(f, again) == bytes, name + ": profile is stable");
        // A space made from the profile has the same model and components and its own bytes back.
        const auto from_icc = f.id(cg, "_CGColorSpaceCreateWithICCData", {data});
        require(from_icc != 0, name + ": CGColorSpaceCreateWithICCData");
        require(f.id(cg, "_CGColorSpaceGetModel", {from_icc}) == f.id(cg, "_CGColorSpaceGetModel", {space}) &&
                f.id(cg, "_CGColorSpaceGetNumberOfComponents", {from_icc}) == f.id(cg, "_CGColorSpaceGetNumberOfComponents", {space}),
                name + ": model / components of the ICC-based space");
        const auto back = f.id(cg, "_CGColorSpaceCopyICCData", {from_icc});
        require(back != 0 && data_contents(f, back) == bytes, name + ": ICC-based space keeps the bytes");
        // It converts like the named space (wide-gamut and linear spaces differ from plain sRGB).
        const auto components = f.allocate(4 * 8);
        const std::array<double, 4> rgb{0.9, 0.25, 0.5, 1.0}, gray{0.5, 1.0, 0, 0};
        for (std::size_t i = 0; i < 4; ++i) f.engine.write64(components + 8 * i, std::bit_cast<std::uint64_t>(entry.gray ? gray[i] : rgb[i]));
        std::array<std::array<std::uint8_t, 4>, 2> pixels{};
        int which = 0;
        for (const auto candidate : {space, from_icc}) {
            const auto color = f.id(cg, "_CGColorCreate", {candidate, components});
            Canvas canvas(f, 4, 4);
            f.call(cg, "_CGContextSetFillColorWithColor", {canvas.context, color});
            canvas.fill(0, 0, 4, 4);
            pixels[static_cast<std::size_t>(which++)] = canvas.cg_at(1, 1);
            f.call(cg, "_CGColorRelease", {color});
        }
        require(same(pixels[0], pixels[1], 0), name + ": ICC-based space draws " + describe(pixels[1]) + ", the named " + describe(pixels[0]));
        for (const auto object : {data, again, back}) f.call(cf_library, "_CFRelease", {object});
        f.call(cg, "_CGColorSpaceRelease", {from_icc});
        f.call(cg, "_CGColorSpaceRelease", {space});
    }
    // Published Bradford-adapted (D50) sRGB colorants.
    {
        const auto space = f.id(cg, "_CGColorSpaceCreateWithName", {f.str("kCGColorSpaceSRGB")});
        const auto data = f.id(cg, "_CGColorSpaceCopyICCData", {space});
        const auto bytes = data_contents(f, data);
        const auto tags = icc_tags(bytes, false, "sRGB");
        const double colorants[3][3]{{0.4361, 0.2225, 0.0139}, {0.3851, 0.7169, 0.0971}, {0.1431, 0.0606, 0.7141}};
        int channel = 0;
        for (const auto* tag : {"rXYZ", "gXYZ", "bXYZ"}) {
            for (int i = 0; i < 3; ++i)
                require(close_to(fixed(bytes, tags.at(tag) + 8 + 4 * static_cast<std::size_t>(i)), colorants[channel][i], 5e-4),
                        std::string("sRGB D50 colorant ") + tag);
            ++channel;
        }
        // Display P3 and sRGB are different profiles; the extended space shares sRGB's.
        const auto p3_space = f.id(cg, "_CGColorSpaceCreateWithName", {f.str("kCGColorSpaceDisplayP3")});
        const auto extended = f.id(cg, "_CGColorSpaceCreateWithName", {f.str("kCGColorSpaceExtendedSRGB")});
        const auto p3_data = f.id(cg, "_CGColorSpaceCopyICCData", {p3_space}), extended_data = f.id(cg, "_CGColorSpaceCopyICCData", {extended});
        require(data_contents(f, p3_data) != bytes && data_contents(f, extended_data) == bytes, "profiles per standard");
        for (const auto object : {data, p3_data, extended_data}) f.call(cf_library, "_CFRelease", {object});
        for (const auto object : {space, p3_space, extended}) f.call(cg, "_CGColorSpaceRelease", {object});
    }
    // Spaces without a profile, as on macOS: CMYK, Lab, XYZ and the device spaces.
    for (const auto* name : {"kCGColorSpaceGenericCMYK", "kCGColorSpaceGenericLab", "kCGColorSpaceGenericXYZ"}) {
        const auto space = f.id(cg, "_CGColorSpaceCreateWithName", {f.str(name)});
        require(space != 0 && f.id(cg, "_CGColorSpaceCopyICCData", {space}) == 0, std::string(name) + " has no ICC data");
        f.call(cg, "_CGColorSpaceRelease", {space});
    }
    const auto device = f.id(cg, "_CGColorSpaceCreateDeviceRGB");
    require(f.id(cg, "_CGColorSpaceCopyICCData", {device}) == 0, "DeviceRGB has no ICC data");
    f.call(cg, "_CGColorSpaceRelease", {device});
    // Another profile keeps its own bytes and converts by its model.
    std::vector<std::uint8_t> foreign(132, 0);
    foreign[3] = 132; foreign[8] = 4;
    std::memcpy(foreign.data() + 16, "RGB XYZ ", 8);
    std::memcpy(foreign.data() + 36, "acsp", 4);
    const auto foreign_data = f.cf.invoke("CFDataCreate", std::array<Address, 3>{0, [&] {
        const auto at = f.allocate(foreign.size()); f.engine.write_bytes(at, foreign); return at; }(), foreign.size()});
    const auto foreign_space = f.id(cg, "_CGColorSpaceCreateWithICCData", {foreign_data});
    const auto foreign_back = f.id(cg, "_CGColorSpaceCopyICCData", {foreign_space});
    require(foreign_space != 0 && foreign_back != 0 && data_contents(f, foreign_back) == foreign, "a foreign profile keeps its bytes");
    for (const auto object : {foreign_data, foreign_back}) f.call(cf_library, "_CFRelease", {object});
    f.call(cg, "_CGColorSpaceRelease", {foreign_space});
}

// Chrome's GPU process (Graphite on IOSurface-backed shared images): the
// surface gets a named color space, then that name's profile is read.
void chrome_iosurface_color_space(Fixture& f) {
    constexpr std::string_view iosurface = "/System/Library/Frameworks/IOSurface.framework/Versions/A/IOSurface";
    const auto number = [&](std::int64_t value) { return f.cf.integer(value, 9 /* kCFNumberSInt64Type */); };
    const std::array<std::pair<Address, Address>, 4> entries{{
        {f.str("IOSurfaceWidth"), number(64)}, {f.str("IOSurfaceHeight"), number(64)},
        {f.str("IOSurfaceBytesPerElement"), number(4)}, {f.str("IOSurfacePixelFormat"), number(0x42475241)}}};
    const auto surface = f.id(iosurface, "_IOSurfaceCreate", {f.cf.dictionary(entries)});
    require(surface != 0, "IOSurfaceCreate");
    const auto name = f.cf.constant_string("kCGColorSpaceSRGB");
    f.call(iosurface, "_IOSurfaceSetValue", {surface, f.str("IOSurfaceColorSpace"), name});
    const auto stored = f.id(iosurface, "_IOSurfaceCopyValue", {surface, f.str("IOSurfaceColorSpace")});
    require(stored != 0 && f.cf.utf8(stored) == "kCGColorSpaceSRGB", "IOSurfaceColorSpace value");
    const auto space = f.id(cg, "_CGColorSpaceCreateWithName", {stored});
    const auto icc = f.id(cg, "_CGColorSpaceCopyICCData", {space});
    require(icc != 0, "CGColorSpaceCopyICCData of the surface's color space is NULL");
    static_cast<void>(icc_tags(data_contents(f, icc), false, "IOSurface sRGB"));
    f.call(cf_library, "_CFRelease", {icc});
    f.call(cg, "_CGColorSpaceRelease", {space});
    f.call(cf_library, "_CFRelease", {stored});
    f.call(cf_library, "_CFRelease", {surface});
    // gfx::IOSurfaceCanSetColorSpace sets a video color space's values on a NULL
    // surface to see whether it can; macOS ignores the call.
    f.call(iosurface, "_IOSurfaceSetValue", {0, f.str("IOSurfaceColorPrimaries"), f.cf.constant_string("ITU_R_709_2")});
}

} // namespace

int main() {
#ifdef _WIN32
    _putenv_s("ALCHEMY_GUI_NO_ACTIVATE", "1");
#endif
    try {
        Fixture fixture;
        shapes(fixture);
        strokes_and_paths(fixture);
        clipping(fixture);
        gradients_and_images(fixture);
        compositing(fixture);
        imageio(fixture);
        appkit_drawing(fixture);
        color_profiles(fixture);
        {
            Fixture with_quartzcore(true);
            chrome_iosurface_color_space(with_quartzcore);
        }
        std::cout << "Alchemy Quartz 2D tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Quartz 2D test failed: " << error.what() << '\n';
        return 1;
    }
}
