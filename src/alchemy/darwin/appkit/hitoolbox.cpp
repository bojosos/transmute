#include "alchemy/darwin/appkit/controls_internal.hpp"

#include <algorithm>
#include <cmath>

// HIToolbox pieces Cocoa programs still call (wxWidgets' wxOSX port among
// them), on the GUI build (they replace the headless shells):
//  * HIShape: regions as sets of disjoint rectangles (wxRegion, paint update
//    regions, clipping through HIShapeReplacePathInCGContext);
//  * HITheme drawing into a CGContext with AppKit's flat theme: buttons
//    (push, check box, radio, bevel, popup, combo box arrow, list header,
//    disclosure, arrow, increment/decrement), focus rings, text-field and
//    list-box frames, pane splitters;
//  * GetCurrentKeyModifiers from the window server's modifier state;
//  * GetThemeMetric for the basic control metrics (scroll bar, check box,
//    radio button, text frames, focus ring); other metrics fail explicitly.
// HIThemeDrawTrack and HIThemeBrushCreateCGColor are not provided here.
namespace alchemy::gui::appkit {
namespace {
using drawing::Color;

constexpr std::string_view application_services_library =
    "/System/Library/Frameworks/ApplicationServices.framework/Versions/A/ApplicationServices";
constexpr std::uint32_t no_err = 0, param_err = static_cast<std::uint32_t>(-50);

// ---- regions ---------------------------------------------------------------------------
struct ShapeState final : ObjectPayload {
    std::vector<Rect> rects;      // disjoint, non-empty
    bool mutable_shape{};
};

Rect normalized(Rect r) {
    if (r.size.width < 0) { r.origin.x += r.size.width; r.size.width = -r.size.width; }
    if (r.size.height < 0) { r.origin.y += r.size.height; r.size.height = -r.size.height; }
    return r;
}
bool empty(const Rect& r) { return !(r.size.width > 0 && r.size.height > 0) || std::isnan(r.origin.x) || std::isinf(r.origin.x); }
std::optional<Rect> intersection(const Rect& a, const Rect& b) {
    const auto x0 = std::max(a.origin.x, b.origin.x), y0 = std::max(a.origin.y, b.origin.y);
    const auto x1 = std::min(a.origin.x + a.size.width, b.origin.x + b.size.width);
    const auto y1 = std::min(a.origin.y + a.size.height, b.origin.y + b.size.height);
    if (x1 <= x0 || y1 <= y0) return std::nullopt;
    return Rect{{x0, y0}, {x1 - x0, y1 - y0}};
}
// a minus b: up to four disjoint pieces.
void subtract(const Rect& a, const Rect& b, std::vector<Rect>& out) {
    const auto overlap = intersection(a, b);
    if (!overlap) { out.push_back(a); return; }
    const auto ax1 = a.origin.x + a.size.width, ay1 = a.origin.y + a.size.height;
    const auto ox1 = overlap->origin.x + overlap->size.width, oy1 = overlap->origin.y + overlap->size.height;
    if (overlap->origin.y > a.origin.y) out.push_back({a.origin, {a.size.width, overlap->origin.y - a.origin.y}});
    if (oy1 < ay1) out.push_back({{a.origin.x, oy1}, {a.size.width, ay1 - oy1}});
    if (overlap->origin.x > a.origin.x) out.push_back({{a.origin.x, overlap->origin.y}, {overlap->origin.x - a.origin.x, overlap->size.height}});
    if (ox1 < ax1) out.push_back({{ox1, overlap->origin.y}, {ax1 - ox1, overlap->size.height}});
}
std::vector<Rect> difference(const std::vector<Rect>& a, const std::vector<Rect>& b) {
    auto current = a;
    for (const auto& cut : b) {
        std::vector<Rect> next;
        for (const auto& piece : current) subtract(piece, cut, next);
        current = std::move(next);
    }
    return current;
}
// Merges neighbours that share a full edge, so simple unions stay small and
// rectangular shapes become one rectangle.
void coalesce(std::vector<Rect>& rects) {
    for (bool merged = true; merged;) {
        merged = false;
        for (std::size_t i = 0; i < rects.size() && !merged; ++i)
            for (std::size_t j = i + 1; j < rects.size() && !merged; ++j) {
                auto& a = rects[i];
                const auto& b = rects[j];
                if (a.origin.x == b.origin.x && a.size.width == b.size.width &&
                    (a.origin.y + a.size.height == b.origin.y || b.origin.y + b.size.height == a.origin.y)) {
                    a = {{a.origin.x, std::min(a.origin.y, b.origin.y)}, {a.size.width, a.size.height + b.size.height}};
                    merged = true;
                } else if (a.origin.y == b.origin.y && a.size.height == b.size.height &&
                           (a.origin.x + a.size.width == b.origin.x || b.origin.x + b.size.width == a.origin.x)) {
                    a = {{std::min(a.origin.x, b.origin.x), a.origin.y}, {a.size.width + b.size.width, a.size.height}};
                    merged = true;
                }
                if (merged) rects.erase(rects.begin() + static_cast<std::ptrdiff_t>(j));
            }
    }
}
std::vector<Rect> united(const std::vector<Rect>& a, const std::vector<Rect>& b) {
    auto result = a;
    for (const auto& piece : difference(b, a)) result.push_back(piece);
    coalesce(result);
    return result;
}
std::vector<Rect> intersected(const std::vector<Rect>& a, const std::vector<Rect>& b) {
    std::vector<Rect> result;
    for (const auto& x : a)
        for (const auto& y : b)
            if (const auto overlap = intersection(x, y)) result.push_back(*overlap);
    coalesce(result);
    return result;
}
Rect bounds_of(const std::vector<Rect>& rects) {
    if (rects.empty()) return {};
    auto x0 = rects[0].origin.x, y0 = rects[0].origin.y;
    auto x1 = x0 + rects[0].size.width, y1 = y0 + rects[0].size.height;
    for (const auto& r : rects) {
        x0 = std::min(x0, r.origin.x); y0 = std::min(y0, r.origin.y);
        x1 = std::max(x1, r.origin.x + r.size.width); y1 = std::max(y1, r.origin.y + r.size.height);
    }
    return {{x0, y0}, {x1 - x0, y1 - y0}};
}

// A rounded control face with a one-pixel border on `canvas`.
void bezel(drawing::Canvas& canvas, Rect rect, double radius, Color face, Color border) {
    drawing::Path outer;
    outer.add_rounded_rect(rect, radius, radius);
    canvas.fill_path(outer, border);
    const Rect inner_rect{{rect.origin.x + 1, rect.origin.y + 1}, {std::max(0.0, rect.size.width - 2), std::max(0.0, rect.size.height - 2)}};
    drawing::Path inner;
    inner.add_rounded_rect(inner_rect, std::max(0.0, radius - 1), std::max(0.0, radius - 1));
    canvas.fill_path(inner, face);
}

std::uint16_t read16(GuestMemory& m, Address at) {
    return static_cast<std::uint16_t>(m.read8(at) | (m.read8(at + 1) << 8));
}
Rect read_rect(GuestMemory& m, Address at) {
    return {{std::bit_cast<double>(m.read64(at)), std::bit_cast<double>(m.read64(at + 8))},
            {std::bit_cast<double>(m.read64(at + 16)), std::bit_cast<double>(m.read64(at + 24))}};
}
void write_rect(GuestMemory& m, Address at, Rect r) {
    m.write64(at, std::bit_cast<std::uint64_t>(r.origin.x));
    m.write64(at + 8, std::bit_cast<std::uint64_t>(r.origin.y));
    m.write64(at + 16, std::bit_cast<std::uint64_t>(r.size.width));
    m.write64(at + 24, std::bit_cast<std::uint64_t>(r.size.height));
}

// ---- HITheme ---------------------------------------------------------------------------
// ThemeButtonKind
namespace kind {
inline constexpr std::uint32_t push = 0, check_box = 1, radio = 2, bevel = 3, arrow = 4, popup = 5, disclosure = 6,
    inc_dec = 7, small_bevel = 8, large_bevel = 9, list_header = 10, round = 11, large_round = 12, small_check_box = 13,
    small_radio = 14, rounded_bevel = 15, combo_box = 16, combo_box_small = 17, combo_box_mini = 18, mini_check_box = 19,
    mini_radio = 20, push_small = 21, push_mini = 22;
}
// ThemeDrawState
namespace draw_state {
inline constexpr std::uint32_t inactive = 0, active = 1, pressed = 2, unavailable = 7, unavailable_inactive = 8;
}
constexpr std::uint32_t adornment_default = 1U << 0, adornment_focus = 1U << 2, adornment_header_sort_up = 1U << 9;
} // namespace

void install_hitoolbox(AppKit& kit);

void install_hitoolbox(AppKit& kit) {
    // HIShape objects: instances of a host class (CFRetain / CFRelease /
    // CFGetTypeID work as on any object).
    auto shape_class = kit.objc.find_class("_AlchemyHIShape");
    if (!shape_class) shape_class = kit.objc.register_native_class("_AlchemyHIShape", kit.object_class, 8);
    const auto shape = [&kit](Address value) -> std::shared_ptr<ShapeState> {
        if (!value) fail("HIShape: NULL shape");
        const auto state = kit.objc.find_host_state<ShapeState>(value);
        if (!state) fail("HIShape: not an HIShapeRef");
        return state;
    };
    const auto make = [&kit, shape_class](std::vector<Rect> rects, bool mutable_shape) {
        const auto made = kit.alloc_init(shape_class);
        auto state = kit.objc.host_state<ShapeState>(made);
        state->rects = std::move(rects);
        state->mutable_shape = mutable_shape;
        return made;
    };
    const auto writable = [shape](Address value) {
        const auto state = shape(value);
        if (!state->mutable_shape) fail("HIShape: the destination is not an HIMutableShapeRef");
        return state;
    };
    for (const auto library : {application_services_library, carbon_library}) {
        kit.function(library, "_HIShapeCreateEmpty", [make](CpuState& s, GuestMemory&) { s.x[0] = make({}, false); });
        kit.function(library, "_HIShapeCreateMutable", [make](CpuState& s, GuestMemory&) { s.x[0] = make({}, true); });
        kit.function(library, "_HIShapeCreateWithRect", [make](CpuState& s, GuestMemory& m) {
            if (!s.x[0]) { s.x[0] = 0; return; }
            const auto r = normalized(read_rect(m, s.x[0]));
            s.x[0] = make(empty(r) ? std::vector<Rect>{} : std::vector<Rect>{r}, false);
        });
        kit.function(library, "_HIShapeCreateCopy", [make, shape](CpuState& s, GuestMemory&) { s.x[0] = make(shape(s.x[0])->rects, false); });
        kit.function(library, "_HIShapeCreateMutableCopy", [make, shape](CpuState& s, GuestMemory&) { s.x[0] = make(shape(s.x[0])->rects, true); });
        kit.function(library, "_HIShapeCreateMutableWithRect", [make](CpuState& s, GuestMemory& m) {
            const auto r = s.x[0] ? normalized(read_rect(m, s.x[0])) : Rect{};
            s.x[0] = make(empty(r) ? std::vector<Rect>{} : std::vector<Rect>{r}, true);
        });
        kit.function(library, "_HIShapeCreateIntersection", [make, shape](CpuState& s, GuestMemory&) {
            s.x[0] = make(intersected(shape(s.x[0])->rects, shape(s.x[1])->rects), false);
        });
        kit.function(library, "_HIShapeCreateUnion", [make, shape](CpuState& s, GuestMemory&) {
            s.x[0] = make(united(shape(s.x[0])->rects, shape(s.x[1])->rects), false);
        });
        kit.function(library, "_HIShapeCreateDifference", [make, shape](CpuState& s, GuestMemory&) {
            auto rects = difference(shape(s.x[0])->rects, shape(s.x[1])->rects);
            coalesce(rects);
            s.x[0] = make(std::move(rects), false);
        });
        kit.function(library, "_HIShapeCreateXor", [make, shape](CpuState& s, GuestMemory&) {
            const auto& a = shape(s.x[0])->rects;
            const auto& b = shape(s.x[1])->rects;
            auto rects = difference(a, b);
            for (const auto& piece : difference(b, a)) rects.push_back(piece);
            coalesce(rects);
            s.x[0] = make(std::move(rects), false);
        });
        // In-place operations write the result into the mutable destination
        // (which may be one of the operands).
        kit.function(library, "_HIShapeIntersect", [shape, writable](CpuState& s, GuestMemory&) {
            auto result = intersected(shape(s.x[0])->rects, shape(s.x[1])->rects);
            writable(s.x[2])->rects = std::move(result);
            s.x[0] = no_err;
        });
        kit.function(library, "_HIShapeUnion", [shape, writable](CpuState& s, GuestMemory&) {
            auto result = united(shape(s.x[0])->rects, shape(s.x[1])->rects);
            writable(s.x[2])->rects = std::move(result);
            s.x[0] = no_err;
        });
        kit.function(library, "_HIShapeDifference", [shape, writable](CpuState& s, GuestMemory&) {
            auto result = difference(shape(s.x[0])->rects, shape(s.x[1])->rects);
            coalesce(result);
            writable(s.x[2])->rects = std::move(result);
            s.x[0] = no_err;
        });
        kit.function(library, "_HIShapeXor", [shape, writable](CpuState& s, GuestMemory&) {
            const auto a = shape(s.x[0])->rects, b = shape(s.x[1])->rects;
            auto result = difference(a, b);
            for (const auto& piece : difference(b, a)) result.push_back(piece);
            coalesce(result);
            writable(s.x[2])->rects = std::move(result);
            s.x[0] = no_err;
        });
        kit.function(library, "_HIShapeUnionWithRect", [writable](CpuState& s, GuestMemory& m) {
            if (!s.x[1]) { s.x[0] = param_err; return; }
            const auto r = normalized(read_rect(m, s.x[1]));
            const auto state = writable(s.x[0]);
            if (!empty(r)) state->rects = united(state->rects, {r});
            s.x[0] = no_err;
        });
        kit.function(library, "_HIShapeSetEmpty", [writable](CpuState& s, GuestMemory&) { writable(s.x[0])->rects.clear(); s.x[0] = no_err; });
        kit.function(library, "_HIShapeSetWithShape", [shape, writable](CpuState& s, GuestMemory&) {
            auto rects = shape(s.x[1])->rects;
            writable(s.x[0])->rects = std::move(rects);
            s.x[0] = no_err;
        });
        kit.function(library, "_HIShapeOffset", [writable](CpuState& s, GuestMemory&) {
            const auto dx = d(s, 0), dy = d(s, 1);
            for (auto& r : writable(s.x[0])->rects) { r.origin.x += dx; r.origin.y += dy; }
            s.x[0] = no_err;
        });
        kit.function(library, "_HIShapeInset", [writable](CpuState& s, GuestMemory&) {
            const auto state = writable(s.x[0]);
            if (state->rects.size() > 1) unsupported("HIShapeInset of a non-rectangular shape");
            if (!state->rects.empty()) {
                auto& r = state->rects.front();
                r = {{r.origin.x + d(s, 0), r.origin.y + d(s, 1)}, {r.size.width - 2 * d(s, 0), r.size.height - 2 * d(s, 1)}};
                if (empty(r)) state->rects.clear();
            }
            s.x[0] = no_err;
        });
        kit.function(library, "_HIShapeIsEmpty", [shape](CpuState& s, GuestMemory&) { s.x[0] = shape(s.x[0])->rects.empty() ? 1 : 0; });
        kit.function(library, "_HIShapeIsRectangular", [shape](CpuState& s, GuestMemory&) {
            const auto& rects = shape(s.x[0])->rects;
            double area = 0;
            for (const auto& r : rects) area += r.size.width * r.size.height;
            const auto box = bounds_of(rects);
            s.x[0] = rects.size() <= 1 || std::abs(area - box.size.width * box.size.height) < 1e-9 ? 1 : 0;
        });
        kit.function(library, "_HIShapeContainsPoint", [shape](CpuState& s, GuestMemory& m) {
            if (!s.x[1]) { s.x[0] = 0; return; }
            const Point p{std::bit_cast<double>(m.read64(s.x[1])), std::bit_cast<double>(m.read64(s.x[1] + 8))};
            const auto rects = shape(s.x[0])->rects;
            s.x[0] = 0;
            for (const auto& r : rects) if (rect_contains(r, p)) { s.x[0] = 1; return; }
        });
        kit.function(library, "_HIShapeIntersectsRect", [shape](CpuState& s, GuestMemory& m) {
            if (!s.x[1]) { s.x[0] = 0; return; }
            const auto r = normalized(read_rect(m, s.x[1]));
            const auto rects = shape(s.x[0])->rects;
            s.x[0] = 0;
            for (const auto& piece : rects) if (intersection(piece, r)) { s.x[0] = 1; return; }
        });
        kit.function(library, "_HIShapeGetBounds", [shape](CpuState& s, GuestMemory& m) {
            if (!s.x[1]) { s.x[0] = 0; return; }
            write_rect(m, s.x[1], bounds_of(shape(s.x[0])->rects));
            s.x[0] = s.x[1];
        });
        // The path of the shape replaces the context's current path
        // (CGContextBeginPath + one CGContextAddRect per rectangle).
        kit.function(library, "_HIShapeReplacePathInCGContext", [&kit, shape](CpuState& s, GuestMemory&) {
            const auto rects = shape(s.x[0])->rects;
            const auto context = s.x[1];
            const auto call = [&kit](std::string_view symbol, const CallRegs& registers) {
                const auto found = kit.process.resolve(core_graphics_library, symbol);
                if (!found) unsupported("CoreGraphics " + std::string(symbol.substr(1)) + " (the CoreGraphics provider is not installed)");
                static_cast<void>(EngineAdapter(kit.process.calling_engine()).call_regs(found->address, registers));
            };
            CallRegs begin;
            begin.x[0] = context;
            call("_CGContextBeginPath", begin);
            for (const auto& r : rects) {
                CallRegs add;
                add.x[0] = context;
                put_rect(add, 0, r);
                call("_CGContextAddRect", add);
            }
            s.x[0] = no_err;
        });
        // Enumeration: kHIShapeParseFromTop (0) / FromBottom / FromLeft /
        // FromRight / FromTopLeft (default) / FromBottomRight; the callback
        // gets (message, shape, rect pointer, refcon) and returns OSStatus.
        kit.function(library, "_HIShapeEnumerate", [&kit, shape](CpuState& s, GuestMemory&) {
            const auto self = s.x[0];
            auto rects = shape(self)->rects;
            const auto options = s.x[1];
            const auto callback = s.x[2], refcon = s.x[3];
            const bool from_bottom = (options & 1) != 0, from_right = (options & 2) != 0;
            std::stable_sort(rects.begin(), rects.end(), [&](const Rect& a, const Rect& b) {
                if (a.origin.y != b.origin.y) return from_bottom ? a.origin.y > b.origin.y : a.origin.y < b.origin.y;
                return from_right ? a.origin.x > b.origin.x : a.origin.x < b.origin.x;
            });
            const auto block = kit.process.allocate_guest(32);
            std::uint32_t status = no_err;
            try {
                EngineAdapter engine(kit.process.calling_engine());
                const auto call = [&](std::uint64_t message, Address rect) {
                    const std::array<std::uint64_t, 4> arguments{message, self, rect, refcon};
                    return static_cast<std::uint32_t>(engine.call(callback, std::span<const std::uint64_t>(arguments)));
                };
                status = call(1, 0);   // kHIShapeEnumerateInit
                for (const auto& r : rects) {
                    if (status != no_err) break;
                    write_rect(kit.memory(), block, r);
                    status = call(2, block);   // kHIShapeEnumerateRect
                }
                const auto end = call(3, 0);   // kHIShapeEnumerateTerminate
                if (status == no_err) status = end;
            } catch (...) { kit.process.release_guest(block); throw; }
            kit.process.release_guest(block);
            s.x[0] = status;
        });
    }

    // ---- HITheme drawing into a CGContext ----
    const auto canvas_for = [&kit](Address context, std::string_view operation) -> drawing::Canvas& {
        if (!context) fail(std::string(operation) + ": NULL CGContextRef");
        auto* canvas = kit.canvas_over_cg(context);
        if (!canvas) unsupported(std::string(operation) + " into a context without 32-bit bitmap pixels");
        return *canvas;
    };
    const Theme* light = &theme(false);
    kit.function(carbon_library, "_HIThemeDrawButton", [canvas_for, light](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[1]) { s.x[0] = param_err; return; }
        const auto bounds = normalized(read_rect(m, s.x[0]));
        const auto info = s.x[1];
        const auto state_value = m.read32(info + 4);
        const auto kind_value = static_cast<std::uint32_t>(read16(m, info + 8));
        const auto value = static_cast<std::uint32_t>(read16(m, info + 10));
        const auto adornment = static_cast<std::uint32_t>(read16(m, info + 12));
        auto& canvas = canvas_for(s.x[2], "HIThemeDrawButton");
        const auto& th = *light;
        const bool pressed = state_value == draw_state::pressed;
        const bool disabled = state_value == draw_state::unavailable || state_value == draw_state::unavailable_inactive;
        const bool inverted = s.x[3] == 1;   // kHIThemeOrientationInverted: y up
        const auto face = disabled ? th.control_face.with_alpha(0.5) : pressed ? th.control_face_pressed : th.control_face;
        Rect label = bounds;
        canvas.save();
        const auto chevron = [&](Point center, double size, bool down, Color color) {
            const double dir = (down != inverted) ? 1 : -1;   // +y is down unless inverted
            drawing::Path mark;
            mark.move_to({center.x - size, center.y - dir * size / 2});
            mark.line_to({center.x, center.y + dir * size / 2});
            mark.line_to({center.x + size, center.y - dir * size / 2});
            canvas.stroke_path(mark, color, 1.5);
        };
        switch (kind_value) {
        case kind::check_box: case kind::small_check_box: case kind::mini_check_box:
        case kind::radio: case kind::small_radio: case kind::mini_radio: {
            const bool radio = kind_value == kind::radio || kind_value == kind::small_radio || kind_value == kind::mini_radio;
            const double side = std::min({bounds.size.width, bounds.size.height, kind_value == kind::check_box || kind_value == kind::radio ? 14.0 : 11.0});
            const Rect mark{{bounds.origin.x, bounds.origin.y + (bounds.size.height - side) / 2}, {side, side}};
            const bool on = value != 0;
            const auto fill = on && !disabled ? th.accent : th.control_background;
            bezel(canvas, mark, radio ? side / 2 : 3, pressed ? th.control_face_pressed : fill, on ? fill : th.control_border);
            if (on && radio) {
                drawing::Path dot;
                dot.add_oval({{mark.origin.x + side * 0.32, mark.origin.y + side * 0.32}, {side * 0.36, side * 0.36}});
                canvas.fill_path(dot, th.accent_text);
            } else if (value == 1) {
                const auto at = [&](double x, double y) {   // unit coordinates, y up
                    return Point{mark.origin.x + x * side, inverted ? mark.origin.y + y * side : mark.origin.y + (1 - y) * side};
                };
                drawing::Path check;
                check.move_to(at(0.22, 0.52));
                check.line_to(at(0.42, 0.30));
                check.line_to(at(0.78, 0.74));
                canvas.stroke_path(check, th.accent_text, 1.8);
            } else if (value == 2) {
                canvas.fill_rect({{mark.origin.x + side * 0.25, mark.origin.y + side / 2 - 1}, {side * 0.5, 2}}, th.accent_text);
            }
            label = {{mark.origin.x + side + 4, bounds.origin.y}, {std::max(0.0, bounds.size.width - side - 4), bounds.size.height}};
            break;
        }
        case kind::push: case kind::push_small: case kind::push_mini: case kind::round: case kind::large_round: {
            const bool is_default = (adornment & adornment_default) != 0 && !disabled;
            const bool round = kind_value == kind::round || kind_value == kind::large_round;
            const auto radius = round ? std::min(bounds.size.width, bounds.size.height) / 2 : 5.0;
            const auto fill = is_default ? (pressed ? Color{th.accent.r * 0.8, th.accent.g * 0.8, th.accent.b * 0.85, 1} : th.accent) : face;
            bezel(canvas, bounds, radius, fill, is_default ? fill : th.control_border);
            label = {{bounds.origin.x + 8, bounds.origin.y}, {std::max(0.0, bounds.size.width - 16), bounds.size.height}};
            break;
        }
        case kind::bevel: case kind::small_bevel: case kind::large_bevel: case kind::rounded_bevel: {
            bezel(canvas, bounds, kind_value == kind::rounded_bevel ? 4 : 2, value == 1 ? th.control_face_pressed : face, th.control_border);
            label = {{bounds.origin.x + 4, bounds.origin.y + 2}, {std::max(0.0, bounds.size.width - 8), std::max(0.0, bounds.size.height - 4)}};
            break;
        }
        case kind::popup: {
            bezel(canvas, bounds, 5, face, th.control_border);
            const Rect arrows{{bounds.origin.x + bounds.size.width - 18, bounds.origin.y + 2}, {16, std::max(0.0, bounds.size.height - 4)}};
            bezel(canvas, arrows, 4, disabled ? th.accent.with_alpha(0.5) : th.accent, th.accent);
            const Point center{arrows.origin.x + arrows.size.width / 2, arrows.origin.y + arrows.size.height / 2};
            chevron({center.x, center.y - 3}, 3, false, th.accent_text);
            chevron({center.x, center.y + 3}, 3, true, th.accent_text);
            label = {{bounds.origin.x + 8, bounds.origin.y}, {std::max(0.0, bounds.size.width - 30), bounds.size.height}};
            break;
        }
        case kind::combo_box: case kind::combo_box_small: case kind::combo_box_mini: {
            // The combo box's arrow button part (the field draws itself).
            bezel(canvas, bounds, 4, disabled ? th.accent.with_alpha(0.5) : th.accent, th.accent);
            chevron({bounds.origin.x + bounds.size.width / 2, bounds.origin.y + bounds.size.height / 2}, 3.5, true, th.accent_text);
            label = {};
            break;
        }
        case kind::arrow: {
            chevron({bounds.origin.x + bounds.size.width / 2, bounds.origin.y + bounds.size.height / 2}, std::max(2.0, std::min(bounds.size.width, bounds.size.height) / 4),
                    true, disabled ? th.disabled_text : th.text);
            label = {};
            break;
        }
        case kind::disclosure: {
            // kThemeDisclosureRight (0) / Down (1) / Left (2): a triangle.
            const auto side = std::min(bounds.size.width, bounds.size.height) * 0.5;
            const Point c{bounds.origin.x + bounds.size.width / 2, bounds.origin.y + bounds.size.height / 2};
            drawing::Path triangle;
            const double down = inverted ? -1 : 1;
            if (value == 1) {
                triangle.move_to({c.x - side / 2, c.y - down * side / 3});
                triangle.line_to({c.x + side / 2, c.y - down * side / 3});
                triangle.line_to({c.x, c.y + down * side / 2});
            } else {
                const double dir = value == 2 ? -1 : 1;
                triangle.move_to({c.x - dir * side / 3, c.y - side / 2});
                triangle.line_to({c.x - dir * side / 3, c.y + side / 2});
                triangle.line_to({c.x + dir * side / 2, c.y});
            }
            triangle.close();
            canvas.fill_path(triangle, disabled ? th.disabled_text : th.secondary_text);
            label = {};
            break;
        }
        case kind::inc_dec: {
            bezel(canvas, bounds, 4, face, th.control_border);
            const auto middle = bounds.origin.y + bounds.size.height / 2;
            canvas.fill_rect({{bounds.origin.x + 2, middle}, {std::max(0.0, bounds.size.width - 4), 1}}, th.separator);
            const auto cx = bounds.origin.x + bounds.size.width / 2;
            const auto quarter = bounds.size.height / 4;
            chevron({cx, inverted ? middle + quarter : middle - quarter}, 3, false, th.text);
            chevron({cx, inverted ? middle - quarter : middle + quarter}, 3, true, th.text);
            label = {};
            break;
        }
        case kind::list_header: {
            canvas.fill_rect(bounds, value == 1 ? th.control_face_pressed : th.header_background);
            const auto right = bounds.origin.x + bounds.size.width - 1;
            canvas.fill_rect({{right, bounds.origin.y + 3}, {1, std::max(0.0, bounds.size.height - 6)}}, th.separator);
            canvas.fill_rect({{bounds.origin.x, inverted ? bounds.origin.y : bounds.origin.y + bounds.size.height - 1}, {bounds.size.width, 1}}, th.separator);
            // The sort indicator of a sorted (value on) header.
            if (value == 1)
                chevron({right - 10, bounds.origin.y + bounds.size.height / 2}, 3, (adornment & adornment_header_sort_up) == 0, th.secondary_text);
            label = {{bounds.origin.x + 4, bounds.origin.y}, {std::max(0.0, bounds.size.width - (value == 1 ? 22 : 8)), bounds.size.height}};
            break;
        }
        default:
            canvas.restore();
            unsupported("HIThemeDrawButton kind " + std::to_string(kind_value));
        }
        if ((adornment & adornment_focus) != 0) {
            drawing::Path ring;
            ring.add_rounded_rect({{bounds.origin.x - 2, bounds.origin.y - 2}, {bounds.size.width + 4, bounds.size.height + 4}}, 6, 6);
            canvas.stroke_path(ring, th.focus_ring, 3);
        }
        canvas.restore();
        if (s.x[4]) write_rect(m, s.x[4], label);
        s.x[0] = no_err;
    });
    kit.function(carbon_library, "_HIThemeDrawFocusRect", [canvas_for, light](CpuState& s, GuestMemory& m) {
        if (!s.x[0]) { s.x[0] = param_err; return; }
        if ((s.x[1] & 0xff) == 0) { s.x[0] = no_err; return; }   // no focus: nothing to draw
        const auto r = normalized(read_rect(m, s.x[0]));
        auto& canvas = canvas_for(s.x[2], "HIThemeDrawFocusRect");
        drawing::Path ring;
        ring.add_rounded_rect({{r.origin.x - 2, r.origin.y - 2}, {r.size.width + 4, r.size.height + 4}}, 3, 3);
        canvas.save();
        canvas.stroke_path(ring, light->focus_ring, 3);
        canvas.restore();
        s.x[0] = no_err;
    });
    kit.function(carbon_library, "_HIThemeDrawFrame", [canvas_for, light](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[1]) { s.x[0] = param_err; return; }
        const auto r = normalized(read_rect(m, s.x[0]));
        const auto frame_kind = m.read32(s.x[1] + 4);   // kHIThemeFrameTextFieldSquare (0) / ListBox (1)
        const auto focused = m.read8(s.x[1] + 12) != 0;
        if (frame_kind > 1) unsupported("HIThemeDrawFrame kind " + std::to_string(frame_kind));
        auto& canvas = canvas_for(s.x[2], "HIThemeDrawFrame");
        canvas.save();
        canvas.stroke_rect({{r.origin.x + 0.5, r.origin.y + 0.5}, {std::max(0.0, r.size.width - 1), std::max(0.0, r.size.height - 1)}},
                           focused ? light->focus_ring : light->control_border, 1);
        canvas.restore();
        s.x[0] = no_err;
    });
    kit.function(carbon_library, "_HIThemeDrawPaneSplitter", [canvas_for, light](CpuState& s, GuestMemory& m) {
        if (!s.x[0]) { s.x[0] = param_err; return; }
        const auto r = normalized(read_rect(m, s.x[0]));
        auto& canvas = canvas_for(s.x[2], "HIThemeDrawPaneSplitter");
        canvas.save();
        canvas.fill_rect(r, light->window_background);
        // A one-point divider line along the splitter's long axis.
        if (r.size.width >= r.size.height)
            canvas.fill_rect({{r.origin.x, r.origin.y + std::floor(r.size.height / 2)}, {r.size.width, 1}}, light->separator);
        else
            canvas.fill_rect({{r.origin.x + std::floor(r.size.width / 2), r.origin.y}, {1, r.size.height}}, light->separator);
        canvas.restore();
        s.x[0] = no_err;
    });

    // GetThemeMetric(ThemeMetric, SInt32* out): the metrics of AppKit's flat
    // theme (the sizes the controls of controls.cpp draw with) for the
    // metrics wxWidgets asks for; any other metric fails explicitly.
    kit.function(carbon_library, "_GetThemeMetric", [](CpuState& s, GuestMemory& m) {
        if (!s.x[1]) { s.x[0] = param_err; return; }
        std::int32_t value{};
        switch (static_cast<std::uint32_t>(s.x[0])) {
        case 0: value = 15; break;   // kThemeMetricScrollBarWidth
        case 1: value = 11; break;   // kThemeMetricSmallScrollBarWidth
        case 2: value = 14; break;   // kThemeMetricCheckBoxHeight
        case 3: value = 14; break;   // kThemeMetricRadioButtonHeight
        case 4: value = 2; break;    // kThemeMetricEditTextWhitespace
        case 5: value = 1; break;    // kThemeMetricEditTextFrameOutset
        case 6: value = 1; break;    // kThemeMetricListBoxFrameOutset
        case 7: value = 3; break;    // kThemeMetricFocusRectOutset
        case 8: value = 3; break;    // kThemeMetricImageWellThickness
        case 9: value = 0; break;    // kThemeMetricScrollBarOverlap
        default:
            unsupported("GetThemeMetric metric " + std::to_string(static_cast<std::uint32_t>(s.x[0])));
        }
        m.write32(s.x[1], static_cast<std::uint32_t>(value));
        s.x[0] = no_err;
    });

    // ---- Carbon event state ----
    // EventModifiers from the window server's NSEventModifierFlags.
    kit.function(carbon_library, "_GetCurrentKeyModifiers", [](CpuState& s, GuestMemory&) {
        const auto server = window_server_or_null();
        const auto flags = server ? server->modifier_flags() : 0;
        std::uint32_t carbon{};
        if (flags & (1ULL << 16)) carbon |= 0x400;    // alphaLock
        if (flags & (1ULL << 17)) carbon |= 0x200;    // shiftKey
        if (flags & (1ULL << 18)) carbon |= 0x1000;   // controlKey
        if (flags & (1ULL << 19)) carbon |= 0x800;    // optionKey
        if (flags & (1ULL << 20)) carbon |= 0x100;    // cmdKey
        s.x[0] = carbon;
    });
}

void AppKit::install_hitoolbox() { appkit::install_hitoolbox(*this); }

} // namespace alchemy::gui::appkit
