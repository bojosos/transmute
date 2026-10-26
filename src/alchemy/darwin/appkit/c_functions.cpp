#include "alchemy/darwin/appkit/c_functions.hpp"

#include "alchemy/darwin/appkit/appkit_internal.hpp"
#include "alchemy/engine_adapter.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// AppKit's C functions besides the drawing primitives Quartz 2D and
// graphics.cpp register (NSRectFill, NSFrameRect, NSEraseRect, ...):
// - Rectangle lists and tiled rectangles: NSRectFillList* and NSRectClipList
//   fill or clip through NSRectFill / CGContextClipToRects; NSDrawTiledRects
//   and NSDrawColorTiledRects slice one-point strips off the given sides in
//   their grays / colors; the classic bezels (NSDrawButton, NSDrawGrayBezel,
//   NSDrawWhiteBezel, NSDrawDarkBezel, NSDrawLightBezel, NSDrawGroove) are
//   tiled rectangles of NSBlack / NSDarkGray / NSLightGray / NSWhite edges
//   around a filled middle, mirrored in flipped contexts. NSDrawNinePartImage
//   draws corners at their size and stretches edges and middle;
//   NSDottedFrameRect, NSHighlightRect, NSDrawWindowBackground and
//   NSDrawBitmap draw; NSReadPixel reads the current bitmap context.
// - Windows: NSCountWindows / NSWindowList (and the ...ForContext forms,
//   whose context is this application) are the application's windows on
//   screen, front to back. NSDisableScreenUpdates / NSEnableScreenUpdates
//   nest; windows are presented when updates resume.
// - Depths: NSBestDepth and the depth accessors use NSWindowDepth = bits per
//   sample | (color space << 8); windows offer 24, 64 and 128-bit RGB.
// - Alerts: NSRunAlertPanel and its informational / critical forms run an
//   NSAlert (the message is the printf-style format with its arguments);
//   NSBeginAlertSheet runs one as a sheet with the legacy delegate callback.
//   Alert panels as window objects (NSGetAlertPanel) stop explicitly.
// - Accessibility helpers convert to screen coordinates;
//   NSAccessibilitySetMayContainProtectedContent is kept.
// - Services: providers register as NSApp's services provider; there is no
//   Services menu, so per-item visibility is kept for NSShowsServicesMenuItem.
// - Pasteboard types: the typed file-contents / filenames types
//   ("NSTypedFileContentsPboardType:<type>", "NSTypedFilenamesPboardType:<type>").
// - NSShowAnimationEffect draws no effect and tells the delegate at once;
//   NSInterfaceStyleForKey answers NSMacintoshInterfaceStyle;
//   NSTouchTypeMaskFromType shifts. NSCopyBits and NSGetWindowServerMemory
//   stop explicitly (no window server bitmaps to copy or account).
namespace alchemy::gui::appkit {
namespace {

std::atomic<int> screen_update_holds{0};

// NSRectEdge
constexpr std::uint64_t min_x = 0, min_y = 1, max_x = 2, max_y = 3;
constexpr double black = 0, dark_gray = 1.0 / 3, light_gray = 2.0 / 3, white = 1;
constexpr std::string_view typed_contents = "NSTypedFileContentsPboardType:", typed_filenames = "NSTypedFilenamesPboardType:";

struct Bezel {
    std::vector<std::uint64_t> sides;
    std::vector<double> grays;
    double fill{};
    bool filled{true};
};

} // namespace

bool screen_updates_disabled() { return screen_update_holds.load() > 0; }

void install_c_functions(AppKit& kit) {
    auto* k = &kit;
    const auto both = [k](const std::string& symbol, Handler handler) {
        k->function(appkit_library, symbol, handler);
        k->function(cocoa_library, symbol, std::move(handler));
    };
    // An exported function called with a full register image (rectangles in d0..d3).
    const auto call = [k](std::string_view library, std::string_view symbol, const CallRegs& registers) {
        const auto found = k->process.resolve(library, symbol);
        if (!found) fail("missing " + std::string(symbol));
        return EngineAdapter(k->process.calling_engine()).call_regs(found->address, registers);
    };
    const auto fill = [call](Rect rect, std::optional<std::uint64_t> operation = std::nullopt) {
        CallRegs registers;
        put_rect(registers, 0, rect);
        if (operation) {
            registers.x[0] = *operation;
            static_cast<void>(call(appkit_library, "_NSRectFillUsingOperation", registers));
        } else {
            static_cast<void>(call(appkit_library, "_NSRectFill", registers));
        }
    };
    const auto set_gray = [k](double gray) {
        CallRegs registers;
        registers.set_d(0, gray);
        registers.set_d(1, 1);
        static_cast<void>(k->send(k->send(k->color, "colorWithCalibratedWhite:alpha:", registers).x0, "set"));
    };
    const auto current_context = [k] { return k->send(k->graphics_context, "currentContext").x0; };
    const auto flipped = [k, current_context] {
        const auto context = current_context();
        return context && k->send_bool(context, "isFlipped");
    };
    const auto read_rects = [k](Address at, Address count) {
        std::vector<Rect> rects;
        for (Address index = 0; index < std::min<Address>(count, 1 << 20); ++index) {
            const auto base = at + index * 32;
            const auto value = [&](unsigned i) { return std::bit_cast<double>(k->memory().read64(base + i * 8)); };
            rects.push_back({{value(0), value(1)}, {value(2), value(3)}});
        }
        return rects;
    };

    // ---- rectangle lists ----
    both("_NSRectFillListUsingOperation", [read_rects, fill](CpuState& s, GuestMemory&) {
        for (const auto& rect : read_rects(s.x[0], s.x[1])) fill(rect, s.x[2]);
    });
    both("_NSRectFillListWithGrays", [k, read_rects, fill, set_gray](CpuState& s, GuestMemory&) {
        const auto rects = read_rects(s.x[0], s.x[2]);
        for (std::size_t index = 0; index < rects.size(); ++index) {
            set_gray(std::bit_cast<double>(k->memory().read64(s.x[1] + index * 8)));
            fill(rects[index]);
        }
    });
    both("_NSRectFillListWithColors", [k, read_rects, fill](CpuState& s, GuestMemory&) {
        const auto rects = read_rects(s.x[0], s.x[2]);
        for (std::size_t index = 0; index < rects.size(); ++index) {
            static_cast<void>(k->send(k->memory().read64(s.x[1] + index * 8), "set"));
            fill(rects[index]);
        }
    });
    both("_NSRectFillListWithColorsUsingOperation", [k, read_rects, fill](CpuState& s, GuestMemory&) {
        const auto rects = read_rects(s.x[0], s.x[2]);
        for (std::size_t index = 0; index < rects.size(); ++index) {
            static_cast<void>(k->send(k->memory().read64(s.x[1] + index * 8), "set"));
            fill(rects[index], s.x[3]);
        }
    });
    both("_NSRectClipList", [k, current_context](CpuState& s, GuestMemory&) {
        const auto context = current_context();
        if (!context) return;
        static_cast<void>(k->guest_call(core_graphics_library, "_CGContextClipToRects", {k->send(context, "CGContext").x0, s.x[0], s.x[1]}));
    });

    // ---- tiled rectangles and bezels ----
    // Slices one-point strips off `bounds`' `sides`, each filled (within
    // `clip`) by `paint(index)`; answers what is left.
    const auto tile = [fill, flipped](Rect bounds, Rect clip, const std::vector<std::uint64_t>& sides, const std::function<void(std::size_t)>& paint) {
        const bool mirror = flipped();
        auto rest = bounds;
        for (std::size_t index = 0; index < sides.size(); ++index) {
            auto side = sides[index];
            if (mirror && (side == min_y || side == max_y)) side = side == min_y ? max_y : min_y;
            if (rest.size.width <= 0 || rest.size.height <= 0) break;
            Rect strip = rest;
            switch (side) {
            case min_x: strip.size.width = 1; rest.origin.x += 1; rest.size.width -= 1; break;
            case max_x: strip.origin.x += rest.size.width - 1; strip.size.width = 1; rest.size.width -= 1; break;
            case min_y: strip.size.height = 1; rest.origin.y += 1; rest.size.height -= 1; break;
            default: strip.origin.y += rest.size.height - 1; strip.size.height = 1; rest.size.height -= 1; break;
            }
            const auto x0 = std::max(strip.origin.x, clip.origin.x), y0 = std::max(strip.origin.y, clip.origin.y);
            const auto x1 = std::min(strip.origin.x + strip.size.width, clip.origin.x + clip.size.width);
            const auto y1 = std::min(strip.origin.y + strip.size.height, clip.origin.y + clip.size.height);
            if (x1 > x0 && y1 > y0) {
                paint(index);
                fill({{x0, y0}, {x1 - x0, y1 - y0}});
            }
        }
        return rest;
    };
    const auto read_sides = [k](Address at, Address count) {
        std::vector<std::uint64_t> sides;
        for (Address index = 0; index < std::min<Address>(count, 4096); ++index) sides.push_back(k->memory().read64(at + index * 8));
        return sides;
    };
    both("_NSDrawTiledRects", [k, tile, read_sides, set_gray](CpuState& s, GuestMemory&) {
        // (bounds d0..d3, clip d4..d7, sides x0, grays x1, count x2) -> the remaining rect
        const Rect bounds{{d(s, 0), d(s, 1)}, {d(s, 2), d(s, 3)}}, clip{{d(s, 4), d(s, 5)}, {d(s, 6), d(s, 7)}};
        const auto grays = s.x[1];
        return_rect(s, tile(bounds, clip, read_sides(s.x[0], s.x[2]), [k, grays, set_gray](std::size_t index) {
            set_gray(std::bit_cast<double>(k->memory().read64(grays + index * 8)));
        }));
    });
    both("_NSDrawColorTiledRects", [k, tile, read_sides](CpuState& s, GuestMemory&) {
        const Rect bounds{{d(s, 0), d(s, 1)}, {d(s, 2), d(s, 3)}}, clip{{d(s, 4), d(s, 5)}, {d(s, 6), d(s, 7)}};
        const auto colors = s.x[1];
        return_rect(s, tile(bounds, clip, read_sides(s.x[0], s.x[2]), [k, colors](std::size_t index) {
            static_cast<void>(k->send(k->memory().read64(colors + index * 8), "set"));
        }));
    });
    const std::map<std::string, Bezel> bezels{
        {"_NSDrawButton", {{max_x, min_y, min_x, max_y, max_x, min_y}, {black, black, white, white, dark_gray, dark_gray}, light_gray}},
        {"_NSDrawGrayBezel", {{max_x, min_y, min_x, max_y, max_x, min_y, min_x, max_y}, {white, white, dark_gray, dark_gray, light_gray, light_gray, black, black}, light_gray}},
        {"_NSDrawWhiteBezel", {{max_x, min_y, min_x, max_y, max_x, min_y, min_x, max_y}, {white, white, dark_gray, dark_gray, light_gray, light_gray, dark_gray, dark_gray}, white}},
        {"_NSDrawDarkBezel", {{max_x, min_y, min_x, max_y, max_x, min_y, min_x, max_y}, {white, white, dark_gray, dark_gray, light_gray, light_gray, black, black}, dark_gray}},
        {"_NSDrawLightBezel", {{max_x, min_y, min_x, max_y, max_x, min_y, min_x, max_y}, {white, white, dark_gray, dark_gray, light_gray, light_gray, dark_gray, dark_gray}, white}},
        {"_NSDrawGroove", {{min_x, max_y, min_x, max_y, max_x, min_y, max_x, min_y}, {dark_gray, dark_gray, white, white, white, white, dark_gray, dark_gray}, light_gray}},
    };
    for (const auto& [symbol, bezel] : bezels)
        both(symbol, [tile, fill, set_gray, bezel](CpuState& s, GuestMemory&) {
            // (rect d0..d3, clip d4..d7)
            const Rect bounds{{d(s, 0), d(s, 1)}, {d(s, 2), d(s, 3)}}, clip{{d(s, 4), d(s, 5)}, {d(s, 6), d(s, 7)}};
            const auto rest = tile(bounds, clip, bezel.sides, [&](std::size_t index) { set_gray(bezel.grays[index]); });
            set_gray(bezel.fill);
            const auto x0 = std::max(rest.origin.x, clip.origin.x), y0 = std::max(rest.origin.y, clip.origin.y);
            const auto x1 = std::min(rest.origin.x + rest.size.width, clip.origin.x + clip.size.width);
            const auto y1 = std::min(rest.origin.y + rest.size.height, clip.origin.y + clip.size.height);
            if (x1 > x0 && y1 > y0) fill({{x0, y0}, {x1 - x0, y1 - y0}});
        });

    // ---- other drawing ----
    both("_NSDrawWindowBackground", [k, fill](CpuState& s, GuestMemory&) {
        static_cast<void>(k->send(k->send(k->color, "windowBackgroundColor").x0, "set"));
        fill(rect_arg(s));
    });
    both("_NSHighlightRect", [k, fill](CpuState& s, GuestMemory&) {
        static_cast<void>(k->send(k->send(k->send(k->color, "selectedTextBackgroundColor").x0, "colorWithAlphaComponent:", [] {
            CallRegs r; r.set_d(0, 0.5); return r; }()).x0, "set"));
        fill(rect_arg(s), 2);   // NSCompositingOperationSourceOver
    });
    // A one-point dotted frame in the current color (the classic focus rectangle).
    both("_NSDottedFrameRect", [fill](CpuState& s, GuestMemory&) {
        const auto r = rect_arg(s);
        const auto width = std::min(r.size.width, 4096.0), height = std::min(r.size.height, 4096.0);
        for (double x = 0; x < width; x += 2) {
            fill({{r.origin.x + x, r.origin.y}, {1, 1}});
            fill({{r.origin.x + x, r.origin.y + height - 1}, {1, 1}});
        }
        for (double y = 2; y < height - 1; y += 2) {
            fill({{r.origin.x, r.origin.y + y}, {1, 1}});
            fill({{r.origin.x + width - 1, r.origin.y + y}, {1, 1}});
        }
    });
    // (frame d0..d3, nine images x0..x7 and the stack's first slot, operation (stack +8),
    // alpha d4, flipped (stack +16)): corners at their size, edges and middle stretched.
    both("_NSDrawNinePartImage", [k](CpuState& s, GuestMemory& m) {
        const Rect frame{{d(s, 0), d(s, 1)}, {d(s, 2), d(s, 3)}};
        std::array<Address, 9> images{};
        for (unsigned index = 0; index < 8; ++index) images[index] = s.x[index];
        images[8] = m.read64(s.sp);
        const auto operation = m.read64(s.sp + 8);
        const auto alpha = d(s, 4);
        const bool flip = (m.read64(s.sp + 16) & 0xff) != 0;
        const auto size_of = [k](Address picture) { return picture ? result_size(k->send(picture, "size")) : Size{}; };
        // Rows top to bottom: images 0-2, 3-5, 6-8 (top is the larger y unless flipped).
        const auto left = std::max(size_of(images[0]).width, size_of(images[6]).width);
        const auto right = std::max(size_of(images[2]).width, size_of(images[8]).width);
        const auto top = std::max(size_of(images[0]).height, size_of(images[2]).height);
        const auto bottom = std::max(size_of(images[6]).height, size_of(images[8]).height);
        const std::array<double, 3> xs{frame.origin.x, frame.origin.x + left, frame.origin.x + frame.size.width - right};
        const std::array<double, 3> widths{left, std::max(0.0, frame.size.width - left - right), right};
        const double high_y = frame.origin.y + frame.size.height - top, low_y = frame.origin.y;
        const std::array<double, 3> ys{flip ? low_y : high_y, frame.origin.y + (flip ? top : bottom), flip ? frame.origin.y + frame.size.height - bottom : low_y};
        const std::array<double, 3> heights{top, std::max(0.0, frame.size.height - top - bottom), bottom};
        for (unsigned row = 0; row < 3; ++row)
            for (unsigned column = 0; column < 3; ++column) {
                const auto picture = images[row * 3 + column];
                if (!picture || widths[column] <= 0 || heights[row] <= 0) continue;
                CallRegs registers;
                put_rect(registers, 0, {{xs[column], ys[row]}, {widths[column], heights[row]}});
                put_rect(registers, 4, {});
                registers.set_d(8, alpha);
                registers.x[2] = operation;
                registers.x[3] = 1;   // respectFlipped
                registers.x[4] = 0;
                static_cast<void>(k->send(picture, "drawInRect:fromRect:operation:fraction:respectFlipped:hints:", registers));
            }
    });
    // (rect d0..d3, width x0, height x1, bitsPerSample x2, samplesPerPixel x3,
    // bitsPerPixel x4, bytesPerRow x5, planar x6, alpha x7, colorSpaceName and
    // data planes on the stack): a bitmap image rep of the data, drawn in the rect.
    both("_NSDrawBitmap", [k](CpuState& s, GuestMemory& m) {
        CallRegs init;
        init.x[2] = m.read64(s.sp + 8);   // const unsigned char *const data[5]
        init.x[3] = s.x[0];
        init.x[4] = s.x[1];
        init.x[5] = s.x[2];
        init.x[6] = s.x[3];
        init.x[7] = s.x[7] & 0xff;
        // The rest of -initWithBitmapDataPlanes:...'s arguments go on the stack.
        init.push_stack(static_cast<std::uint8_t>(s.x[6] & 0xff));   // isPlanar
        init.push_stack(m.read64(s.sp));                            // colorSpaceName
        init.push_stack(static_cast<std::int64_t>(s.x[5]));          // bytesPerRow
        init.push_stack(static_cast<std::int64_t>(s.x[4]));          // bitsPerPixel
        const auto rep = k->send(k->send(k->objc.find_class("NSBitmapImageRep"), "alloc").x0,
                                 "initWithBitmapDataPlanes:pixelsWide:pixelsHigh:bitsPerSample:samplesPerPixel:hasAlpha:isPlanar:colorSpaceName:bytesPerRow:bitsPerPixel:",
                                 init).x0;
        if (!rep) fail("NSDrawBitmap: the bitmap data was not accepted");
        CallRegs draw;
        put_rect(draw, 0, rect_arg(s));
        static_cast<void>(k->send(rep, "drawInRect:", draw));
        k->release(rep);
    });
    // The current bitmap context's pixel at a user-space point (nil outside the context).
    both("_NSReadPixel", [k, call, current_context](CpuState& s, GuestMemory& m) {
        const auto context = current_context();
        const auto cg = context ? k->send(context, "CGContext").x0 : 0;
        const auto data = cg ? k->guest_call(core_graphics_library, "_CGBitmapContextGetData", {cg}) : 0;
        if (!data) { s.x[0] = 0; return; }
        CallRegs convert;
        convert.x[0] = cg;
        convert.set_d(0, d(s, 0));
        convert.set_d(1, d(s, 1));
        const auto device = call(core_graphics_library, "_CGContextConvertPointToDeviceSpace", convert);
        const auto width = k->guest_call(core_graphics_library, "_CGBitmapContextGetWidth", {cg});
        const auto height = k->guest_call(core_graphics_library, "_CGBitmapContextGetHeight", {cg});
        const auto row_bytes = k->guest_call(core_graphics_library, "_CGBitmapContextGetBytesPerRow", {cg});
        const auto bits = k->guest_call(core_graphics_library, "_CGBitmapContextGetBitsPerPixel", {cg});
        const auto info = k->guest_call(core_graphics_library, "_CGBitmapContextGetBitmapInfo", {cg});
        const auto x = static_cast<std::int64_t>(std::floor(device.d(0))), y = static_cast<std::int64_t>(std::floor(device.d(1)));
        if (bits != 32 || x < 0 || y < 0 || x >= static_cast<std::int64_t>(width) || y >= static_cast<std::int64_t>(height)) { s.x[0] = 0; return; }
        // Device space has y up; rows are stored top first.
        const auto at = data + static_cast<Address>(static_cast<std::int64_t>(height) - 1 - y) * row_bytes + static_cast<Address>(x) * 4;
        std::array<std::uint8_t, 4> px{};
        m.read_bytes(at, px);
        // kCGBitmapByteOrder32Little with alpha first: B G R A in memory; otherwise R G B A.
        const bool bgra = (info & 0x7000) == 0x2000;
        double r = px[bgra ? 2 : 0] / 255.0, g = px[1] / 255.0, b = px[bgra ? 0 : 2] / 255.0;
        const double a = px[3] / 255.0;
        if (a > 0) { r /= a; g /= a; b /= a; }   // premultiplied
        CallRegs make;
        make.set_d(0, std::min(r, 1.0)); make.set_d(1, std::min(g, 1.0)); make.set_d(2, std::min(b, 1.0)); make.set_d(3, a);
        s.x[0] = k->send(k->color, "colorWithSRGBRed:green:blue:alpha:", make).x0;
    });
    both("_NSCopyBits", [](CpuState&, GuestMemory&) { unsupported("NSCopyBits (no window server bitmaps to copy from)"); });
    both("_NSGetWindowServerMemory", [](CpuState&, GuestMemory&) { unsupported("NSGetWindowServerMemory (no window server memory accounting)"); });
    both("_NSDisableScreenUpdates", [](CpuState&, GuestMemory&) { ++screen_update_holds; });
    both("_NSEnableScreenUpdates", [k](CpuState&, GuestMemory&) {
        if (screen_update_holds.load() <= 0 || --screen_update_holds > 0) return;
        std::vector<Address> windows;
        {
            std::lock_guard lock(k->mutex);
            windows = k->window_order;
        }
        for (const auto value : windows) {
            const auto state = k->window_state(value);
            if (state->visible && state->backing) k->present_window(value, {{0, 0}, state->backing->points});
        }
    });

    // ---- windows ----
    const auto on_screen = [k] {
        std::vector<Address> list;
        {
            std::lock_guard lock(k->mutex);
            list = k->window_order;   // front to back
        }
        std::vector<Address> visible;
        for (const auto value : list)
            if (const auto state = k->window_state(value); state->visible && state->id && !state->closed) visible.push_back(value);
        return visible;
    };
    for (const auto* name : {"_NSCountWindows", "_NSCountWindowsForContext"}) {
        const bool for_context = std::string_view(name) == "_NSCountWindowsForContext";
        both(name, [on_screen, for_context](CpuState& s, GuestMemory& m) {
            const auto out = for_context ? s.x[1] : s.x[0];
            if (out) m.write64(out, on_screen().size());
        });
    }
    for (const auto* name : {"_NSWindowList", "_NSWindowListForContext"}) {
        const bool for_context = std::string_view(name) == "_NSWindowListForContext";
        both(name, [k, on_screen, for_context](CpuState& s, GuestMemory& m) {
            const auto size = for_context ? s.x[1] : s.x[0];
            const auto out = for_context ? s.x[2] : s.x[1];
            const auto list = on_screen();
            for (std::size_t index = 0; index < std::min<std::size_t>(list.size(), size) && out; ++index)
                m.write64(out + index * 8, k->send(list[index], "windowNumber").x0);
        });
    }

    // ---- depths ----
    both("_NSBestDepth", [k](CpuState& s, GuestMemory& m) {
        // (colorSpaceName, bitsPerSample, bitsPerPixel, planar, exactMatch*)
        const auto bps = s.x[1];
        const auto name = s.x[0] ? k->cf.utf8(s.x[0]) : std::string("NSCalibratedRGBColorSpace");
        const std::uint32_t depth = bps <= 8 ? 0x208 : bps <= 16 ? 0x210 : 0x220;
        const bool rgb = name.find("RGB") != std::string::npos;
        if (s.x[4]) m.write8(s.x[4], rgb && (bps == 8 || bps == 16 || bps == 32) && !(s.x[3] & 0xff) ? 1 : 0);
        s.x[0] = depth;
    });
    both("_NSPlanarFromDepth", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    both("_NSBitsPerSampleFromDepth", [](CpuState& s, GuestMemory&) { s.x[0] = static_cast<std::uint32_t>(s.x[0]) & 0xff; });
    both("_NSColorSpaceFromDepth", [k](CpuState& s, GuestMemory&) {
        s.x[0] = (static_cast<std::uint32_t>(s.x[0]) >> 8) == 2 ? k->auto_string("NSCalibratedRGBColorSpace") : k->auto_string("NSCalibratedWhiteColorSpace");
    });
    both("_NSNumberOfColorComponents", [k](CpuState& s, GuestMemory&) {
        const auto name = s.x[0] ? k->cf.utf8(s.x[0]) : std::string();
        s.x[0] = name.find("RGB") != std::string::npos ? 3 : name.find("CMYK") != std::string::npos ? 4
               : name.find("White") != std::string::npos || name.find("Black") != std::string::npos ? 1 : 0;
    });
    static Address available_depths{};
    both("_NSAvailableWindowDepths", [k](CpuState& s, GuestMemory& m) {
        if (!available_depths) {
            available_depths = k->process.allocate_guest(16);
            for (const auto& [offset, value] : std::array<std::pair<unsigned, std::uint32_t>, 4>{{{0, 0x208}, {4, 0x210}, {8, 0x220}, {12, 0}}})
                m.write32(available_depths + offset, value);
        }
        s.x[0] = available_depths;
    });

    // ---- alerts ----
    // The message: the printf-style format with the variadic arguments (on the stack).
    const auto message_of = [k](Address format, Address arguments) -> Address {
        if (!format) return 0;
        const auto made = k->guest_call(core_foundation_library, "_CFStringCreateWithFormatAndArguments", {0, 0, format, arguments});
        return made ? k->autorelease(made) : 0;
    };
    // An NSAlert with the legacy buttons (default, alternate, other); returns it.
    const auto legacy_alert = [k](Address title, Address message, Address default_button, Address alternate, Address other, std::uint64_t style) {
        const auto alert = k->autorelease(k->send(k->send(k->alert, "alloc").x0, "init").x0);
        if (title) static_cast<void>(k->send_id(alert, "setMessageText:", {title}));
        if (message) static_cast<void>(k->send_id(alert, "setInformativeText:", {message}));
        static_cast<void>(k->send_id(alert, "setAlertStyle:", {style}));
        static_cast<void>(k->send_id(alert, "addButtonWithTitle:", {default_button ? default_button : k->auto_string("OK")}));
        if (alternate) static_cast<void>(k->send_id(alert, "addButtonWithTitle:", {alternate}));
        if (other) static_cast<void>(k->send_id(alert, "addButtonWithTitle:", {other}));
        return alert;
    };
    // NSAlertFirstButtonReturn 1000 -> NSAlertDefaultReturn 1, 1001 -> NSAlertAlternateReturn 0, 1002 -> NSAlertOtherReturn -1.
    const auto legacy_answer = [](std::int64_t response) -> std::int64_t {
        return response == 1000 ? 1 : response == 1001 ? 0 : response == 1002 ? -1 : -2;
    };
    for (const auto& [name, style] : std::initializer_list<std::pair<const char*, std::uint64_t>>{
             {"_NSRunAlertPanel", 1}, {"_NSRunInformationalAlertPanel", 1}, {"_NSRunCriticalAlertPanel", 2}})
        both(name, [k, message_of, legacy_alert, legacy_answer, style](CpuState& s, GuestMemory&) {
            // (title, msgFormat, defaultButton, alternateButton, otherButton, ...)
            const auto alert = legacy_alert(s.x[0], message_of(s.x[1], s.sp), s.x[2], s.x[3], s.x[4], style);
            s.x[0] = static_cast<Address>(legacy_answer(static_cast<std::int64_t>(k->send(alert, "runModal").x0)));
        });
    // (title, default, alternate, other, docWindow, modalDelegate, didEndSelector,
    // didDismissSelector, contextInfo (stack), msgFormat (stack), ...)
    both("_NSBeginAlertSheet", [k, message_of, legacy_alert](CpuState& s, GuestMemory& m) {
        const auto context_info = m.read64(s.sp), format = m.read64(s.sp + 8);
        const auto alert = legacy_alert(s.x[0], message_of(format, s.sp + 16), s.x[1], s.x[2], s.x[3], 1);
        if (!s.x[4]) {
            static_cast<void>(k->send(alert, "runModal"));
            return;
        }
        CallRegs registers;
        registers.x[2] = s.x[4];
        registers.x[3] = s.x[5];
        registers.x[4] = s.x[6];
        registers.x[5] = context_info;
        static_cast<void>(k->send(alert, "beginSheetModalForWindow:modalDelegate:didEndSelector:contextInfo:", registers));
    });
    both("_NSGetAlertPanel", [](CpuState&, GuestMemory&) {
        unsupported("NSGetAlertPanel (alerts are host dialogs, not panels; NSRunAlertPanel and NSAlert run them)");
    });
    both("_NSReleaseAlertPanel", [k](CpuState& s, GuestMemory&) { k->release(s.x[0]); });

    // ---- accessibility ----
    both("_NSAccessibilityRoleDescriptionForUIElement", [k](CpuState& s, GuestMemory&) {
        const auto element = s.x[0];
        if (element && k->responds(element, "accessibilityRoleDescription")) { s.x[0] = k->send(element, "accessibilityRoleDescription").x0; return; }
        const auto role = element && k->responds(element, "accessibilityRole") ? k->send(element, "accessibilityRole").x0 : 0;
        s.x[0] = role ? k->guest_call(appkit_library, "_NSAccessibilityRoleDescription", {role, 0}) : 0;
    });
    // A view's rect / point in screen coordinates.
    both("_NSAccessibilityFrameInView", [k](CpuState& s, GuestMemory&) {
        const auto view_object = s.x[0];
        CallRegs registers;
        put_rect(registers, 0, rect_arg(s));
        registers.x[2] = 0;
        auto in_window = result_rect(k->send(view_object, "convertRect:toView:", registers));
        const auto owner = k->send(view_object, "window").x0;
        if (owner) {
            CallRegs screen;
            put_rect(screen, 0, in_window);
            in_window = result_rect(k->send(owner, "convertRectToScreen:", screen));
        }
        return_rect(s, in_window);
    });
    both("_NSAccessibilityPointInView", [k](CpuState& s, GuestMemory&) {
        const auto view_object = s.x[0];
        CallRegs registers;
        put_point(registers, 0, {d(s, 0), d(s, 1)});
        registers.x[2] = 0;
        const auto in_window = k->send(view_object, "convertPoint:toView:", registers);
        Point point{in_window.d(0), in_window.d(1)};
        if (const auto owner = k->send(view_object, "window").x0) {
            CallRegs screen;
            put_rect(screen, 0, {point, {0, 0}});
            const auto moved = result_rect(k->send(owner, "convertRectToScreen:", screen));
            point = moved.origin;
        }
        set_d(s, 0, point.x);
        set_d(s, 1, point.y);
    });
    static std::atomic<bool> protected_content{};
    both("_NSAccessibilitySetMayContainProtectedContent", [](CpuState& s, GuestMemory&) {
        protected_content = (s.x[0] & 0xff) != 0;
        s.x[0] = 1;
    });

    // ---- services ----
    struct Services {
        std::mutex mutex;
        std::map<std::string, bool> shown;
    };
    static Services services;
    both("_NSRegisterServicesProvider", [k](CpuState& s, GuestMemory&) {
        if (k->app) static_cast<void>(k->send_id(k->app, "setServicesProvider:", {s.x[0]}));
    });
    both("_NSUnregisterServicesProvider", [k](CpuState&, GuestMemory&) {
        if (k->app) static_cast<void>(k->send_id(k->app, "setServicesProvider:", {0}));
    });
    // There is no Services menu to rebuild.
    both("_NSUpdateDynamicServices", [](CpuState&, GuestMemory&) {});
    both("_NSShowsServicesMenuItem", [k](CpuState& s, GuestMemory&) {
        const auto name = s.x[0] ? k->cf.utf8(s.x[0]) : std::string();
        std::lock_guard lock(services.mutex);
        const auto found = services.shown.find(name);
        s.x[0] = found != services.shown.end() && found->second ? 1 : 0;
    });
    both("_NSSetShowsServicesMenuItem", [k](CpuState& s, GuestMemory&) {
        const auto name = s.x[0] ? k->cf.utf8(s.x[0]) : std::string();
        std::lock_guard lock(services.mutex);
        services.shown[name] = (s.x[1] & 0xff) != 0;
        s.x[0] = 0;
    });

    // ---- pasteboard types ----
    both("_NSCreateFileContentsPboardType", [k](CpuState& s, GuestMemory&) {
        s.x[0] = k->string(std::string(typed_contents) + (s.x[0] ? k->cf.utf8(s.x[0]) : std::string()));
    });
    both("_NSCreateFilenamePboardType", [k](CpuState& s, GuestMemory&) {
        s.x[0] = k->string(std::string(typed_filenames) + (s.x[0] ? k->cf.utf8(s.x[0]) : std::string()));
    });
    const auto file_type = [](const std::string& type) -> std::optional<std::string> {
        for (const auto prefix : {typed_contents, typed_filenames})
            if (type.starts_with(prefix)) return type.substr(prefix.size());
        return std::nullopt;
    };
    both("_NSGetFileType", [k, file_type](CpuState& s, GuestMemory&) {
        const auto type = s.x[0] ? file_type(k->cf.utf8(s.x[0])) : std::nullopt;
        s.x[0] = type ? k->auto_string(*type) : 0;
    });
    both("_NSGetFileTypes", [k, file_type](CpuState& s, GuestMemory&) {
        std::vector<Address> types;
        for (const auto value : s.x[0] ? k->cf.array_values(s.x[0]) : std::vector<Address>{})
            if (const auto type = file_type(k->cf.utf8(value))) types.push_back(k->auto_string(*type));
        s.x[0] = types.empty() ? 0 : k->auto_array(types);
    });

    // ---- the rest ----
    // (effect, centerLocation d0 d1, size d2 d3, animationDelegate, didEndSelector, contextInfo)
    both("_NSShowAnimationEffect", [k](CpuState& s, GuestMemory&) {
        const auto delegate = s.x[1], selector = s.x[2], context_info = s.x[3];
        if (!delegate || !selector) return;
        CallRegs registers;
        registers.x[2] = context_info;
        static_cast<void>(k->objc.send(delegate, selector, registers));
    });
    both("_NSInterfaceStyleForKey", [](CpuState& s, GuestMemory&) { s.x[0] = 3; });   // NSMacintoshInterfaceStyle
    both("_NSTouchTypeMaskFromType", [](CpuState& s, GuestMemory&) { s.x[0] = 1ULL << (s.x[0] & 63); });
}

} // namespace alchemy::gui::appkit
