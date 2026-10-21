#include "alchemy/darwin/coretext/ct_internal.hpp"
#include "alchemy/darwin/coregraphics/context.hpp"
#include "alchemy/darwin/coregraphics/quartz2d.hpp"

#include "alchemy/engine_adapter.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <numbers>

// AppKit drawing over Quartz 2D: NSGraphicsContext, -[NSColor set],
// NSBezierPath over CGPath, NSImage / NSImageRep drawing and -lockFocus,
// NSString / NSAttributedString drawing and measuring through CoreText, and
// NSView bitmap caching (render_view_hierarchy, quartz2d.hpp).
//
// These replace the small software renderer of the AppKit providers for the
// selectors below (installed after install_appkit_gui; A4 keeps NSImage /
// NSBitmapImageRep storage, which is used here only through messages).
namespace alchemy::gui::quartz2d {
namespace {

struct GraphicsContextState final : ObjectPayload {
    Address cg{};                             // +1 CGContextRef
    Address rep{};                            // +1 NSBitmapImageRep the context draws into
    bool flipped{};
    std::uint64_t compositing{2};             // NSCompositingOperationSourceOver
    std::uint64_t interpolation{};            // NSImageInterpolationDefault
};

struct ThreadGraphics {
    Address current{};                        // +1 NSGraphicsContext
    std::vector<Address> saved;               // +1 each (+saveGraphicsState)
    struct Focus { Address image{}; Address context{}; };   // +1 both
    std::vector<Focus> focus;
};
thread_local ThreadGraphics graphics;

// NSCustomImageRep: an image representation drawn by a block
// (-initWithSize:flipped:drawingHandler:) or a delegate's selector
// (-initWithDrawSelector:delegate:) whenever it is drawn. NSImageRep
// instances carry AppKit's bitmap state (their size), so the drawing
// source lives in this table, keyed by the representation.
struct CustomRep {
    Address handler{};                        // _Block_copy'd drawing handler, or 0
    Address delegate{}, selector{};           // delegate (not retained) and its draw selector
    bool flipped{};
};
constexpr std::string_view libsystem_library = "/usr/lib/libSystem.B.dylib";
std::mutex custom_mutex;
std::map<Address, CustomRep> custom_reps;

struct BezierPathState final : ObjectPayload {
    PathData path;
    double line_width{1}, miter_limit{10}, flatness{0.6};
    std::uint64_t cap{}, join{}, winding{};
    std::vector<double> dashes;
    double phase{};
};
struct BezierDefaults {
    double line_width{1}, miter_limit{10}, flatness{0.6};
    std::uint64_t cap{}, join{}, winding{};
};
BezierDefaults bezier_defaults;

// NSCompositingOperation -> CGBlendMode.
std::int32_t blend_for(std::uint64_t operation) {
    switch (operation) {
    case 0: return blend::clear;
    case 1: return blend::copy;
    case 2: return blend::normal;
    case 3: return blend::source_in;
    case 4: return blend::source_out;
    case 5: return blend::source_atop;
    case 6: return blend::destination_over;
    case 7: return blend::destination_in;
    case 8: return blend::destination_out;
    case 9: return blend::destination_atop;
    case 10: return blend::exclusive_or;
    case 11: return blend::plus_darker;
    case 12: return blend::normal;            // NSCompositingOperationHighlight (deprecated: source over)
    case 13: return blend::plus_lighter;
    default:
        if (operation >= 14 && operation <= 28) return static_cast<std::int32_t>(operation - 13);   // multiply .. luminosity
        unsupported("NSCompositingOperation " + std::to_string(operation));
    }
}

} // namespace

std::uint64_t current_cg_context() {
    const ProviderScope scope;
    if (!graphics.current) return 0;
    auto* q = provider();
    if (!q) return 0;
    const auto state = q->find<GraphicsContextState>(graphics.current);
    return state ? state->cg : 0;
}

namespace {

std::vector<Address> array_items(Quartz& q, Address array) {
    if (!array) return {};
    if (q.cf.kind(array) == ObjectKind::array) return q.cf.array_values(array);
    std::vector<Address> items;
    const auto count = q.send(array, "count").x0;
    for (Address index = 0; index < count; ++index) items.push_back(q.send_id(array, "objectAtIndex:", {index}));
    return items;
}

// +1 NSGraphicsContext over a CGContext.
Address make_graphics_context(Quartz& q, Address cg, bool flipped, Address rep = 0) {
    const auto object = q.objc.allocate(q.objc.find_class("NSGraphicsContext"));
    const auto state = q.objc.host_state<GraphicsContextState>(object);
    state->cg = q.retain(cg);
    state->rep = q.retain(rep);
    state->flipped = flipped;
    return object;
}

void set_current(Quartz& q, Address context) {
    const auto previous = std::exchange(graphics.current, q.retain(context));
    q.release(previous);
}

// A bitmap context over an NSBitmapImageRep's pixels (8-bit RGBA / RGB(X) / gray).
Address context_for_rep(Quartz& q, Address rep) {
    const auto data = q.send_id(rep, "bitmapData");
    const auto width = q.send(rep, "pixelsWide").x0, height = q.send(rep, "pixelsHigh").x0;
    const auto bytes_per_row = q.send(rep, "bytesPerRow").x0, bits_per_pixel = q.send(rep, "bitsPerPixel").x0;
    const auto samples = q.send(rep, "samplesPerPixel").x0, bits_per_sample = q.send(rep, "bitsPerSample").x0;
    const auto has_alpha = (q.send(rep, "hasAlpha").x0 & 0xff) != 0;
    const auto format = q.send(rep, "bitmapFormat").x0;
    const auto points = result_size(q.send(rep, "size"));
    if (!data || !width || !height) return 0;
    PixelLayout layout;
    layout.width = static_cast<std::uint32_t>(width);
    layout.height = static_cast<std::uint32_t>(height);
    layout.bytes_per_row = bytes_per_row;
    layout.bits_per_component = static_cast<std::uint32_t>(bits_per_sample);
    layout.bits_per_pixel = static_cast<std::uint32_t>(bits_per_pixel);
    const bool alpha_first = (format & 1) != 0, straight = (format & 2) != 0;
    if ((format & 4) || bits_per_sample != 8) {
        diagnostic("NSGraphicsContext: unsupported NSBitmapImageRep format (8 bits per sample, integer only)");
        return 0;
    }
    if (samples == 1 && !has_alpha && bits_per_pixel == 8) {
        layout.model = ColorModel::monochrome;
        layout.color_components = 1;
        layout.bitmap_info = alpha_info::none;
    } else if (samples == 4 && has_alpha && bits_per_pixel == 32) {
        layout.straight_alpha = straight;
        layout.bitmap_info = straight ? (alpha_first ? alpha_info::first : alpha_info::last)
                                      : (alpha_first ? alpha_info::premultiplied_first : alpha_info::premultiplied_last);
    } else if (samples == 3 && !has_alpha && bits_per_pixel == 32) {
        layout.bitmap_info = alpha_info::none_skip_last;
    } else {
        diagnostic("NSGraphicsContext: unsupported NSBitmapImageRep layout (" + std::to_string(samples) + " samples, " +
                   std::to_string(bits_per_pixel) + " bits per pixel)");
        return 0;
    }
    const auto space = layout.model == ColorModel::monochrome ? q.device_gray() : q.srgb();
    const auto context = create_context(q, layout, data, space);
    if (!context) return 0;
    const auto state = context_state(q, context);
    const auto sx = points.width > 0 ? width / points.width : 1.0, sy = points.height > 0 ? height / points.height : 1.0;
    state->base_ctm = Affine::scaling(sx, sy);
    state->g.ctm = state->base_ctm;
    return context;
}

// The current CGContext's state (nullptr when no context is current).
std::shared_ptr<ContextState> current_state(Quartz& q) {
    const auto cg = current_cg_context();
    return cg ? context_state(q, cg) : nullptr;
}
bool current_flipped(Quartz& q) {
    if (!graphics.current) return false;
    const auto state = q.find<GraphicsContextState>(graphics.current);
    return state && state->flipped;
}

// A CGImage of an NSBitmapImageRep's pixels (copied).
Address image_from_rep(Quartz& q, Address rep) {
    const auto data = q.send_id(rep, "bitmapData");
    const auto width = q.send(rep, "pixelsWide").x0, height = q.send(rep, "pixelsHigh").x0;
    if (!data || !width || !height) return 0;
    PixelLayout layout;
    layout.width = static_cast<std::uint32_t>(width);
    layout.height = static_cast<std::uint32_t>(height);
    layout.bytes_per_row = q.send(rep, "bytesPerRow").x0;
    layout.bits_per_component = static_cast<std::uint32_t>(q.send(rep, "bitsPerSample").x0);
    layout.bits_per_pixel = static_cast<std::uint32_t>(q.send(rep, "bitsPerPixel").x0);
    const auto samples = q.send(rep, "samplesPerPixel").x0;
    const auto has_alpha = (q.send(rep, "hasAlpha").x0 & 0xff) != 0;
    const auto format = q.send(rep, "bitmapFormat").x0;
    const bool alpha_first = (format & 1) != 0, straight = (format & 2) != 0;
    layout.straight_alpha = true;
    if (format & 4) layout.bitmap_info |= bitmap_info::float_components;
    if (samples <= 2) {
        layout.model = ColorModel::monochrome;
        layout.color_components = 1;
    }
    if (has_alpha) layout.bitmap_info |= straight ? (alpha_first ? alpha_info::first : alpha_info::last)
                                                  : (alpha_first ? alpha_info::premultiplied_first : alpha_info::premultiplied_last);
    else if (layout.bits_per_pixel > layout.bits_per_component * layout.color_components) layout.bitmap_info |= alpha_info::none_skip_last;
    auto bytes = std::make_shared<std::vector<std::uint8_t>>(static_cast<std::size_t>(layout.byte_size()));
    q.memory().read_bytes(data, *bytes);
    return q.make_image(layout, layout.model == ColorModel::monochrome ? q.device_gray() : q.srgb(), bytes);
}

Address bitmap_rep_of(Quartz& q, Address image) {
    const auto bitmap_class = q.objc.find_class("NSBitmapImageRep");
    for (const auto rep : array_items(q, q.send_id(image, "representations")))
        if (q.is_kind(rep, bitmap_class)) return rep;
    return 0;
}

std::optional<CustomRep> custom_rep(Address rep) {
    std::lock_guard lock(custom_mutex);
    const auto found = custom_reps.find(rep);
    if (found == custom_reps.end()) return std::nullopt;
    return found->second;
}

// Runs a custom representation's drawing source for `rect` in the current
// context; false when it is not one or its handler answered NO.
bool run_custom_rep(Quartz& q, Address rep, Rect rect) {
    const auto info = custom_rep(rep);
    if (!info) return false;
    if (info->handler) {
        // block->invoke(block, NSRect dstRect) -> BOOL
        CallRegs registers;
        registers.x[0] = info->handler;
        registers.set_d(0, rect.origin.x); registers.set_d(1, rect.origin.y);
        registers.set_d(2, rect.size.width); registers.set_d(3, rect.size.height);
        const auto invoke = q.memory().read64(info->handler + 16);
        return (EngineAdapter(q.process.calling_engine()).call_regs(invoke, registers).x0 & 0xff) != 0;
    }
    if (info->delegate && info->selector) {
        CallRegs registers;
        registers.x[2] = rep;
        static_cast<void>(q.objc.send(info->delegate, info->selector, registers));
        return true;
    }
    return false;
}

// Draws `cg_image` (pixels of an image whose point size is `size`), the
// `from` part (image points, bottom-left origin; empty = all) into `rect`.
void draw_cg_image(Quartz& q, Address cg_image, Size size, Rect rect, Rect from, std::uint64_t operation, double fraction, bool respect_flipped) {
    const auto c = current_state(q);
    if (!c || !cg_image) return;
    const auto info = q.require<ImageInfo>(cg_image, "CGImage");
    std::lock_guard lock(c->mutex);
    save_state(*c);
    try {
        c->g.alpha *= std::clamp(fraction, 0.0, 1.0);
        c->g.blend_mode = blend_for(operation);
        if (respect_flipped && current_flipped(q)) {
            // Keep the image upright in a flipped context.
            c->g.ctm = Affine::translation(0, rect.origin.y + rect.size.height).then(Affine::scaling(1, -1))
                           .then(Affine::translation(0, -rect.origin.y)).then(c->g.ctm);
        }
        auto image = cg_image;
        Address cropped{};
        if (!rect_is_empty(from) && size.width > 0 && size.height > 0) {
            const auto sx = info->desc.layout.width / size.width, sy = info->desc.layout.height / size.height;
            const Rect pixels{{from.origin.x * sx, (size.height - from.origin.y - from.size.height) * sy}, {from.size.width * sx, from.size.height * sy}};
            const auto found = q.process.resolve(core_graphics_library, "_CGImageCreateWithImageInRect");
            if (!found) fail("missing CGImageCreateWithImageInRect");
            CallRegs registers;
            registers.x[0] = cg_image;
            put_rect(registers, 0, pixels);
            cropped = engine_call_regs(q.process.calling_engine(), found->address, registers).x0;
            if (cropped) image = cropped;
        }
        try { draw_image(q, *c, image, rect); } catch (...) { q.release(cropped); throw; }
        q.release(cropped);
    } catch (...) { restore_state(q, *c); throw; }
    restore_state(q, *c);
}

// ---- string drawing ---------------------------------------------------------------------------
// NSStringDrawingOptions
constexpr std::uint64_t uses_line_fragment_origin = 1, truncates_last_visible_line = 1 << 5;

struct Measured { double width{}, height{}, first_ascent{}, last_descent{}; };
Measured measure(Quartz& q, const AttributedText& text, double width, bool wrap) {
    auto placed = place_text(q, text, {0, 0}, wrap && width > 0 ? width : 1.0e7, 1.0e7);
    Measured result;
    result.width = placed.max_width;
    result.height = placed.used_height;
    if (!placed.lines.empty()) {
        result.first_ascent = placed.lines.front().ascent;
        result.last_descent = placed.lines.back().descent;
    }
    for (auto& line : placed.lines) release_line(q, line);
    return result;
}

// Draws `text` in `box` (user space of the current context): top-aligned,
// wrapping at the box width when `wrap`, clipped to the box when `clip`.
void draw_text(Quartz& q, AttributedText& text, Rect box, bool wrap, bool clip) {
    const auto c = current_state(q);
    if (!c) return;
    const bool flipped = current_flipped(q);
    auto placed = place_text(q, text, {0, 0}, wrap && box.size.width > 0 ? box.size.width : 1.0e7, 1.0e7);
    std::lock_guard lock(c->mutex);
    save_state(*c);
    const auto saved_matrix = c->text_matrix;
    try {
        if (clip) {
            PathData path;
            path.add_rect(c->g.ctm, box);
            clip_to_path(q, *c, path, false);
        }
        for (std::size_t index = 0; index < placed.lines.size(); ++index) {
            const auto& origin = placed.origins[index];
            const auto x = box.origin.x + origin.x;
            if (flipped) {
                c->text_matrix = {1, 0, 0, -1, x, box.origin.y + origin.y};
            } else {
                c->text_matrix = {1, 0, 0, 1, x, box.origin.y + box.size.height - origin.y};
            }
            draw_line_at(q, *c, placed.lines[index], true);
        }
    } catch (...) {
        c->text_matrix = saved_matrix;
        restore_state(q, *c);
        for (auto& line : placed.lines) release_line(q, line);
        throw;
    }
    c->text_matrix = saved_matrix;
    restore_state(q, *c);
    for (auto& line : placed.lines) release_line(q, line);
}

// ---- views -------------------------------------------------------------------------------------------
void draw_view(Quartz& q, Address view, Address cg, bool place, bool parent_flipped) {
    if ((q.send(view, "isHidden").x0 & 0xff) != 0) return;
    const auto frame = result_rect(q.send(view, "frame"));
    const auto bounds = result_rect(q.send(view, "bounds"));
    const bool flipped = (q.send(view, "isFlipped").x0 & 0xff) != 0;
    const auto c = context_state(q, cg);
    {
        std::lock_guard lock(c->mutex);
        save_state(*c);
        if (place) {
            c->g.ctm = Affine::translation(frame.origin.x, frame.origin.y).then(c->g.ctm);
            if (flipped != parent_flipped) c->g.ctm = Affine::translation(0, frame.size.height).then(Affine::scaling(1, -1)).then(c->g.ctm);
            if (bounds.size.width > 0 && bounds.size.height > 0)
                c->g.ctm = Affine::scaling(frame.size.width / bounds.size.width, frame.size.height / bounds.size.height).then(c->g.ctm);
            c->g.ctm = Affine::translation(-bounds.origin.x, -bounds.origin.y).then(c->g.ctm);
        }
        PathData clip;
        clip.add_rect(c->g.ctm, bounds);
        clip_to_path(q, *c, clip, false);
    }
    const auto previous = graphics.current ? q.retain(graphics.current) : 0;
    const auto ns_context = make_graphics_context(q, cg, flipped);
    set_current(q, ns_context);
    q.release(ns_context);
    try {
        CallRegs registers;
        put_rect(registers, 0, bounds);
        static_cast<void>(q.send(view, "drawRect:", registers));
        for (const auto child : array_items(q, q.send_id(view, "subviews"))) draw_view(q, child, cg, true, flipped);
    } catch (...) {
        set_current(q, previous);
        q.release(previous);
        std::lock_guard lock(c->mutex);
        restore_state(q, *c);
        throw;
    }
    set_current(q, previous);
    q.release(previous);
    std::lock_guard lock(c->mutex);
    restore_state(q, *c);
}

} // namespace

void render_view_hierarchy(std::uint64_t view, std::uint64_t context, std::optional<Rect> dirty) {
    const ProviderScope scope;
    auto* q = provider();
    if (!q) fail("Quartz provider is not installed");
    const auto c = context_state(*q, context);
    if (dirty) {
        std::lock_guard lock(c->mutex);
        save_state(*c);
        PathData clip;
        clip.add_rect(c->g.ctm, *dirty);
        clip_to_path(*q, *c, clip, false);
    }
    try { draw_view(*q, view, context, true, false); } catch (...) {
        if (dirty) { std::lock_guard lock(c->mutex); restore_state(*q, *c); }
        throw;
    }
    if (dirty) { std::lock_guard lock(c->mutex); restore_state(*q, *c); }
}

void Quartz::install_appkit_drawing() {
    Quartz* const q = this;
    const auto method = [this](Address cls, const char* name, Handler handler, const char* types = "@@:",
                                   const std::source_location& where = std::source_location::current()) {
        registration_audit::Site site(where);
        static_cast<void>(objc.add_native_method(cls, name, scoped(std::move(handler)), false, types));
    };
    const auto class_method = [this](Address cls, const char* name, Handler handler, const char* types = "@@:",
                                   const std::source_location& where = std::source_location::current()) {
        registration_audit::Site site(where);
        static_cast<void>(objc.add_native_method(cls, name, scoped(std::move(handler)), true, types));
    };
    constexpr auto rect_type = "{CGRect={CGPoint=dd}{CGSize=dd}}";

    // ---- NSGraphicsContext ----
    const auto gc = declare_class("NSGraphicsContext", "NSObject", appkit_library);
    const auto gc_state = [q](Address object) { return q->objc.host_state<GraphicsContextState>(object); };
    class_method(gc, "currentContext", [](CpuState& s, GuestMemory&) { s.x[0] = graphics.current; });
    class_method(gc, "setCurrentContext:", [q](CpuState& s, GuestMemory&) { set_current(*q, s.x[2]); }, "v@:@");
    class_method(gc, "currentContextDrawingToScreen", [](CpuState& s, GuestMemory&) { return_bool(s, true); }, "B@:");
    class_method(gc, "saveGraphicsState", [q](CpuState&, GuestMemory&) {
        graphics.saved.push_back(q->retain(graphics.current));
        if (const auto cg = current_cg_context()) {
            const auto c = context_state(*q, cg);
            std::lock_guard lock(c->mutex);
            save_state(*c);
        }
    }, "v@:");
    class_method(gc, "restoreGraphicsState", [q](CpuState&, GuestMemory&) {
        if (graphics.saved.empty()) return;
        const auto saved = graphics.saved.back();
        graphics.saved.pop_back();
        set_current(*q, saved);
        q->release(saved);
        if (const auto cg = current_cg_context()) {
            const auto c = context_state(*q, cg);
            std::lock_guard lock(c->mutex);
            restore_state(*q, *c);
        }
    }, "v@:");
    class_method(gc, "graphicsContextWithCGContext:flipped:", [q](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[2] ? q->autorelease(make_graphics_context(*q, s.x[2], bool_arg(s, 3))) : 0;
    }, "@@:^vB");
    class_method(gc, "graphicsContextWithGraphicsPort:flipped:", [q](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[2] ? q->autorelease(make_graphics_context(*q, s.x[2], bool_arg(s, 3))) : 0;
    }, "@@:^vB");
    class_method(gc, "graphicsContextWithBitmapImageRep:", [q](CpuState& s, GuestMemory&) {
        const auto cg = s.x[2] ? context_for_rep(*q, s.x[2]) : 0;
        if (!cg) { s.x[0] = 0; return; }
        s.x[0] = q->autorelease(make_graphics_context(*q, cg, false, s.x[2]));
        q->release(cg);
    });
    method(gc, "CGContext", [gc_state](CpuState& s, GuestMemory&) { s.x[0] = gc_state(s.x[0])->cg; }, "^v@:");
    method(gc, "graphicsPort", [gc_state](CpuState& s, GuestMemory&) { s.x[0] = gc_state(s.x[0])->cg; }, "^v@:");
    method(gc, "isFlipped", [gc_state](CpuState& s, GuestMemory&) { return_bool(s, gc_state(s.x[0])->flipped); }, "B@:");
    method(gc, "isDrawingToScreen", [gc_state](CpuState& s, GuestMemory&) { return_bool(s, !gc_state(s.x[0])->rep); }, "B@:");
    method(gc, "attributes", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    // Bitmap contexts are always synchronized with their pixels.
    method(gc, "flushGraphics", [](CpuState&, GuestMemory&) {}, "v@:");
    method(gc, "saveGraphicsState", [q, gc_state](CpuState& s, GuestMemory&) {
        const auto c = context_state(*q, gc_state(s.x[0])->cg);
        std::lock_guard lock(c->mutex);
        save_state(*c);
    }, "v@:");
    method(gc, "restoreGraphicsState", [q, gc_state](CpuState& s, GuestMemory&) {
        const auto c = context_state(*q, gc_state(s.x[0])->cg);
        std::lock_guard lock(c->mutex);
        restore_state(*q, *c);
    }, "v@:");
    method(gc, "shouldAntialias", [q, gc_state](CpuState& s, GuestMemory&) {
        const auto c = context_state(*q, gc_state(s.x[0])->cg);
        return_bool(s, c->g.antialias);
    }, "B@:");
    method(gc, "setShouldAntialias:", [q, gc_state](CpuState& s, GuestMemory&) {
        const auto c = context_state(*q, gc_state(s.x[0])->cg);
        std::lock_guard lock(c->mutex);
        c->g.antialias = bool_arg(s, 2);
    }, "v@:B");
    method(gc, "imageInterpolation", [gc_state](CpuState& s, GuestMemory&) { s.x[0] = gc_state(s.x[0])->interpolation; }, "Q@:");
    method(gc, "setImageInterpolation:", [q, gc_state](CpuState& s, GuestMemory&) {
        const auto state = gc_state(s.x[0]);
        state->interpolation = s.x[2];
        const auto c = context_state(*q, state->cg);
        std::lock_guard lock(c->mutex);
        // NSImageInterpolation: default 0, none 1, low 2, medium 4, high 3 (same as CGInterpolationQuality).
        c->g.interpolation = static_cast<std::int32_t>(s.x[2]);
    }, "v@:Q");
    method(gc, "compositingOperation", [gc_state](CpuState& s, GuestMemory&) { s.x[0] = gc_state(s.x[0])->compositing; }, "Q@:");
    method(gc, "setCompositingOperation:", [q, gc_state](CpuState& s, GuestMemory&) {
        const auto state = gc_state(s.x[0]);
        state->compositing = s.x[2];
        const auto c = context_state(*q, state->cg);
        std::lock_guard lock(c->mutex);
        c->g.blend_mode = blend_for(s.x[2]);
    }, "v@:Q");
    method(gc, "patternPhase", [q, gc_state](CpuState& s, GuestMemory&) {
        const auto c = context_state(*q, gc_state(s.x[0])->cg);
        return_point(s, {c->g.pattern_phase.width, c->g.pattern_phase.height});
    }, "{CGPoint=dd}@:");
    method(gc, "setPatternPhase:", [q, gc_state](CpuState& s, GuestMemory&) {
        const auto c = context_state(*q, gc_state(s.x[0])->cg);
        std::lock_guard lock(c->mutex);
        c->g.pattern_phase = size_arg(s);
    }, "v@:{CGPoint=dd}");
    method(gc, "dealloc", [q, gc](CpuState& s, GuestMemory&) {
        if (const auto state = q->find<GraphicsContextState>(s.x[0])) {
            q->release(std::exchange(state->cg, 0));
            q->release(std::exchange(state->rep, 0));
        }
        static_cast<void>(q->objc.send_super(s.x[0], gc, q->objc.selector("dealloc")));
    }, "v@:");

    // ---- NSColor drawing ----
    if (const auto color = objc.find_class("NSColor")) {
        const auto set_paint = [q](Address color_object, bool fill, bool stroke) {
            const auto c = current_state(*q);
            if (!c) return;   // no current context: nothing to set, as in AppKit
            const auto rgba = object_color(*q, color_object).value_or(Rgba{0, 0, 0, 1});
            std::lock_guard lock(c->mutex);
            if (fill) { c->g.fill.pattern.reset(); c->g.fill.color = rgba; c->g.fill.space = ObjectRef::retain(q->srgb()); }
            if (stroke) { c->g.stroke.pattern.reset(); c->g.stroke.color = rgba; c->g.stroke.space = ObjectRef::retain(q->srgb()); }
        };
        method(color, "set", [set_paint](CpuState& s, GuestMemory&) { set_paint(s.x[0], true, true); }, "v@:");
        method(color, "setFill", [set_paint](CpuState& s, GuestMemory&) { set_paint(s.x[0], true, false); }, "v@:");
        method(color, "setStroke", [set_paint](CpuState& s, GuestMemory&) { set_paint(s.x[0], false, true); }, "v@:");
        method(color, "CGColor", [q](CpuState& s, GuestMemory&) {
            const auto rgba = object_color(*q, s.x[0]).value_or(Rgba{0, 0, 0, 1});
            s.x[0] = q->autorelease(q->make_color(q->srgb(), {rgba.r, rgba.g, rgba.b, rgba.a}));
        }, "^v@:");
        class_method(color, "colorWithCGColor:", [q](CpuState& s, GuestMemory&) {
            if (!s.x[2]) { s.x[0] = 0; return; }
            const auto rgba = q->color_rgba(s.x[2]);
            CallRegs registers;
            registers.set_d(0, rgba.r); registers.set_d(1, rgba.g); registers.set_d(2, rgba.b); registers.set_d(3, rgba.a);
            s.x[0] = q->send(s.x[0], "colorWithSRGBRed:green:blue:alpha:", registers).x0;
        }, "@@:^v");
        method(color, "drawSwatchInRect:", [q](CpuState& s, GuestMemory&) {
            const auto c = current_state(*q);
            if (!c) return;
            const auto rgba = object_color(*q, s.x[0]).value_or(Rgba{0, 0, 0, 1});
            std::lock_guard lock(c->mutex);
            fill_rect_color(*q, *c, rect_arg(s), rgba);
        }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    }
    // NSRectFill & co. (AppKit functions): the current fill color / operation.
    const auto rect_fill = [q](Rect rect, std::optional<std::int32_t> mode) {
        const auto c = current_state(*q);
        if (!c) return;
        std::lock_guard lock(c->mutex);
        PathData path;
        path.add_rect(c->g.ctm, rect);
        const auto saved = c->g.blend_mode;
        c->g.blend_mode = mode.value_or(blend::copy);
        try { fill_path(*q, *c, path, false); } catch (...) { c->g.blend_mode = saved; throw; }
        c->g.blend_mode = saved;
    };
    function(appkit_library, "_NSRectFill", [rect_fill](CpuState& s, GuestMemory&) { rect_fill(rect_arg(s), std::nullopt); });
    function(appkit_library, "_NSRectFillUsingOperation", [rect_fill](CpuState& s, GuestMemory&) { rect_fill(rect_arg(s), blend_for(s.x[0])); });
    function(appkit_library, "_NSRectFillList", [this, rect_fill](CpuState& s, GuestMemory&) {
        for (Address index = 0; index < s.x[1]; ++index) rect_fill(read_rect(s.x[0] + index * 32), std::nullopt);
    });
    function(appkit_library, "_NSFrameRect", [q](CpuState& s, GuestMemory&) {
        const auto c = current_state(*q);
        if (!c) return;
        std::lock_guard lock(c->mutex);
        const auto r = standardize(rect_arg(s));
        PathData path;
        for (const auto& edge : {Rect{r.origin, {r.size.width, 1}}, Rect{{r.origin.x, r.origin.y + r.size.height - 1}, {r.size.width, 1}},
                                 Rect{r.origin, {1, r.size.height}}, Rect{{r.origin.x + r.size.width - 1, r.origin.y}, {1, r.size.height}}})
            path.add_rect(c->g.ctm, edge);
        fill_path(*q, *c, path, false);
    });
    function(appkit_library, "_NSRectClip", [q](CpuState& s, GuestMemory&) {
        const auto c = current_state(*q);
        if (!c) return;
        std::lock_guard lock(c->mutex);
        PathData path;
        path.add_rect(c->g.ctm, rect_arg(s));
        clip_to_path(*q, *c, path, false);
    });

    // ---- NSBezierPath ----
    const auto bp = declare_class("NSBezierPath", "NSObject", appkit_library);
    const auto path_of = [q](Address object) {
        return q->objc.host_state<BezierPathState>(object, [] {
            auto state = std::make_shared<BezierPathState>();
            state->line_width = bezier_defaults.line_width;
            state->miter_limit = bezier_defaults.miter_limit;
            state->flatness = bezier_defaults.flatness;
            state->cap = bezier_defaults.cap;
            state->join = bezier_defaults.join;
            state->winding = bezier_defaults.winding;
            return state;
        });
    };
    const auto new_path = [q, bp, path_of](const std::function<void(PathData&)>& build) {
        const auto object = q->objc.allocate(bp);
        build(path_of(object)->path);
        return q->autorelease(object);
    };
    class_method(bp, "bezierPath", [new_path](CpuState& s, GuestMemory&) { s.x[0] = new_path([](PathData&) {}); });
    class_method(bp, "bezierPathWithRect:", [new_path](CpuState& s, GuestMemory&) {
        const auto r = rect_arg(s);
        s.x[0] = new_path([&](PathData& p) { p.add_rect({}, r); });
    }, "@@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    class_method(bp, "bezierPathWithOvalInRect:", [new_path](CpuState& s, GuestMemory&) {
        const auto r = rect_arg(s);
        s.x[0] = new_path([&](PathData& p) { p.add_ellipse({}, r); });
    }, "@@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    class_method(bp, "bezierPathWithRoundedRect:xRadius:yRadius:", [new_path](CpuState& s, GuestMemory&) {
        const auto r = rect_arg(s);
        const auto rx = d(s, 4), ry = d(s, 5);
        s.x[0] = new_path([&](PathData& p) { p.add_rounded_rect({}, r, rx, ry); });
    }, "@@:{CGRect={CGPoint=dd}{CGSize=dd}}dd");
    class_method(bp, "bezierPathWithCGPath:", [this, new_path](CpuState& s, GuestMemory&) {
        const auto source = s.x[2] ? require<PathInfo>(s.x[2], "CGPath")->data : PathData{};
        s.x[0] = new_path([&](PathData& p) { p = source; });
    }, "@@:^v");
    for (const auto& [name, field] : std::initializer_list<std::pair<const char*, double BezierDefaults::*>>{
             {"defaultLineWidth", &BezierDefaults::line_width}, {"defaultMiterLimit", &BezierDefaults::miter_limit},
             {"defaultFlatness", &BezierDefaults::flatness}}) {
        const auto member = field;
        class_method(bp, name, [member](CpuState& s, GuestMemory&) { set_d(s, 0, bezier_defaults.*member); }, "d@:");
        const std::string setter = std::string("set") + static_cast<char>(name[0] - 'a' + 'A') + (name + 1) + ":";
        static_cast<void>(objc.add_native_method(bp, setter, [member](CpuState& s, GuestMemory&) { bezier_defaults.*member = d(s, 0); }, true, "v@:d"));
    }
    for (const auto& [name, field] : std::initializer_list<std::pair<const char*, std::uint64_t BezierDefaults::*>>{
             {"defaultLineCapStyle", &BezierDefaults::cap}, {"defaultLineJoinStyle", &BezierDefaults::join},
             {"defaultWindingRule", &BezierDefaults::winding}}) {
        const auto member = field;
        class_method(bp, name, [member](CpuState& s, GuestMemory&) { s.x[0] = bezier_defaults.*member; }, "Q@:");
        const std::string setter = std::string("set") + static_cast<char>(name[0] - 'a' + 'A') + (name + 1) + ":";
        static_cast<void>(objc.add_native_method(bp, setter, [member](CpuState& s, GuestMemory&) { bezier_defaults.*member = s.x[2]; }, true, "v@:Q"));
    }
    // Painting a path with its own line attributes in the current context.
    const auto paint = [q](const BezierPathState& path, bool fill) {
        const auto c = current_state(*q);
        if (!c) return;   // AppKit logs "invalid context" and draws nothing
        std::lock_guard lock(c->mutex);
        const auto base = path.path.transformed(c->g.ctm);
        if (fill) { fill_path(*q, *c, base, path.winding == 1); return; }
        const auto saved = c->g;
        c->g.line_width = path.line_width;
        c->g.cap = static_cast<std::uint32_t>(path.cap);
        c->g.join = static_cast<std::uint32_t>(path.join);
        c->g.miter_limit = path.miter_limit;
        c->g.dashes = path.dashes;
        c->g.dash_phase = path.phase;
        try { stroke_path(*q, *c, base); } catch (...) { c->g = saved; throw; }
        c->g.line_width = saved.line_width; c->g.cap = saved.cap; c->g.join = saved.join;
        c->g.miter_limit = saved.miter_limit; c->g.dashes = saved.dashes; c->g.dash_phase = saved.dash_phase;
    };
    method(bp, "fill", [paint, path_of](CpuState& s, GuestMemory&) { paint(*path_of(s.x[0]), true); }, "v@:");
    method(bp, "stroke", [paint, path_of](CpuState& s, GuestMemory&) { paint(*path_of(s.x[0]), false); }, "v@:");
    method(bp, "addClip", [q, path_of](CpuState& s, GuestMemory&) {
        const auto c = current_state(*q);
        if (!c) return;
        std::lock_guard lock(c->mutex);
        const auto state = path_of(s.x[0]);
        clip_to_path(*q, *c, state->path.transformed(c->g.ctm), state->winding == 1);
    }, "v@:");
    method(bp, "setClip", [q, path_of](CpuState& s, GuestMemory&) {
        const auto c = current_state(*q);
        if (!c) return;
        std::lock_guard lock(c->mutex);
        c->g.clips.clear();
        const auto state = path_of(s.x[0]);
        clip_to_path(*q, *c, state->path.transformed(c->g.ctm), state->winding == 1);
    }, "v@:");
    class_method(bp, "fillRect:", [paint](CpuState& s, GuestMemory&) {
        BezierPathState state;
        state.path.add_rect({}, rect_arg(s));
        paint(state, true);
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    class_method(bp, "strokeRect:", [paint](CpuState& s, GuestMemory&) {
        BezierPathState state;
        state.line_width = bezier_defaults.line_width;
        state.path.add_rect({}, rect_arg(s));
        paint(state, false);
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    class_method(bp, "strokeLineFromPoint:toPoint:", [paint](CpuState& s, GuestMemory&) {
        BezierPathState state;
        state.line_width = bezier_defaults.line_width;
        state.path.move_to(point_arg(s));
        state.path.line_to(point_arg(s, 2));
        paint(state, false);
    }, "v@:{CGPoint=dd}{CGPoint=dd}");
    class_method(bp, "clipRect:", [q](CpuState& s, GuestMemory&) {
        const auto c = current_state(*q);
        if (!c) return;
        std::lock_guard lock(c->mutex);
        PathData path;
        path.add_rect(c->g.ctm, rect_arg(s));
        clip_to_path(*q, *c, path, false);
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    method(bp, "moveToPoint:", [path_of](CpuState& s, GuestMemory&) { path_of(s.x[0])->path.move_to(point_arg(s)); }, "v@:{CGPoint=dd}");
    method(bp, "lineToPoint:", [path_of](CpuState& s, GuestMemory&) {
        auto& path = path_of(s.x[0])->path;
        if (!path.has_current) fail("-[NSBezierPath lineToPoint:]: no current point");
        path.line_to(point_arg(s));
    }, "v@:{CGPoint=dd}");
    method(bp, "curveToPoint:controlPoint1:controlPoint2:", [path_of](CpuState& s, GuestMemory&) {
        auto& path = path_of(s.x[0])->path;
        if (!path.has_current) fail("-[NSBezierPath curveToPoint:...]: no current point");
        path.cubic_to(point_arg(s, 2), point_arg(s, 4), point_arg(s));
    }, "v@:{CGPoint=dd}{CGPoint=dd}{CGPoint=dd}");
    method(bp, "curveToPoint:controlPoint:", [path_of](CpuState& s, GuestMemory&) {
        auto& path = path_of(s.x[0])->path;
        if (!path.has_current) fail("-[NSBezierPath curveToPoint:controlPoint:]: no current point");
        path.quad_to(point_arg(s, 2), point_arg(s));
    }, "v@:{CGPoint=dd}{CGPoint=dd}");
    method(bp, "closePath", [path_of](CpuState& s, GuestMemory&) { path_of(s.x[0])->path.close(); }, "v@:");
    method(bp, "relativeMoveToPoint:", [path_of](CpuState& s, GuestMemory&) {
        auto& path = path_of(s.x[0])->path;
        path.move_to({path.current.x + d(s, 0), path.current.y + d(s, 1)});
    }, "v@:{CGPoint=dd}");
    method(bp, "relativeLineToPoint:", [path_of](CpuState& s, GuestMemory&) {
        auto& path = path_of(s.x[0])->path;
        path.line_to({path.current.x + d(s, 0), path.current.y + d(s, 1)});
    }, "v@:{CGPoint=dd}");
    method(bp, "relativeCurveToPoint:controlPoint1:controlPoint2:", [path_of](CpuState& s, GuestMemory&) {
        auto& path = path_of(s.x[0])->path;
        const auto o = path.current;
        path.cubic_to({o.x + d(s, 2), o.y + d(s, 3)}, {o.x + d(s, 4), o.y + d(s, 5)}, {o.x + d(s, 0), o.y + d(s, 1)});
    }, "v@:{CGPoint=dd}{CGPoint=dd}{CGPoint=dd}");
    method(bp, "removeAllPoints", [path_of](CpuState& s, GuestMemory&) { path_of(s.x[0])->path = {}; }, "v@:");
    method(bp, "appendBezierPath:", [path_of](CpuState& s, GuestMemory&) {
        if (!s.x[2]) return;
        const auto source = path_of(s.x[2])->path;
        path_of(s.x[0])->path.append(source, {});
    }, "v@:@");
    method(bp, "appendBezierPathWithRect:", [path_of](CpuState& s, GuestMemory&) { path_of(s.x[0])->path.add_rect({}, rect_arg(s)); }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    method(bp, "appendBezierPathWithOvalInRect:", [path_of](CpuState& s, GuestMemory&) { path_of(s.x[0])->path.add_ellipse({}, rect_arg(s)); }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    method(bp, "appendBezierPathWithRoundedRect:xRadius:yRadius:", [path_of](CpuState& s, GuestMemory&) {
        path_of(s.x[0])->path.add_rounded_rect({}, rect_arg(s), d(s, 4), d(s, 5));
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}dd");
    constexpr double degrees = std::numbers::pi / 180.0;
    method(bp, "appendBezierPathWithArcWithCenter:radius:startAngle:endAngle:clockwise:", [path_of, degrees](CpuState& s, GuestMemory&) {
        path_of(s.x[0])->path.add_arc({}, point_arg(s), d(s, 2), d(s, 3) * degrees, d(s, 4) * degrees, bool_arg(s, 2));
    }, "v@:{CGPoint=dd}dddB");
    method(bp, "appendBezierPathWithArcWithCenter:radius:startAngle:endAngle:", [path_of, degrees](CpuState& s, GuestMemory&) {
        path_of(s.x[0])->path.add_arc({}, point_arg(s), d(s, 2), d(s, 3) * degrees, d(s, 4) * degrees, false);
    }, "v@:{CGPoint=dd}ddd");
    method(bp, "appendBezierPathWithArcFromPoint:toPoint:radius:", [path_of](CpuState& s, GuestMemory&) {
        path_of(s.x[0])->path.add_arc_to_point({}, point_arg(s), point_arg(s, 2), d(s, 4));
    }, "v@:{CGPoint=dd}{CGPoint=dd}d");
    method(bp, "appendBezierPathWithPoints:count:", [this, path_of](CpuState& s, GuestMemory&) {
        auto& path = path_of(s.x[0])->path;
        for (Address index = 0; index < s.x[3]; ++index) {
            const auto p = read_point(s.x[2] + index * 16);
            if (index == 0 && !path.has_current) path.move_to(p); else path.line_to(p);
        }
    }, "v@:^{CGPoint=dd}q");
    method(bp, "bounds", [path_of](CpuState& s, GuestMemory&) {
        const auto r = path_of(s.x[0])->path.path_bounds();
        return_rect(s, rect_is_null(r) ? Rect{} : r);
    }, rect_type);
    method(bp, "controlPointBounds", [path_of](CpuState& s, GuestMemory&) {
        const auto r = path_of(s.x[0])->path.control_bounds();
        return_rect(s, rect_is_null(r) ? Rect{} : r);
    }, rect_type);
    method(bp, "isEmpty", [path_of](CpuState& s, GuestMemory&) { return_bool(s, path_of(s.x[0])->path.empty()); }, "B@:");
    method(bp, "currentPoint", [path_of](CpuState& s, GuestMemory&) {
        const auto& path = path_of(s.x[0])->path;
        if (!path.has_current) fail("-[NSBezierPath currentPoint]: no current point");
        return_point(s, path.current);
    }, "{CGPoint=dd}@:");
    method(bp, "containsPoint:", [path_of](CpuState& s, GuestMemory&) {
        const auto state = path_of(s.x[0]);
        return_bool(s, state->path.contains(point_arg(s), state->winding == 1));
    }, "B@:{CGPoint=dd}");
    method(bp, "elementCount", [path_of](CpuState& s, GuestMemory&) { s.x[0] = path_of(s.x[0])->path.elements.size(); }, "q@:");
    method(bp, "elementAtIndex:associatedPoints:", [this, path_of](CpuState& s, GuestMemory&) {
        const auto& elements = path_of(s.x[0])->path.elements;
        if (s.x[2] >= elements.size()) fail("NSRangeException: -[NSBezierPath elementAtIndex:] index out of bounds");
        const auto& element = elements[static_cast<std::size_t>(s.x[2])];
        // NSBezierPathElement: move 0, line 1, cubic 2, close 3, quadratic 4.
        std::uint64_t type{};
        switch (element.op) {
        case PathOp::move: type = 0; break;
        case PathOp::line: type = 1; break;
        case PathOp::cubic: type = 2; break;
        case PathOp::close: type = 3; break;
        case PathOp::quad: type = 4; break;
        }
        if (s.x[3])
            for (unsigned index = 0; index < element.count(); ++index) {
                write_double(s.x[3] + index * 16, element.points[index].x);
                write_double(s.x[3] + index * 16 + 8, element.points[index].y);
            }
        s.x[0] = type;
    }, "Q@:q^{CGPoint=dd}");
    method(bp, "elementAtIndex:", [path_of](CpuState& s, GuestMemory&) {
        const auto& elements = path_of(s.x[0])->path.elements;
        if (s.x[2] >= elements.size()) fail("NSRangeException: -[NSBezierPath elementAtIndex:] index out of bounds");
        const auto op = elements[static_cast<std::size_t>(s.x[2])].op;
        s.x[0] = op == PathOp::move ? 0 : op == PathOp::line ? 1 : op == PathOp::cubic ? 2 : op == PathOp::close ? 3 : 4;
    }, "Q@:q");
    const auto real_property = [&method, bp, path_of](const char* getter, const char* setter, double BezierPathState::*field) {
        method(bp, getter, [path_of, field](CpuState& s, GuestMemory&) { set_d(s, 0, (*path_of(s.x[0])).*field); }, "d@:");
        method(bp, setter, [path_of, field](CpuState& s, GuestMemory&) { (*path_of(s.x[0])).*field = d(s, 0); }, "v@:d");
    };
    real_property("lineWidth", "setLineWidth:", &BezierPathState::line_width);
    real_property("miterLimit", "setMiterLimit:", &BezierPathState::miter_limit);
    real_property("flatness", "setFlatness:", &BezierPathState::flatness);
    const auto enum_property = [&method, bp, path_of](const char* getter, const char* setter, std::uint64_t BezierPathState::*field) {
        method(bp, getter, [path_of, field](CpuState& s, GuestMemory&) { s.x[0] = (*path_of(s.x[0])).*field; }, "Q@:");
        method(bp, setter, [path_of, field](CpuState& s, GuestMemory&) { (*path_of(s.x[0])).*field = s.x[2]; }, "v@:Q");
    };
    enum_property("lineCapStyle", "setLineCapStyle:", &BezierPathState::cap);
    enum_property("lineJoinStyle", "setLineJoinStyle:", &BezierPathState::join);
    enum_property("windingRule", "setWindingRule:", &BezierPathState::winding);
    method(bp, "setLineDash:count:phase:", [this, path_of](CpuState& s, GuestMemory&) {
        const auto state = path_of(s.x[0]);
        state->dashes.clear();
        for (Address index = 0; index < s.x[3] && s.x[2]; ++index) state->dashes.push_back(read_double(s.x[2] + index * 8));
        state->phase = d(s, 0);
    }, "v@:^dqd");
    method(bp, "getLineDash:count:phase:", [this, path_of](CpuState& s, GuestMemory& m) {
        const auto state = path_of(s.x[0]);
        if (s.x[2]) for (std::size_t index = 0; index < state->dashes.size(); ++index) write_double(s.x[2] + index * 8, state->dashes[index]);
        if (s.x[3]) m.write64(s.x[3], state->dashes.size());
        if (s.x[4]) write_double(s.x[4], state->phase);
    }, "v@:^d^q^d");
    method(bp, "copyWithZone:", [q, bp, path_of](CpuState& s, GuestMemory&) {
        const auto object = q->objc.allocate(bp);
        *path_of(object) = *path_of(s.x[0]);
        s.x[0] = object;
    }, "@@:^v");
    method(bp, "CGPath", [q, path_of](CpuState& s, GuestMemory&) { s.x[0] = q->autorelease(q->make_path(path_of(s.x[0])->path, false)); }, "^v@:");
    method(bp, "bezierPathByReversingPath", [new_path, path_of](CpuState& s, GuestMemory&) {
        const auto source = path_of(s.x[0])->path;
        s.x[0] = new_path([&](PathData& out) {
            // Reverse each subpath.
            std::vector<std::vector<PathElement>> subpaths;
            for (const auto& element : source.elements) {
                if (element.op == PathOp::move || subpaths.empty()) subpaths.emplace_back();
                subpaths.back().push_back(element);
            }
            for (const auto& sub : subpaths) {
                if (sub.empty()) continue;
                const bool closed = sub.back().op == PathOp::close;
                std::vector<Point> ends;
                for (const auto& e : sub) if (e.op != PathOp::close) ends.push_back(e.points[e.count() - 1]);
                out.move_to(ends.back());
                for (std::size_t index = sub.size(); index-- > 1;) {
                    const auto& e = sub[index];
                    if (e.op == PathOp::close) continue;
                    const auto target = ends[std::min(index - 1, ends.size() - 1)];
                    if (e.op == PathOp::line) out.line_to(target);
                    else if (e.op == PathOp::quad) out.quad_to(e.points[0], target);
                    else if (e.op == PathOp::cubic) out.cubic_to(e.points[1], e.points[0], target);
                }
                if (closed) out.close();
            }
        });
    });
    method(bp, "transformUsingAffineTransform:", [this, path_of](CpuState& s, GuestMemory&) {
        // NSAffineTransformStruct (six doubles) through -transformStruct (returned via x8).
        if (!s.x[2]) return;
        Scratch buffer(*this, 48);
        CallRegs registers;
        registers.indirect_result = buffer.address();
        static_cast<void>(send(s.x[2], "transformStruct", registers));
        const auto state = path_of(s.x[0]);
        state->path = state->path.transformed(read_affine(buffer.address()));
    }, "v@:@");

    // ---- NSImage / NSImageRep drawing ----
    const auto image_class = objc.find_class("NSImage");
    const auto image_rep_class = objc.find_class("NSImageRep");
    const auto bitmap_class = objc.find_class("NSBitmapImageRep");
    if (image_class && bitmap_class) {
        // +1 empty 8-bit RGBA NSBitmapImageRep of `pixels`, `points` in size.
        const auto new_bitmap_rep = [q, bitmap_class](std::uint64_t width, std::uint64_t height, Size points) {
            CallRegs registers;
            registers.x[2] = 0; registers.x[3] = width; registers.x[4] = height; registers.x[5] = 8; registers.x[6] = 4; registers.x[7] = 1;
            registers.push_stack(std::uint8_t{0});
            registers.push_stack(q->cf.constant_string("NSCalibratedRGBColorSpace"));
            registers.push_stack(std::int64_t{0});
            registers.push_stack(std::int64_t{0});
            const auto rep = q->send(q->objc.allocate(bitmap_class),
                          "initWithBitmapDataPlanes:pixelsWide:pixelsHigh:bitsPerSample:samplesPerPixel:hasAlpha:isPlanar:colorSpaceName:bytesPerRow:bitsPerPixel:",
                          registers).x0;
            if (!rep) fail("NSBitmapImageRep creation failed");
            CallRegs size_registers;
            size_registers.set_d(0, points.width);
            size_registers.set_d(1, points.height);
            static_cast<void>(q->send(rep, "setSize:", size_registers));
            return rep;
        };
        // +1 CGImage of an image drawn only by a custom representation: its
        // handler runs now, into a bitmap at the current context's scale
        // (as AppKit draws such images on demand, at the destination's
        // resolution).
        const auto render_custom = [q, new_bitmap_rep](Address image) -> Address {
            Address rep{};
            for (const auto candidate : array_items(*q, q->send_id(image, "representations")))
                if (custom_rep(candidate)) { rep = candidate; break; }
            if (!rep) return 0;
            const auto info = custom_rep(rep);
            const auto size = result_size(q->send(image, "size"));
            if (!(size.width > 0) || !(size.height > 0)) return 0;
            double scale = 1;
            if (const auto c = current_state(*q)) {
                std::lock_guard lock(c->mutex);
                scale = std::sqrt(std::abs(c->g.ctm.a * c->g.ctm.d - c->g.ctm.b * c->g.ctm.c));
            }
            scale = std::clamp(std::isfinite(scale) ? scale : 1.0, 1.0, 4.0);
            const auto width = static_cast<std::uint64_t>(std::ceil(size.width * scale));
            const auto height = static_cast<std::uint64_t>(std::ceil(size.height * scale));
            const auto bitmap = new_bitmap_rep(width, height, size);
            const auto cg = context_for_rep(*q, bitmap);
            if (!cg) { q->release(bitmap); fail("NSCustomImageRep: no bitmap context"); }
            if (info->flipped) {
                const auto c = context_state(*q, cg);
                c->g.ctm = Affine::translation(0, size.height).then(Affine::scaling(1, -1)).then(c->g.ctm);
            }
            const auto context = make_graphics_context(*q, cg, info->flipped, bitmap);
            q->release(cg);
            const auto previous = q->retain(graphics.current);
            set_current(*q, context);
            const auto restore = [&] {
                set_current(*q, previous);
                q->release(previous);
                q->release(context);
            };
            try { static_cast<void>(run_custom_rep(*q, rep, {{0, 0}, size})); } catch (...) { restore(); q->release(bitmap); throw; }
            restore();
            const auto result = image_from_rep(*q, bitmap);
            q->release(bitmap);
            return result;
        };
        const auto cg_image_of = [q, render_custom](Address image) -> Address {
            const auto rep = bitmap_rep_of(*q, image);
            return rep ? image_from_rep(*q, rep) : render_custom(image);
        };
        const auto draw_image_object = [q, cg_image_of](Address image, Rect rect, Rect from, std::uint64_t operation, double fraction, bool respect) {
            const auto cg = cg_image_of(image);
            if (!cg) return;
            const auto size = result_size(q->send(image, "size"));
            try { draw_cg_image(*q, cg, size, rect, from, operation, fraction, respect); } catch (...) { q->release(cg); throw; }
            q->release(cg);
        };
        method(image_class, "drawInRect:fromRect:operation:fraction:", [draw_image_object](CpuState& s, GuestMemory& m) {
            // rect d0-d3, fromRect d4-d7, operation x2; fraction is past v7: on the stack.
            StackReader stack{s, m};
            draw_image_object(s.x[0], rect_arg(s), rect_arg(s, 4), s.x[2], stack.next<double>(), false);
        }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}{CGRect={CGPoint=dd}{CGSize=dd}}Qd");
        method(image_class, "drawInRect:", [q, draw_image_object](CpuState& s, GuestMemory&) {
            draw_image_object(s.x[0], rect_arg(s), {}, 2, 1.0, true);
            static_cast<void>(q);
        }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
        method(image_class, "drawAtPoint:fromRect:operation:fraction:", [q, draw_image_object](CpuState& s, GuestMemory&) {
            // (point d0 d1, fromRect d2-d5, operation x2, fraction d6)
            const auto from = rect_arg(s, 2);
            const auto size = result_size(q->send(s.x[0], "size"));
            const Size drawn = rect_is_empty(from) ? size : from.size;
            draw_image_object(s.x[0], {point_arg(s), drawn}, from, s.x[2], d(s, 6), false);
        }, "v@:{CGPoint=dd}{CGRect={CGPoint=dd}{CGSize=dd}}Qd");
        method(image_class, "drawInRect:fromRect:operation:fraction:respectFlipped:hints:", [draw_image_object](CpuState& s, GuestMemory& m) {
            // rect d0-d3, fromRect d4-d7, operation x2, fraction (stack, double), respectFlipped x3, hints x4
            StackReader stack{s, m};
            const auto fraction = stack.next<double>();
            draw_image_object(s.x[0], rect_arg(s), rect_arg(s, 4), s.x[2], fraction, bool_arg(s, 3));
        }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}{CGRect={CGPoint=dd}{CGSize=dd}}QdB@");
        method(image_class, "CGImageForProposedRect:context:hints:", [q, cg_image_of](CpuState& s, GuestMemory&) {
            const auto image = cg_image_of(s.x[0]);
            if (image && s.x[2]) {
                const auto size = result_size(q->send(s.x[0], "size"));
                q->write_double(s.x[2], 0); q->write_double(s.x[2] + 8, 0);
                q->write_double(s.x[2] + 16, size.width); q->write_double(s.x[2] + 24, size.height);
            }
            s.x[0] = q->autorelease(image);
        }, "^v@:^{CGRect={CGPoint=dd}{CGSize=dd}}@@");
        // NSImage from a CGImage: one bitmap representation.
        const auto rep_from_cg = [q, bitmap_class](Address rep, Address cg) -> Address {
            const auto info = q->require<ImageInfo>(cg, "CGImage");
            const auto pixels = q->image_pixels(cg);
            CallRegs registers;
            registers.x[2] = 0;
            registers.x[3] = pixels->width;
            registers.x[4] = pixels->height;
            registers.x[5] = 8;
            registers.x[6] = 4;
            registers.x[7] = 1;
            registers.push_stack(std::uint8_t{0});                                  // isPlanar
            registers.push_stack(q->cf.constant_string("NSCalibratedRGBColorSpace"));  // colorSpaceName
            registers.push_stack(std::uint64_t{2});                                 // bitmapFormat: non-premultiplied
            registers.push_stack(std::int64_t{static_cast<std::int64_t>(pixels->width) * 4});
            registers.push_stack(std::int64_t{32});
            const auto initialized = q->send(rep, "initWithBitmapDataPlanes:pixelsWide:pixelsHigh:bitsPerSample:samplesPerPixel:hasAlpha:isPlanar:colorSpaceName:bitmapFormat:bytesPerRow:bitsPerPixel:", registers).x0;
            if (!initialized) return 0;
            const auto data = q->send_id(initialized, "bitmapData");
            std::vector<std::uint8_t> rgba(pixels->pixels.size());
            for (std::size_t at = 0; at < rgba.size(); at += 4) {
                const auto a = pixels->pixels[at + 3];
                const auto un = [a](std::uint8_t v) { return a ? static_cast<std::uint8_t>(std::min(255, (v * 255 + a / 2) / a)) : std::uint8_t{0}; };
                rgba[at] = un(pixels->pixels[at + 2]); rgba[at + 1] = un(pixels->pixels[at + 1]); rgba[at + 2] = un(pixels->pixels[at]); rgba[at + 3] = a;
            }
            q->write(data, rgba);
            static_cast<void>(info);
            static_cast<void>(bitmap_class);
            return initialized;
        };
        method(bitmap_class, "initWithCGImage:", [rep_from_cg](CpuState& s, GuestMemory&) { s.x[0] = s.x[2] ? rep_from_cg(s.x[0], s.x[2]) : 0; });
        method(bitmap_class, "CGImage", [q](CpuState& s, GuestMemory&) { s.x[0] = q->autorelease(image_from_rep(*q, s.x[0])); }, "^v@:");
        method(image_class, "initWithCGImage:size:", [q, bitmap_class, rep_from_cg](CpuState& s, GuestMemory&) {
            // (image, size d0 d1): NSZeroSize means the image's pixel size.
            if (!s.x[2]) { q->release(s.x[0]); s.x[0] = 0; return; }
            const auto rep = rep_from_cg(q->objc.allocate(bitmap_class), s.x[2]);
            if (!rep) { q->release(s.x[0]); s.x[0] = 0; return; }
            auto size = size_arg(s);
            if (size.width <= 0 || size.height <= 0) {
                const auto info = q->require<ImageInfo>(s.x[2], "CGImage");
                size = {static_cast<double>(info->desc.layout.width), static_cast<double>(info->desc.layout.height)};
            }
            CallRegs registers;
            registers.set_d(0, size.width);
            registers.set_d(1, size.height);
            const auto self = q->send(s.x[0], "initWithSize:", registers).x0;
            static_cast<void>(q->send(rep, "setSize:", registers));
            static_cast<void>(q->send_id(self ? self : s.x[0], "addRepresentation:", {rep}));
            q->release(rep);
            s.x[0] = self ? self : s.x[0];
        }, "@@:^v{CGSize=dd}");
        // -lockFocus: draw into the first bitmap representation (made at 1x when absent).
        const auto lock_focus = [q, bitmap_class](Address image, bool flipped) {
            auto rep = bitmap_rep_of(*q, image);
            const auto size = result_size(q->send(image, "size"));
            if (!rep) {
                const auto width = static_cast<std::uint64_t>(std::max(1.0, std::ceil(size.width)));
                const auto height = static_cast<std::uint64_t>(std::max(1.0, std::ceil(size.height)));
                CallRegs registers;
                registers.x[2] = 0; registers.x[3] = width; registers.x[4] = height; registers.x[5] = 8; registers.x[6] = 4; registers.x[7] = 1;
                registers.push_stack(std::uint8_t{0});
                registers.push_stack(q->cf.constant_string("NSCalibratedRGBColorSpace"));
                registers.push_stack(std::int64_t{0});
                registers.push_stack(std::int64_t{0});
                rep = q->send(q->objc.allocate(bitmap_class),
                              "initWithBitmapDataPlanes:pixelsWide:pixelsHigh:bitsPerSample:samplesPerPixel:hasAlpha:isPlanar:colorSpaceName:bytesPerRow:bitsPerPixel:",
                              registers).x0;
                if (!rep) fail("-[NSImage lockFocus]: creating the bitmap representation");
                CallRegs size_registers;
                size_registers.set_d(0, size.width);
                size_registers.set_d(1, size.height);
                static_cast<void>(q->send(rep, "setSize:", size_registers));
                static_cast<void>(q->send_id(image, "addRepresentation:", {rep}));
                q->release(rep);
            }
            const auto cg = context_for_rep(*q, rep);
            if (!cg) unsupported("-[NSImage lockFocus] on this bitmap representation format");
            if (flipped) {
                const auto c = context_state(*q, cg);
                c->g.ctm = Affine::translation(0, size.height).then(Affine::scaling(1, -1)).then(c->g.ctm);
            }
            const auto context = make_graphics_context(*q, cg, flipped, rep);
            q->release(cg);
            graphics.focus.push_back({q->retain(image), q->retain(graphics.current)});
            set_current(*q, context);
            q->release(context);
        };
        method(image_class, "lockFocus", [lock_focus](CpuState& s, GuestMemory&) { lock_focus(s.x[0], false); }, "v@:");
        method(image_class, "lockFocusFlipped:", [lock_focus](CpuState& s, GuestMemory&) { lock_focus(s.x[0], bool_arg(s, 2)); }, "v@:B");
        method(image_class, "unlockFocus", [q](CpuState& s, GuestMemory&) {
            if (graphics.focus.empty() || graphics.focus.back().image != s.x[0]) fail("-[NSImage unlockFocus] without -lockFocus");
            const auto focus = graphics.focus.back();
            graphics.focus.pop_back();
            set_current(*q, focus.context);
            q->release(focus.context);
            q->release(focus.image);
        }, "v@:");
        if (image_rep_class) {
            const auto custom = declare_class("NSCustomImageRep", "NSImageRep", appkit_library);
            const auto block_function = [q](const char* symbol) {
                const auto found = q->process.resolve(libsystem_library, symbol);
                if (!found) fail(std::string("missing ") + symbol);
                return found->address;
            };
            method(custom, "initWithSize:flipped:drawingHandler:", [q, block_function](CpuState& s, GuestMemory&) {
                // size d0 d1, flipped x2, handler x3
                if (!s.x[3]) fail("-[NSCustomImageRep initWithSize:flipped:drawingHandler:] with a nil handler");
                CallRegs size_registers;
                size_registers.set_d(0, d(s, 0));
                size_registers.set_d(1, d(s, 1));
                static_cast<void>(q->send(s.x[0], "setSize:", size_registers));
                const auto handler = q->call_guest(block_function("__Block_copy"), {s.x[3]});
                std::lock_guard lock(custom_mutex);
                custom_reps[s.x[0]] = {handler, 0, 0, bool_arg(s, 2)};
            }, "@@:{CGSize=dd}B@?");
            method(custom, "initWithDrawSelector:delegate:", [](CpuState& s, GuestMemory&) {
                std::lock_guard lock(custom_mutex);
                custom_reps[s.x[0]] = {0, s.x[3], s.x[2], false};
            }, "@@::@");
            method(custom, "drawingHandler", [](CpuState& s, GuestMemory&) {
                const auto info = custom_rep(s.x[0]);
                s.x[0] = info ? info->handler : 0;
            }, "@?@:");
            method(custom, "delegate", [](CpuState& s, GuestMemory&) { const auto info = custom_rep(s.x[0]); s.x[0] = info ? info->delegate : 0; }, "@@:");
            method(custom, "drawSelector", [](CpuState& s, GuestMemory&) { const auto info = custom_rep(s.x[0]); s.x[0] = info ? info->selector : 0; }, ":@:");
            method(custom, "dealloc", [q, custom, block_function](CpuState& s, GuestMemory&) {
                std::optional<CustomRep> info;
                {
                    std::lock_guard lock(custom_mutex);
                    if (const auto found = custom_reps.find(s.x[0]); found != custom_reps.end()) { info = found->second; custom_reps.erase(found); }
                }
                if (info && info->handler) static_cast<void>(q->call_guest(block_function("__Block_release"), {info->handler}));
                static_cast<void>(q->objc.send_super(s.x[0], custom, q->objc.selector("dealloc")));
            }, "v@:");
            // Drawing into the current context: the source draws the rep's
            // bounds; drawInRect: / drawAtPoint: map them onto the target.
            const auto draw_custom = [q](Address rep, std::optional<Rect> target) {
                const auto size = result_size(q->send(rep, "size"));
                const auto info = custom_rep(rep);
                const auto c = current_state(*q);
                if (!info || !c) return false;
                {
                    std::lock_guard lock(c->mutex);
                    save_state(*c);
                    auto m = Affine{};
                    if (target) {
                        const auto sx = size.width > 0 ? target->size.width / size.width : 1.0;
                        const auto sy = size.height > 0 ? target->size.height / size.height : 1.0;
                        m = Affine::scaling(sx, sy).then(Affine::translation(target->origin.x, target->origin.y));
                    }
                    if (info->flipped != current_flipped(*q))
                        m = Affine::translation(0, size.height).then(Affine::scaling(1, -1)).then(m);
                    c->g.ctm = m.then(c->g.ctm);
                }
                bool result{};
                try { result = run_custom_rep(*q, rep, {{0, 0}, size}); } catch (...) {
                    std::lock_guard lock(c->mutex);
                    restore_state(*q, *c);
                    throw;
                }
                std::lock_guard lock(c->mutex);
                restore_state(*q, *c);
                return result;
            };
            method(custom, "draw", [draw_custom](CpuState& s, GuestMemory&) { return_bool(s, draw_custom(s.x[0], std::nullopt)); }, "B@:");
            method(custom, "drawInRect:", [draw_custom](CpuState& s, GuestMemory&) { return_bool(s, draw_custom(s.x[0], rect_arg(s))); },
                   "B@:{CGRect={CGPoint=dd}{CGSize=dd}}");
            method(custom, "drawAtPoint:", [q, draw_custom](CpuState& s, GuestMemory&) {
                return_bool(s, draw_custom(s.x[0], Rect{point_arg(s), result_size(q->send(s.x[0], "size"))}));
            }, "B@:{CGPoint=dd}");
            class_method(image_class, "imageWithSize:flipped:drawingHandler:", [q, image_class, custom](CpuState& s, GuestMemory&) {
                CallRegs registers;
                registers.set_d(0, d(s, 0));
                registers.set_d(1, d(s, 1));
                const auto image = q->send(q->objc.allocate(image_class), "initWithSize:", registers).x0;
                registers.x[2] = s.x[2] & 0xff;
                registers.x[3] = s.x[3];
                const auto rep = q->send(q->objc.allocate(custom), "initWithSize:flipped:drawingHandler:", registers).x0;
                static_cast<void>(q->send_id(image, "addRepresentation:", {rep}));
                q->release(rep);
                s.x[0] = q->autorelease(image);
            }, "@@:{CGSize=dd}B@?");

            const auto draw_rep = [q](Address rep, Rect rect, Rect from, std::uint64_t operation, double fraction, bool respect) {
                const auto cg = image_from_rep(*q, rep);
                if (!cg) return false;
                const auto size = result_size(q->send(rep, "size"));
                try { draw_cg_image(*q, cg, size, rect, from, operation, fraction, respect); } catch (...) { q->release(cg); throw; }
                q->release(cg);
                return true;
            };
            method(image_rep_class, "draw", [q, draw_rep](CpuState& s, GuestMemory&) {
                return_bool(s, draw_rep(s.x[0], {{0, 0}, result_size(q->send(s.x[0], "size"))}, {}, 2, 1, false));
            }, "B@:");
            method(image_rep_class, "drawAtPoint:", [q, draw_rep](CpuState& s, GuestMemory&) {
                return_bool(s, draw_rep(s.x[0], {point_arg(s), result_size(q->send(s.x[0], "size"))}, {}, 2, 1, false));
            }, "B@:{CGPoint=dd}");
            method(image_rep_class, "drawInRect:", [draw_rep](CpuState& s, GuestMemory&) { return_bool(s, draw_rep(s.x[0], rect_arg(s), {}, 2, 1, false)); },
                   "B@:{CGRect={CGPoint=dd}{CGSize=dd}}");
            method(image_rep_class, "drawInRect:fromRect:operation:fraction:respectFlipped:hints:", [draw_rep](CpuState& s, GuestMemory& m) {
                StackReader stack{s, m};
                const auto fraction = stack.next<double>();
                return_bool(s, draw_rep(s.x[0], rect_arg(s), rect_arg(s, 4), s.x[2], fraction, bool_arg(s, 3)));
            }, "B@:{CGRect={CGPoint=dd}{CGSize=dd}}{CGRect={CGPoint=dd}{CGSize=dd}}QdB@");
        }
    }

    // ---- NSString / NSAttributedString drawing (AppKit additions) ----
    const auto string_text = [q](Address string, Address attributes) { return plain_text(*q, string, attributes); };
    const auto size_of = [q](AttributedText& text) {
        const auto m = measure(*q, text, 0, false);
        release_text(*q, text);
        return Size{m.width, m.height};
    };
    const auto bounding = [q](AttributedText& text, Size size, std::uint64_t options) -> Rect {
        const bool fragments = options & uses_line_fragment_origin;
        const auto m = measure(*q, text, size.width, fragments);
        release_text(*q, text);
        if (fragments) return {{0, 0}, {m.width, m.height}};
        return {{0, -m.last_descent}, {m.width, m.height}};
    };
    const auto draw_at = [q](AttributedText& text, Point point) {
        const auto m = measure(*q, text, 0, false);
        // Unflipped: the point is the bottom-left of the text; flipped: the top-left.
        const Rect box = current_flipped(*q) ? Rect{point, {m.width + 1, m.height}} : Rect{{point.x, point.y}, {m.width + 1, m.height}};
        try { draw_text(*q, text, box, false, false); } catch (...) { release_text(*q, text); throw; }
        release_text(*q, text);
    };
    const auto draw_in = [q](AttributedText& text, Rect rect, bool wrap) {
        try { draw_text(*q, text, standardize(rect), wrap, true); } catch (...) { release_text(*q, text); throw; }
        release_text(*q, text);
    };
    if (const auto string_class = objc.find_class("NSString")) {
        method(string_class, "sizeWithAttributes:", [string_text, size_of](CpuState& s, GuestMemory&) {
            auto text = string_text(s.x[0], s.x[2]);
            return_size(s, size_of(text));
        }, "{CGSize=dd}@:@");
        method(string_class, "drawAtPoint:withAttributes:", [string_text, draw_at](CpuState& s, GuestMemory&) {
            auto text = string_text(s.x[0], s.x[2]);
            draw_at(text, point_arg(s));
        }, "v@:{CGPoint=dd}@");
        method(string_class, "drawInRect:withAttributes:", [string_text, draw_in](CpuState& s, GuestMemory&) {
            auto text = string_text(s.x[0], s.x[2]);
            draw_in(text, rect_arg(s), true);
        }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}@");
        for (const auto* name : {"drawWithRect:options:attributes:", "drawWithRect:options:attributes:context:"})
            method(string_class, name, [string_text, draw_in](CpuState& s, GuestMemory&) {
                auto text = string_text(s.x[0], s.x[3]);
                draw_in(text, rect_arg(s), (s.x[2] & uses_line_fragment_origin) != 0);
            }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}Q@@");
        for (const auto* name : {"boundingRectWithSize:options:attributes:", "boundingRectWithSize:options:attributes:context:"})
            method(string_class, name, [string_text, bounding](CpuState& s, GuestMemory&) {
                auto text = string_text(s.x[0], s.x[3]);
                return_rect(s, bounding(text, size_arg(s), s.x[2]));
            }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:{CGSize=dd}Q@@");
    }
    const auto attributed = coretext().attributed_class;
    const auto attributed_of = [q](Address object) { return attributed_text(*q, object); };
    method(attributed, "size", [attributed_of, size_of](CpuState& s, GuestMemory&) {
        auto text = attributed_of(s.x[0]);
        return_size(s, size_of(text));
    }, "{CGSize=dd}@:");
    method(attributed, "drawAtPoint:", [attributed_of, draw_at](CpuState& s, GuestMemory&) {
        auto text = attributed_of(s.x[0]);
        draw_at(text, point_arg(s));
    }, "v@:{CGPoint=dd}");
    method(attributed, "drawInRect:", [attributed_of, draw_in](CpuState& s, GuestMemory&) {
        auto text = attributed_of(s.x[0]);
        draw_in(text, rect_arg(s), true);
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    for (const auto* name : {"drawWithRect:options:", "drawWithRect:options:context:"})
        method(attributed, name, [attributed_of, draw_in](CpuState& s, GuestMemory&) {
            auto text = attributed_of(s.x[0]);
            draw_in(text, rect_arg(s), (s.x[2] & uses_line_fragment_origin) != 0);
        }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}Q@");
    for (const auto* name : {"boundingRectWithSize:options:", "boundingRectWithSize:options:context:"})
        method(attributed, name, [attributed_of, bounding](CpuState& s, GuestMemory&) {
            auto text = attributed_of(s.x[0]);
            return_rect(s, bounding(text, size_arg(s), s.x[2]));
        }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:{CGSize=dd}Q@");
    static_cast<void>(truncates_last_visible_line);

    // ---- NSView bitmap caching ----
    if (const auto view = objc.find_class("NSView")) {
        method(view, "bitmapImageRepForCachingDisplayInRect:", [q, bitmap_class](CpuState& s, GuestMemory&) {
            const auto rect = standardize(rect_arg(s));
            const auto width = static_cast<std::uint64_t>(std::max(1.0, std::ceil(rect.size.width)));
            const auto height = static_cast<std::uint64_t>(std::max(1.0, std::ceil(rect.size.height)));
            CallRegs registers;
            registers.x[2] = 0; registers.x[3] = width; registers.x[4] = height; registers.x[5] = 8; registers.x[6] = 4; registers.x[7] = 1;
            registers.push_stack(std::uint8_t{0});
            registers.push_stack(q->cf.constant_string("NSCalibratedRGBColorSpace"));
            registers.push_stack(std::int64_t{0});
            registers.push_stack(std::int64_t{0});
            const auto rep = q->send(q->objc.allocate(bitmap_class),
                                     "initWithBitmapDataPlanes:pixelsWide:pixelsHigh:bitsPerSample:samplesPerPixel:hasAlpha:isPlanar:colorSpaceName:bytesPerRow:bitsPerPixel:",
                                     registers).x0;
            s.x[0] = q->autorelease(rep);
        }, "@@:{CGRect={CGPoint=dd}{CGSize=dd}}");
        method(view, "cacheDisplayInRect:toBitmapImageRep:", [q](CpuState& s, GuestMemory&) {
            // rect d0-d3 (view bounds coordinates), rep x2
            const auto rect = standardize(rect_arg(s));
            if (!s.x[2]) return;
            const auto cg = context_for_rep(*q, s.x[2]);
            if (!cg) unsupported("-[NSView cacheDisplayInRect:toBitmapImageRep:] on this bitmap representation format");
            try {
                const auto c = context_state(*q, cg);
                // The rep's point size is the cached rectangle's.
                const auto pixels_wide = static_cast<double>(c->layout.width), pixels_high = static_cast<double>(c->layout.height);
                c->base_ctm = Affine::scaling(pixels_wide / std::max(1.0, rect.size.width), pixels_high / std::max(1.0, rect.size.height));
                c->g.ctm = c->base_ctm;
                const bool flipped = (q->send(s.x[0], "isFlipped").x0 & 0xff) != 0;
                if (flipped) c->g.ctm = Affine::translation(0, rect.size.height).then(Affine::scaling(1, -1)).then(c->g.ctm);
                c->g.ctm = Affine::translation(-rect.origin.x, -rect.origin.y).then(c->g.ctm);
                // The cached area starts transparent.
                clear_rect(*q, *c, rect);
                draw_view(*q, s.x[0], cg, false, flipped);
            } catch (...) { q->release(cg); throw; }
            q->release(cg);
        }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}@");
        method(view, "displayRectIgnoringOpacity:inContext:", [q](CpuState& s, GuestMemory&) {
            if (!s.x[2]) return;
            const auto state = q->find<GraphicsContextState>(s.x[2]);
            if (!state || !state->cg) return;
            const bool flipped = (q->send(s.x[0], "isFlipped").x0 & 0xff) != 0;
            draw_view(*q, s.x[0], state->cg, false, flipped);
        }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}@");
    }
}

} // namespace alchemy::gui::quartz2d
